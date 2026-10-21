/*
This file is part of LuminaGram,
a fork of Telegram Desktop.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "lumina/lumina_fake_crash.h"

#include "lumina/lumina_locale.h"
#include "lumina/lumina_settings.h"

#include <QtCore/QPointer>
#include <QtGui/QGuiApplication>
#include <QtWidgets/QApplication>
#include <QtWidgets/QMessageBox>
#include <QtWidgets/QWidget>

#include <cstdlib>
#include <vector>

#ifdef Q_OS_WIN
#include <windows.h>
#endif // Q_OS_WIN

namespace Lumina {
namespace {

// Same key names and values as Android's LuminaConfig (KEY_FAKE_CRASH_*,
// FAKE_CRASH_MODE_*), so the two platforms' stores can be read against each
// other.
const auto kKeyEnabled = u"fakeCrashEnabled"_q;
const auto kKeyCode = u"fakeCrashCode"_q;
const auto kKeyMode = u"fakeCrashMode"_q;
const auto kModeExit = u"exit"_q;
const auto kModeDialog = u"dialog"_q;

// Dialog mode, step one: every window goes away first, so the alert stands
// alone the way a crash leaves it.
//
// Qt quits when the last window goes away (quitOnLastWindowClosed() is Qt's
// default, true, and nothing in this tree changes it outside the vault gate -
// see lumina_vault.cpp), and that quit is the graceful, vetoable one
// TriggerFakeCrash() exists to avoid. So it is switched off before anything
// is hidden, and never switched back: the process ends right after the alert.
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

// Dialog mode, step two: the alert itself, modal, returning once it is
// dismissed in any way. The system's own message box where there is one to
// imitate - on Windows MessageBoxW is exactly what a crash dialog is drawn
// with; on macOS QMessageBox is drawn by the system (NSAlert, with the app's
// icon); on Linux it is the plain Qt one.
void ShowAlert() {
	const auto title = Tr(u"LuminaFakeCrashTitle"_q);
	const auto text = Tr(u"LuminaFakeCrashText"_q);
	const auto caption = u"LuminaGram"_q;
#ifdef Q_OS_WIN
	const auto body = (title + u"\n\n"_q + text).toStdWString();
	const auto head = caption.toStdWString();
	MessageBoxW(
		nullptr,
		body.c_str(),
		head.c_str(),
		MB_OK | MB_ICONERROR | MB_SETFOREGROUND | MB_TOPMOST | MB_TASKMODAL);
#else // Q_OS_WIN
	QMessageBox box(QMessageBox::Critical, caption, title);
	box.setInformativeText(text);
	box.addButton(Tr(u"LuminaFakeCrashCloseApp"_q), QMessageBox::AcceptRole);
	box.exec();
#endif // Q_OS_WIN
}

} // namespace

bool FakeCrashEnabled() {
	return Settings::Instance().getBool(kKeyEnabled, false);
}

// Flushed rather than left to the ~500ms coalescing timer, here and in
// SetFakeCrashCode(). These two are what FakeCrashArmed() reads, and the one
// way this application is most likely to end is the feature they arm:
// TriggerFakeCrash() calls std::_Exit(), which runs no destructor and drains
// no timer. Losing the write is silent and fails open - the duress code simply
// is not armed the next time - which is the safe direction but also the one
// the user cannot see. A saveNow() per toggle is cheaper than that.
void SetFakeCrashEnabled(bool value) {
	auto &settings = Settings::Instance();
	settings.set(kKeyEnabled, value, Store::Private);
	if (value && settings.getString(kKeyMode).isEmpty()) {
		// Writes this platform's default down the first time the feature is
		// switched on, so a backup restored on Android - where a missing mode
		// means Dialog - keeps meaning what it meant here.
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

FakeCrashMode CurrentFakeCrashMode() {
	// Missing - every desktop user from before the choice existed - and
	// anything unrecognised read as Exit: the behaviour this platform has
	// always had (see the header).
	return (Settings::Instance().getString(kKeyMode) == kModeDialog)
		? FakeCrashMode::Dialog
		: FakeCrashMode::Exit;
}

// Flushed at once for the same reason as the two setters above.
void SetFakeCrashMode(FakeCrashMode mode) {
	Settings::Instance().set(
		kKeyMode,
		(mode == FakeCrashMode::Dialog) ? kModeDialog : kModeExit,
		Store::Private);
	Settings::Instance().saveNow();
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

[[noreturn]] void TriggerFakeCrash() {
	// std::_Exit, and nothing before it. Three things this deliberately does
	// NOT do, each of which was considered and rejected:
	//
	//  * Core::Quit(). It is vetoable. Core::Application::preventsQuit() pops
	//    a confirmation box when an export, an upload or a download is in
	//    flight (core/application.cpp:1068-1121) and then returns without
	//    quitting, and readyToQuit() can defer the exit by three seconds. A
	//    duress control that answers "are you sure you want to stop this
	//    download?" in front of the person holding the phone is worse than no
	//    duress control at all.
	//
	//  * std::abort() or any other real fault. It would be caught by the crash
	//    handler, which writes a genuine dump; the NEXT launch would then open
	//    LastCrashedWindow (core/sandbox.cpp:451-476) offering to send a crash
	//    report, and on macOS the system would show its own "quit
	//    unexpectedly" dialog naming the app. Loud, and it would transmit a
	//    report about something the user did on purpose.
	//
	//  * a graceful shutdown. Saving state takes visible time and leaves the
	//    window on screen while it happens, which is exactly what a crash does
	//    not look like.
	//
	// _Exit() leaves the empty tdata/working marker behind, so the next launch
	// logs "the previous launch was not finished properly" and starts
	// normally - the sandbox handles an empty dump explicitly. That is the
	// same trace a real crash of this kind leaves, and no window.
	//
	// The cost is the same as any crash: anything the app had not flushed is
	// lost. tdesktop writes through temp-file-and-rename and survives this by
	// design; under duress, an exit that cannot be talked out of is worth far
	// more than a tidy one. Nothing is written here on purpose - not the
	// pref store, not a log line. A log line saying a duress code was entered
	// would hand the whole feature to anyone who later reads the log.
	std::_Exit(0);
}

[[noreturn]] void RunFakeCrash() {
	if (CurrentFakeCrashMode() == FakeCrashMode::Dialog) {
		// Whatever happens in here, the exit below still happens: a duress
		// code that unlocked nothing but also left the app running would be
		// the worst outcome of the three.
		try {
			HideAllWindows();
			ShowAlert();
		} catch (...) {
		}
	}
	TriggerFakeCrash();
}

rpl::producer<> FakeCrashChanges() {
	return Settings::Instance().changes(
	) | rpl::filter([](const QString &key) {
		return (key == kKeyEnabled) || (key == kKeyCode) || (key == kKeyMode);
	}) | rpl::to_empty;
}

} // namespace Lumina
