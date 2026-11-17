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
#include "tray.h"

#include <QtCore/QEventLoop>
#include <QtCore/QJsonObject>
#include <QtCore/QJsonValue>
#include <QtCore/QPointer>
#include <QtGui/QGuiApplication>
#include <QtWidgets/QApplication>
#include <QtWidgets/QWidget>

#include <cstdlib>
#include <optional>
#include <vector>

namespace Lumina {
namespace {

// Android's LuminaDuress (KEY_MODE, KEY_CODE, MODE_*), so the two platforms'
// stores and backups mean the same thing.
const auto kKeyMode = u"duressMode"_q;
const auto kKeyCode = u"fakeCrashCode"_q;
const auto kModeOff = u"off"_q;
const auto kModeExit = u"exit"_q;
const auto kModeLogoutCurrent = u"logoutCurrent"_q;
const auto kModeLogoutAll = u"logoutAll"_q;

// The fake-crash era: an enabled flag, and a mode that was "exit" or the
// fake-crash alert ("dialog"). Only ever removed - at startup and from
// backups - never read, so whoever had the old feature on starts OFF.
const auto kLegacyKeyEnabled = u"fakeCrashEnabled"_q;
const auto kLegacyKeyMode = u"fakeCrashMode"_q;

// A logout recorded where it could not be carried out on the spot (the vault
// gate, or the lock screen of a LuminaGram that has not opened its accounts
// yet), holding the mode that recorded it. Cleared the moment it starts.
// Desktop only: Android logs out at the vault directly.
const auto kKeyPending = u"duressPendingLogout"_q;

// Lumina::Settings' reserved map of which key lives in which store
// (StoresKey() in lumina_settings.cpp), as it appears in a backup payload.
const auto kBackupStoresKey = u"_luminaStores"_q;

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

// Nothing for "off", for a missing value and for anything unrecognised, so no
// stored state can turn into a logout by accident.
[[nodiscard]] std::optional<DuressMode> ParseMode(const QString &key) {
	if (key == kModeExit) {
		return DuressMode::Exit;
	} else if (key == kModeLogoutCurrent) {
		return DuressMode::LogoutCurrent;
	} else if (key == kModeLogoutAll) {
		return DuressMode::LogoutAll;
	}
	return std::nullopt;
}

[[nodiscard]] std::optional<DuressMode> StoredMode() {
	return ParseMode(Settings::Instance().getString(kKeyMode));
}

[[nodiscard]] std::optional<DuressMode> PendingMode() {
	const auto mode = ParseMode(Settings::Instance().getString(kKeyPending));
	return (mode && IsLogoutMode(*mode)) ? mode : std::nullopt;
}

// The one duress logout this process can have in flight. Plain data: the
// timers are base::call_delayed() guarded by the application, so nothing
// here outlives the event loop.
struct Run {
	bool running = false;

