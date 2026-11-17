/*
This file is part of LuminaGram,
a fork of Telegram Desktop.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "lumina/lumina_fake_crash.h"

#include "base/call_delayed.h"
#include "base/weak_ptr.h"
#include "core/application.h"
#include "lumina/lumina_settings.h"
#include "lumina/lumina_vault.h"
#include "main/main_account.h"
#include "main/main_domain.h"
#include "storage/localstorage.h"

#include <QtCore/QEventLoop>
#include <QtCore/QPointer>
#include <QtGui/QGuiApplication>
#include <QtWidgets/QApplication>
#include <QtWidgets/QWidget>

#include <cstdlib>
#include <optional>
#include <vector>

namespace Lumina {
namespace {

// Same key names as Android's LuminaConfig (KEY_FAKE_CRASH_*), so the two
// platforms' stores can be read against each other.
const auto kKeyEnabled = u"fakeCrashEnabled"_q;
const auto kKeyCode = u"fakeCrashCode"_q;
const auto kKeyMode = u"fakeCrashMode"_q;
const auto kModeExit = u"exit"_q;
const auto kModeLogoutCurrent = u"logoutCurrent"_q;
const auto kModeLogoutAll = u"logoutAll"_q;

// The fake-crash alert this feature used to offer. Only ever read, to move
// its users to Exit; never written.
const auto kModeLegacyDialog = u"dialog"_q;

// A logout recorded where it could not be carried out (the vault gate, or the
// lock screen of a LuminaGram that has not opened its accounts yet), holding
// the mode that recorded it. Cleared the moment it starts.
const auto kKeyPending = u"duressPendingLogout"_q;

// How long a server-side auth.logOut is given before the local logout goes
// ahead anyway, same as the panic wipe (lumina_panic_wipe.cpp): the request
// has no timeout of its own and simply waits while there is no network.
constexpr auto kForcedLogOutTimeout = crl::time(5000);

// How often the logout in flight is checked for having finished.
constexpr auto kPollInterval = crl::time(100);

// After the logout, before the process ends: lets the asynchronous parts of
// the local wipe (file removal, cache clearing - each on its own thread) get
// on with it, then the synchronous writes are flushed.
constexpr auto kSettleBeforeExit = crl::time(1000);

[[nodiscard]] QString ModeKey(DuressMode mode) {
	switch (mode) {
	case DuressMode::LogoutCurrent: return kModeLogoutCurrent;
	case DuressMode::LogoutAll: return kModeLogoutAll;
	case DuressMode::Exit: break;
	}
	return kModeExit;
}

[[nodiscard]] DuressMode ModeFromKey(const QString &key) {
	return (key == kModeLogoutCurrent)
		? DuressMode::LogoutCurrent
		: (key == kModeLogoutAll)
		? DuressMode::LogoutAll
		: DuressMode::Exit;
}

// The one duress logout this process can have in flight. Plain data: the
// timers are base::call_delayed() guarded by the application, so nothing
// here outlives the event loop.
struct Run {
	bool running = false;
	int generation = 0;
	std::vector<base::weak_ptr<Main::Account>> targets;
	std::vector<Fn<void()>> waiters;
};

[[nodiscard]] Run &State() {
	static auto result = Run();
	return result;
}

[[nodiscard]] rpl::lifetime &SetupLifetime() {
	static auto result = rpl::lifetime();
	return result;
}

// Every window goes away first, so the lock screen vanishes the moment the
// code is accepted, exactly as it did when the process ended on the spot.
//
// Qt quits when the last window goes away (quitOnLastWindowClosed() is Qt's
// default, true), and that quit is the graceful, vetoable one ExitAtOnce()
// exists to avoid - and it would also cut the logout short. So it is
// switched off before anything is hidden, and never switched back: the
// process ends right after the logout.
//
// Guarded pointers, because hiding one top-level widget can destroy another
// (a menu or tooltip owned by the window) while the list is being walked.
void HideAllWindows() {
	QGuiApplication::setQuitOnLastWindowClosed(false);
	auto widgets = std::vector<QPointer<QWidget>>();
	for (const auto widget : QApplication::topLevelWidgets()) {
		widgets.emplace_back(widget);
	}
	for (const auto &widget : widgets) {
		if (widget && widget->isVisible()) {
			widget->hide();
		}
	}
}

void RecordPending(DuressMode mode) {
	auto &settings = Settings::Instance();
	// A recorded "all" is never narrowed down to "current".
	if (ModeFromKey(settings.getString(kKeyPending)) == DuressMode::LogoutAll) {
		return;
	}
	settings.set(kKeyPending, ModeKey(mode), Store::Private);
	// Flushed at once: what records this is followed by an exit that drains
	// no timer, or by a decoy whose owner may simply kill the process.
	settings.saveNow();
}

[[nodiscard]] std::optional<DuressMode> TakePending() {
	auto &settings = Settings::Instance();
	const auto stored = settings.getString(kKeyPending);
	if (stored.isEmpty()) {
		return std::nullopt;
	}
	// Removed before anything is attempted, so a logout that crashes the
	// process is not repeated at every following start.
	settings.remove(kKeyPending);
	settings.saveNow();
	const auto mode = ModeFromKey(stored);
	return IsLogoutMode(mode) ? std::make_optional(mode) : std::nullopt;
}

void Finish(int generation) {
	auto &run = State();
	if (!run.running || run.generation != generation) {
		return;
	}
	run.running = false;
	run.targets.clear();
	for (const auto &waiter : base::take(run.waiters)) {
		if (waiter) {
			waiter();
		}
	}
}

[[nodiscard]] bool TargetsLoggedOut() {
	for (const auto &weak : State().targets) {
		if (const auto account = weak.get()) {
			if (account->sessionExists()) {
				return false;
			}
		}
	}
	return true;
}

void Poll(int generation) {
	if (State().generation != generation || !State().running) {
		return;
	} else if (TargetsLoggedOut()) {
		Finish(generation);
		return;
	}
	base::call_delayed(kPollInterval, &Core::App(), [=] {
		Poll(generation);
	});
}

// The fallback: logOut() waits for the server, so on a blocked or dead
// network the accounts would stay usable on this device indefinitely.
// forcedLogOut() finishes the logout locally and drops the authorization
// keys; it checks sessionExists() itself, so an account the server already
// let go of is left alone.
void ForceRemaining(int generation) {
	if (State().generation != generation || !State().running) {
		return;
	}
	const auto targets = State().targets;
	for (const auto &weak : targets) {
		if (const auto account = weak.get()) {
			account->forcedLogOut();
		}
	}
	Finish(generation);
}

// The logout itself. Always entered from a fresh turn of the event loop, never
// from inside the lock screen's callback or the domain's activation, because
// both of those can be torn down by what happens here.
void StartLogout(DuressMode mode, int generation) {
	auto &run = State();
	if (!run.running || run.generation != generation) {
		return;
	}
	const auto &domain = Core::App().domain();
	auto targets = std::vector<base::weak_ptr<Main::Account>>();
	if (domain.started()) {
		if (mode == DuressMode::LogoutAll) {
			for (const auto &[index, account] : domain.accounts()) {
				if (account->sessionExists()) {
					targets.push_back(base::make_weak(account.get()));
				}
			}
		} else if (domain.active().sessionExists()) {
			targets.push_back(base::make_weak(&domain.active()));
		}
	}
	run.targets = targets;

	if (mode == DuressMode::LogoutAll) {
		// Exactly what the passcode lock screen's own "Log out" does
		// (Window::Controller::showLogoutConfirmation() with no account):
		// every account logs out, and when the accounts were never opened
		// because the passcode was not entered yet, the local data of all of
		// them is reset instead - there are no keys to reach the server with.
		Core::App().logout(nullptr);
	} else {
		// Main::Account::logOut(), never Core::App().logoutWithChecks(): the
		// latter stops to ask about exports, uploads and downloads in
		// progress, and under duress every such box is a prompt in front of
		// the wrong person (same reasoning as the panic wipe).
		for (const auto &weak : targets) {
			if (const auto account = weak.get()) {
				account->logOut();
			}
		}
	}

	base::call_delayed(kForcedLogOutTimeout, &Core::App(), [=] {
		ForceRemaining(generation);
	});
	Poll(generation);
}

void BeginRun(DuressMode mode) {
	auto &run = State();
	if (run.running) {
		return;
	}
	run.running = true;
	const auto generation = ++run.generation;
	crl::on_main(&Core::App(), [=] {
		StartLogout(mode, generation);
	});
}

void ExitAfterLogout() {
	base::call_delayed(kSettleBeforeExit, &Core::App(), [] {
		Settings::Instance().saveNow();
		Local::sync();
		ExitAtOnce();
	});
}

// The app's accounts have just become available. A logout recorded at the
// vault gate or on a lock screen that could not reach the accounts is carried
// out now, before anything of them is on screen: right at startup the window
// is not shown yet (WaitForDuressLogout() holds it), and after the real
// passcode the lock screen holds its unlock (AfterDuressLogout()).
void AccountsAvailable() {
	if (const auto mode = TakePending()) {
		BeginRun(*mode);
	}
}

// Desktop has always read a missing mode as Exit, and the fake-crash alert
// is gone: whoever picked it is moved to Exit - enabled as before, never to a
// logout mode. Read-time mapping already does this; the stored value is
// rewritten so the stores stop claiming a mode that no longer exists.
void MigrateLegacyMode() {
	auto &settings = Settings::Instance();
	if (settings.getString(kKeyMode) == kModeLegacyDialog) {
		settings.set(kKeyMode, kModeExit, Store::Private);
		settings.saveNow();
	}
}

} // namespace

bool FakeCrashEnabled() {
	return Settings::Instance().getBool(kKeyEnabled, false);
}

// Flushed rather than left to the ~500ms coalescing timer, here and in
// SetFakeCrashCode() / SetDuressMode(). These are what the lock screen and
// the vault read, and ExitAtOnce() drains no timer: losing the write would
// silently leave the code unarmed - or armed in a mode the user just moved
// away from.
void SetFakeCrashEnabled(bool value) {
	auto &settings = Settings::Instance();
	settings.set(kKeyEnabled, value, Store::Private);
	if (value && settings.getString(kKeyMode).isEmpty()) {
		// Writes this platform's default down the first time the feature is
		// switched on, so a backup restored on Android - where a missing mode
		// meant the fake-crash alert - keeps meaning what it meant here.
		settings.set(kKeyMode, kModeExit, Store::Private);
	}
	settings.saveNow();
}

QString FakeCrashCode() {
	return Settings::Instance().getString(kKeyCode).trimmed();
}

void SetFakeCrashCode(const QString &code) {
	const auto trimmed = code.trimmed();
	if (trimmed.isEmpty()) {
		// Not set(key, {}, Store::Private): an empty string would keep the key
		// in the private file, and "the duress code is the empty string" is a
		// state FakeCrashCodeMatches() would have to special-case forever.
		Settings::Instance().remove(kKeyCode);
	} else {
		Settings::Instance().set(kKeyCode, trimmed, Store::Private);
	}
	Settings::Instance().saveNow();
}

DuressMode CurrentDuressMode() {
	return ModeFromKey(Settings::Instance().getString(kKeyMode));
}

void SetDuressMode(DuressMode mode) {
	Settings::Instance().set(kKeyMode, ModeKey(mode), Store::Private);
	Settings::Instance().saveNow();
}

bool IsLogoutMode(DuressMode mode) {
	return (mode == DuressMode::LogoutCurrent)
		|| (mode == DuressMode::LogoutAll);
}

bool FakeCrashArmed() {
	return FakeCrashEnabled() && !FakeCrashCode().isEmpty();
}

bool FakeCrashCodeMatches(const QString &entered) {
	// Both sides trimmed, because the stored code is trimmed when it is saved
	// and the lock screen field is masked. A code that silently fails to fire
	// because a space rode along with it fails in the one moment the feature
	// exists for, in front of the person who is watching; and trimming cannot
	// make it fire in place of a real unlock, because by the time this runs
	// the passcode has already been checked and rejected.
	const auto trimmed = entered.trimmed();
	if (trimmed.isEmpty() || !FakeCrashEnabled()) {
		return false;
	}
	const auto code = FakeCrashCode();
	return !code.isEmpty() && (trimmed == code);
}

bool IsDuressCode(const QString &value) {
	const auto code = FakeCrashCode();
	return !code.isEmpty() && (value.trimmed() == code);
}

[[noreturn]] void ExitAtOnce() {
	// std::_Exit, and nothing before it. Three things this deliberately does
	// NOT do, each of which was considered and rejected:
	//
	//  * Core::Quit(). It is vetoable. Core::Application::preventsQuit() pops
	//    a confirmation box when an export, an upload or a download is in
	//    flight and then returns without quitting, and readyToQuit() can
	//    defer the exit by three seconds. A duress control that answers "are
	//    you sure you want to stop this download?" in front of the person
	//    holding the computer is worse than no duress control at all.
	//
	//  * std::abort() or any other real fault. It would be caught by the crash
	//    handler, which writes a genuine dump; the NEXT launch would then open
	//    LastCrashedWindow offering to send a crash report, and on macOS the
	//    system would show its own "quit unexpectedly" dialog naming the app.
	//
	//  * a graceful shutdown. Saving state takes visible time and leaves the
	//    window on screen while it happens.
	//
	// _Exit() leaves the empty tdata/working marker behind, so the next launch
	// logs "the previous launch was not finished properly" and starts
	// normally. Anything the app had not flushed is lost; tdesktop writes
	// through temp-file-and-rename and survives this by design, and the
	// logout modes flush what they wrote before calling this. Nothing is
	// logged here on purpose: a log line saying a duress code was entered
	// would hand the whole feature to anyone who later reads the log.
	std::_Exit(0);
}

void RunDuressFromPasscodeLock() {
	const auto mode = CurrentDuressMode();
	if (!IsLogoutMode(mode)) {
		ExitAtOnce();
	}
	HideAllWindows();
	if (!Core::App().domain().started()
		&& (mode == DuressMode::LogoutCurrent)) {
		// The lock screen of a LuminaGram that was started with a passcode:
		// the accounts are still sealed with the passcode, so neither which
		// account is the current one nor its keys can be reached. Recorded,
		// and done the next time the real passcode opens them - before
		// anything is shown (AccountsAvailable()).
		RecordPending(mode);
		ExitAtOnce();
	}
	auto &run = State();
	run.waiters.push_back(ExitAfterLogout);
	if (!run.running) {
		BeginRun(mode);
	}
}

bool DuressCodeEnteredAtVault(const QString &entered) {
	const auto mode = CurrentDuressMode();
	if (!IsLogoutMode(mode)
		|| !FakeCrashCodeMatches(entered)
		|| VaultCodeMatches(entered)) {
		// Exit mode does nothing at the vault: the code is just a wrong code
		// there, as it always was. And the vault's own code always wins.
		return false;
	}
	RecordPending(mode);
	return true;
}

void SetupDuress() {
	MigrateLegacyMode();
	auto &lifetime = SetupLifetime();
	lifetime.destroy();
	Core::App().domain().activeValue(
	) | rpl::filter([](Main::Account *account) {
		return (account != nullptr) && Core::App().domain().started();
	}) | rpl::on_next([](Main::Account*) {
		AccountsAvailable();
	}, lifetime);
}

bool DuressLogoutRunning() {
	return State().running;
}

void AfterDuressLogout(Fn<void()> callback) {
	if (!callback) {
		return;
	} else if (!State().running) {
		callback();
		return;
	}
	State().waiters.push_back(std::move(callback));
}

void WaitForDuressLogout() {
	if (!State().running
		|| Core::Quitting()
		|| Core::App().passcodeLocked()) {
		return;
	}
	// A nested loop, the same way the vault gate runs before this point: the
	// first window is not shown until the logout is over, so the account
	// never appears while it is being logged out. Guarded by the loop itself,
	// in case the application quits out of it first.
	QEventLoop loop;
	AfterDuressLogout(crl::guard(&loop, [&loop] {
		loop.quit();
	}));
	if (State().running) {
		loop.exec();
	}
}

rpl::producer<> FakeCrashChanges() {
	return Settings::Instance().changes(
	) | rpl::filter([](const QString &key) {
		return (key == kKeyEnabled) || (key == kKeyCode) || (key == kKeyMode);
	}) | rpl::to_empty;
}

} // namespace Lumina
