/*
This file is part of LuminaGram,
a fork of Telegram Desktop.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include <rpl/producer.h>

#include <QtCore/QString>

namespace Lumina {

// Duress passcode (formerly "fake-crash unlock"; the file and key names are
// kept so existing preferences and backups keep working).
//
// A SECOND, purely local code. Typed into the passcode lock screen it never
// unlocks; it does what the user chose (DuressMode below):
//
//  * Exit - end the process on the spot, nothing logged out. This is what the
//    feature always did on desktop and it stays the default.
//  * LogoutCurrent / LogoutAll - really log out the current account, or every
//    account on this device, the way the app's own "Log out" does it
//    (auth.logOut on the server plus the local wipe; on a dead network the
//    local logout goes ahead without the server after a few seconds), and
//    then end the process.
//
// The logout modes are also accepted wherever the disguise vault accepts its
// secret code - the password door, the calculator's "=", the notepad's title
// double-click - and there the decoy simply carries on, so an onlooker sees a
// wrong code and nothing else. See DuressCodeEnteredAtVault() for why the
// logout itself happens later in that case.
//
// Only this exact code fires. An ordinary wrong passcode never does anything
// it did not do before.
//
// The fake-crash alert this replaced ("dialog") is gone. Its users read as
// Exit, never as a logout mode: a logout is only ever armed by the user
// picking it in settings, through a confirmation that spells out what it
// costs.
//
// Every key lives in Store::Private, written only through the setters below
// (a bare Settings::set() would relocate it into the plaintext pref file).
// Everything defaults off, and with it off the passcode screen and the vault
// behave exactly as before.

[[nodiscard]] bool FakeCrashEnabled();
void SetFakeCrashEnabled(bool value);

// Already trimmed, empty when unset. Never logged, never shown in a toast,
// never sent anywhere.
[[nodiscard]] QString FakeCrashCode();
void SetFakeCrashCode(const QString &code); // Empty removes the key.

// Enabled AND a non-empty code stored: the state in which the code can
// actually fire. The toggle on its own does nothing, so this - not
// FakeCrashEnabled() - is what other behaviour keys off.
[[nodiscard]] bool FakeCrashArmed();

// True only when `entered` is exactly the armed duress code. Exact match on
// the trimmed text: the stored code is trimmed when it is saved, and what the
// user typed is trimmed the same way before it is compared.
[[nodiscard]] bool FakeCrashCodeMatches(const QString &entered);

// True when `value` is the stored duress code, armed or not. For places that
// must never persist the code as ordinary data (the notepad decoy's note).
[[nodiscard]] bool IsDuressCode(const QString &value);

// What an accepted duress code does. Stored under `fakeCrashMode` as "exit",
// "logoutCurrent" and "logoutAll". A missing value, Android's old "dialog"
// and anything unrecognised all read as Exit - the behaviour desktop has
// always had - so no stored state can turn into a logout by accident.
enum class DuressMode {
	Exit,
	LogoutCurrent,
	LogoutAll,
};
[[nodiscard]] DuressMode CurrentDuressMode();
void SetDuressMode(DuressMode mode);
[[nodiscard]] bool IsLogoutMode(DuressMode mode);

// Ends the process immediately. Does not return, does not unwind, does not
// save, does not log.
[[noreturn]] void ExitAtOnce();

// What the passcode lock screen calls on an accepted duress code. Exit mode
// ends the process before returning. The logout modes hide every window at
// once, return, carry out the logout on the next turn of the event loop and
// end the process when it is done - the lock screen may be destroyed by the
// logout in between, so the caller must not touch itself afterwards.
void RunDuressFromPasscodeLock();

// What the vault gate calls with a code it did not accept as its own secret
// code. True when it was the duress code in a logout mode; the gate then
// behaves exactly as for a wrong code.
//
// The gate runs before local storage and the accounts exist (see
// lumina_vault.h), so nothing can be logged out there: the logout is
// recorded and carried out the moment the accounts are next opened - by
// SetupDuress() below, before anything of them is shown.
[[nodiscard]] bool DuressCodeEnteredAtVault(const QString &entered);

// Called once from Core::Application::run() before the accounts start.
// Performs a recorded logout (see above) the first time the accounts are
// available - right at startup without a passcode lock, or as soon as the
// real passcode has opened them.
void SetupDuress();

// True while a duress logout is in flight. Notifications are suppressed for
// that time, and the lock screen does not unlock until it is over.
[[nodiscard]] bool DuressLogoutRunning();

// Calls `callback` once the logout in flight is over (immediately, when none
// is). For the lock screen, which holds its unlock until then.
void AfterDuressLogout(Fn<void()> callback);

// Called from Core::Application::run() right after the accounts start and
// before the first window is shown: when a recorded logout started there,
// waits for it, so the account is never on screen while it is being logged
// out. A no-op otherwise.
void WaitForDuressLogout();

// Fires for any change to the enabled flag, the code or the mode, including
// a whole-file restore through Settings::importAll().
[[nodiscard]] rpl::producer<> FakeCrashChanges();

} // namespace Lumina