	// Set once the process is going to end after a duress logout, and never
	// cleared: from then on nothing may show or announce itself.
	bool exiting = false;

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

[[nodiscard]] rpl::lifetime &ExitingLifetime() {
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

// The process ends after this logout. Everything of the app leaves the
// screen now - the windows and the tray icon - and stays off it: a logout
// that leaves another account active activates that one, and an account with
// a window of its own brings that window up (Main::Domain::activate(),
// Core::Application::showAccount()), so every switch of the active account
// is followed by hiding everything again, at once and once more on the next
// turn of the event loop in case it was shown after this ran. Notifications
// are off from here on (DuressSilencesNotifications()).
void EnterExiting() {
	auto &run = State();
	if (run.exiting) {
		return;
	}
	run.exiting = true;
	HideAllWindows();
	Core::App().tray().destroyIconForExit();
	Core::App().domain().activeChanges(
	) | rpl::to_empty | rpl::on_next([] {
		HideAllWindows();
		crl::on_main(&Core::App(), [] {
			HideAllWindows();
		});
	}, ExitingLifetime());
}

void RecordPending(DuressMode mode) {
	auto &settings = Settings::Instance();
	// A recorded "all" is never narrowed down to "current".
	if (PendingMode() == DuressMode::LogoutAll) {
		return;
	}
	settings.set(kKeyPending, ModeKey(mode), Store::Private);
	// Flushed at once: what records this is followed by an exit that drains
	// no timer, or by a decoy whose owner may simply kill the process.
	settings.saveNow();
}

[[nodiscard]] std::optional<DuressMode> TakePending() {
	auto &settings = Settings::Instance();
	if (settings.getString(kKeyPending).isEmpty()) {
		return std::nullopt;
	}
	const auto mode = PendingMode();
	// Removed before anything is attempted, so a logout that crashes the
	// process is not repeated at every following start.
	settings.remove(kKeyPending);
	settings.saveNow();
	return mode;
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

// What a graceful shutdown would have written, written now, then the end.
// Core::App().saveSettings() because the domain's last-logout handling
// changes app settings through the delayed save (passcode removal turns the
// system unlock off), and ExitAtOnce() drains no timer. Only once the domain
// has started: before that, a LuminaGram whose settings are still sealed by
// a legacy passcode has not read them yet, and writing would replace them
// with defaults (~Application() only ever writes on a requested save).
[[noreturn]] void FlushAndExit() {
	Settings::Instance().saveNow();
	if (Core::App().domain().started()) {
		Core::App().saveSettings();
	}
	Local::sync();
	ExitAtOnce();
}

void ExitAfterLogout() {
	base::call_delayed(kSettleBeforeExit, &Core::App(), [] {
		FlushAndExit();
	});
}

// The app's accounts have just become available. A logout recorded at the
// vault gate or on a lock screen that could not reach the accounts is carried
// out now, before anything of them is on screen: right at startup the window
// is not shown yet (FinishDuressStartup() holds it, or never shows it), and
// after the real passcode the lock screen holds its unlock
// (AfterDuressLogout()).
void AccountsAvailable() {
	if (const auto mode = TakePending()) {
		BeginRun(*mode);
	}
}

// A pending "log out all" while the passcode lock keeps the accounts sealed,
// after the decoy was closed. Nothing can reach the server without the
// accounts' keys, but the stock lock screen's "Log out" resets all local data
// in this very state, and so does this - at once, rather than waiting for a
// passcode that may never be typed. A pending "log out the current account"
// cannot be done this way (it would take every other account with it), so it
// keeps waiting for the passcode.
void StartPendingWhileSealed() {
	if (State().running
		|| Core::App().domain().started()
		|| (PendingMode() != DuressMode::LogoutAll)) {
		return;
	}
	if (const auto mode = TakePending()) {
		BeginRun(*mode);
	}
}

// A nested loop, the same way the vault gate runs before this point: the
// first window is not shown until the logout is over, so the account never
// appears while it is being logged out. Bounded: every run finishes within
// kForcedLogOutTimeout. Guarded by the loop itself, in case the application
// quits out of it first.
void WaitForLogout() {
	if (!State().running || Core::Quitting()) {
		return;
	}
	QEventLoop loop;
	AfterDuressLogout(crl::guard(&loop, [&loop] {
		loop.quit();
	}));
	if (State().running) {
		loop.exec();
	}
}

// The fake-crash era's keys go; the code stays. Nothing reads them any more,
// so this only stops the stores from carrying a state that no longer means
// anything - the new mode key is absent for these users, which reads as off.
void DropLegacyKeys() {
	auto &settings = Settings::Instance();
	const auto had = settings.getBool(kLegacyKeyEnabled)
		|| !settings.getString(kLegacyKeyMode).isEmpty();
	settings.remove(kLegacyKeyEnabled);
	settings.remove(kLegacyKeyMode);
	if (had) {
		settings.saveNow();
	}
}

} // namespace

bool FakeCrashEnabled() {
	return StoredMode().has_value();
}

// Flushed rather than left to the ~500ms coalescing timer, here and in
// SetFakeCrashCode() / SetDuressMode(). These are what the lock screen and
// the vault read, and ExitAtOnce() drains no timer: losing the write would
// silently leave the code unarmed - or armed in a mode the user just moved
// away from.
void SetFakeCrashEnabled(bool value) {
	auto &settings = Settings::Instance();
	if (!value) {
		settings.set(kKeyMode, kModeOff, Store::Private);
	} else if (!StoredMode()) {
		settings.set(kKeyMode, kModeExit, Store::Private);
	} else {
		return;
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
	return StoredMode().value_or(DuressMode::Exit);
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
	EnterExiting();
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

bool DuressLogoutPending() {
	return PendingMode().has_value();
}

void SetupDuress() {
	DropLegacyKeys();
	auto &lifetime = SetupLifetime();
	lifetime.destroy();
	Core::App().domain().activeValue(
	) | rpl::filter([](Main::Account *account) {
		return (account != nullptr) && Core::App().domain().started();
	}) | rpl::on_next([](Main::Account*) {
		AccountsAvailable();
	}, lifetime);
}

bool FinishDuressStartup(bool headless) {
	if (!headless) {
		// The real code opened the vault (or there is no vault): a pending
		// logout that the accounts were open for has started by now, and the
		// first window waits for it. One the passcode lock keeps from the
		// accounts waits for the real passcode on the lock screen, which
		// then holds its unlock until the logout is over - a server-side
		// logout for every account, better than a local reset, and the owner
		// is right there to type the passcode.
		WaitForLogout();
		return true;
	}
	// The decoy was closed with a logout pending: nothing of the app is ever
	// shown, nothing of it may be announced while it logs out, and nobody is
	// left to type a passcode.
	EnterExiting();
	StartPendingWhileSealed();
	auto &run = State();
	if (run.running) {
		run.waiters.push_back(ExitAfterLogout);
	} else {
		// Nothing can be done yet: "log out the current account" behind a
		// passcode lock stays recorded for the real passcode. The app goes
		// away the way it would have right after the decoy.
		FlushAndExit();
	}
	return false;
}

bool DuressLogoutRunning() {
	return State().running;
}

bool DuressSilencesNotifications() {
	return State().running || State().exiting;
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

void FilterDuressBackupPayload(QJsonObject &payload) {
	auto stores = payload.value(kBackupStoresKey).toObject();
	auto storesChanged = false;
	const auto drop = [&](const QString &key) {
		payload.remove(key);
		if (stores.contains(key)) {
			stores.remove(key);
			storesChanged = true;
		}
	};
	drop(kKeyPending);
	drop(kLegacyKeyEnabled);
	drop(kLegacyKeyMode);
	if (payload.contains(kKeyMode)) {
		const auto mode = payload.value(kKeyMode);
		const auto harmless = mode.isString()
			&& ((mode.toString() == kModeOff)
				|| (mode.toString() == kModeExit));
		if (!harmless) {
			drop(kKeyMode);
		}
	}
	if (storesChanged) {
		if (stores.isEmpty()) {
			payload.remove(kBackupStoresKey);
		} else {
			payload.insert(kBackupStoresKey, stores);
		}
	}
}

rpl::producer<> FakeCrashChanges() {
	return Settings::Instance().changes(
	) | rpl::filter([](const QString &key) {
		return (key == kKeyMode) || (key == kKeyCode);
	}) | rpl::to_empty;
}

} // namespace Lumina
