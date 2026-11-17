/*
This file is part of LuminaGram,
a fork of Telegram Desktop.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include <rpl/producer.h>

#include <QtCore/QString>

class QJsonObject;

namespace Lumina {

// Duress passcode (formerly "fake-crash unlock"; the file name and the code's
// stored key are kept, so an existing code carries over).
//
// A SECOND, purely local code. Typed into the passcode lock screen it never
// unlocks; it does what the user chose (DuressMode below):
//
//  * Exit - end the process on the spot, nothing logged out. What the feature
//    always did on desktop.
//  * LogoutCurrent / LogoutAll - really log out the current account, or every
//    account on this device, the way the app's own "Log out" does it
//    (auth.logOut on the server plus the local wipe; on a dead network the
//    local logout goes ahead without the server after a few seconds), and
//    then end the process.
//
// The logout modes are also accepted wherever the disguise vault accepts its
// secret code - the password door, the calculator's "=", the notepad's title
// double-click - and there the decoy simply carries on, so an onlooker sees a
// wrong code and nothing else. See DuressCodeEnteredAtVault() for when the
// logout itself happens in that case.
//
// Only this exact code fires. An ordinary wrong passcode never does anything
// it did not do before.
//
// Stored the way Android's LuminaDuress stores it, so the two platforms read
// each other's backups the same way: the mode under `duressMode` ("off",
// "exit", "logoutCurrent", "logoutAll"; missing or unknown reads as off), the
// code under `fakeCrashCode`. The fake-crash era kept an enabled flag and a
// mode of its own (`fakeCrashEnabled`, `fakeCrashMode`); those are dropped at
// startup, so whoever had the old feature on starts OFF and picks a mode
// again. The code itself is kept. A logout mode is only ever armed by the user
// picking it in settings, through a confirmation that spells out what it
// costs - never by an upgrade and never by a restored backup
// (FilterDuressBackupPayload()).
//
// Every key lives in Store::Private, written only through the setters below
// (a bare Settings::set() would relocate it into the plaintext pref file).
// Everything defaults off, and with it off the passcode screen and the vault
// behave exactly as before.

[[nodiscard]] bool FakeCrashEnabled();

// Switching on starts at Exit, the one mode that destroys nothing; switching
// off forgets the mode. The code is kept either way.
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
// must never persist the code as ordinary data (the notepad decoy's note),
// and for the passcode lock editor, which refuses it as a passcode.
[[nodiscard]] bool IsDuressCode(const QString &value);

// What an accepted duress code does while the feature is on. With it off,
// CurrentDuressMode() answers Exit, which is also what switching on selects.
enum class DuressMode {
	Exit,
	LogoutCurrent,
	LogoutAll,
};
[[nodiscard]] DuressMode CurrentDuressMode();
void SetDuressMode(DuressMode mode); // Also switches the feature on.
[[nodiscard]] bool IsLogoutMode(DuressMode mode);

// Ends the process immediately. Does not return, does not unwind, does not
// save, does not log.
[[noreturn]] void ExitAtOnce();

// What the passcode lock screen calls on an accepted duress code. Exit mode
// ends the process before returning. The logout modes hide every window and
// the tray icon at once, return, carry out the logout on the next turn of the
// event loop and end the process when it is done - the lock screen may be
// destroyed by the logout in between, so the caller must not touch itself
// afterwards.
void RunDuressFromPasscodeLock();

// What the vault gate calls with a code it did not accept as its own secret
// code. True when it was the duress code in a logout mode; the gate then
// behaves exactly as for a wrong code, and the decoy carries on.
//
// The gate runs before local storage and the accounts exist (see
// lumina_vault.h), so the logout is recorded here and carried out as soon as
// the gate is over:
//
//  * the decoy is closed - Core::Application::run() does not quit then but
//    starts the app without ever showing a window, logs out and ends the
//    process (FinishDuressStartup());
//  * the real code is typed into the decoy afterwards - the logout runs
//    before any account is shown.
//
// With a passcode lock on, the accounts stay sealed until the passcode is
// entered, so neither which account is the current one nor any key to log it
// out with can be reached. When the decoy is closed, "log out all" then
// resets all local data at once, exactly like the lock screen's own "Log
// out"; "log out the current account" - and either mode after the real code
// was typed into the decoy - waits for the real passcode to open the
// accounts, and the lock screen holds its unlock until the logout is over.
[[nodiscard]] bool DuressCodeEnteredAtVault(const QString &entered);

// True when a logout recorded at the vault (or on a lock screen that could
// not reach the accounts) is still waiting to be carried out - from this
// launch or an earlier one that was killed. Core::Application::run() asks it
// when the vault gate says to quit.
[[nodiscard]] bool DuressLogoutPending();

// Called once from Core::Application::run() before the accounts start. Drops
// the fake-crash era keys, and performs a recorded logout the first time the
// accounts are available - right at startup without a passcode lock, or as
// soon as the real passcode has opened them.
void SetupDuress();

// Called from Core::Application::run() right after the accounts start, before
// the tray and the first window. `headless` means the vault gate was closed
// with a logout pending: the app must never show anything; this starts a
// pending "log out all" even while the passcode lock seals the accounts (as
// a local reset, see DuressCodeEnteredAtVault()), ends the process itself -
// after the logout, or at once when there is nothing it can do yet - and
// returns false. Otherwise it waits for a logout in flight, so the account is
// never on screen while it is being logged out, and returns true.
[[nodiscard]] bool FinishDuressStartup(bool headless);

// True while a duress logout is in flight. The lock screen does not unlock
// until it is over.
[[nodiscard]] bool DuressLogoutRunning();

// True from the moment a duress code starts a logout until the process ends:
// nothing from any account may announce itself in that time, not even after
// the logout itself is over and the app is only waiting to close.
[[nodiscard]] bool DuressSilencesNotifications();

// Calls `callback` once the logout in flight is over (immediately, when none
// is). For the lock screen, which holds its unlock until then.
void AfterDuressLogout(Fn<void()> callback);

// Backups (lumina_backup.cpp), on the way out and again on the way in, since
// the file is not to be trusted: drops the recorded logout, which belongs to
// this device and this moment only, the fake-crash era keys, and the mode
// when it is anything but "off" or "exit" - so restoring a file can never arm
// a logout mode, and the mode already on the restoring device stays as it is.
void FilterDuressBackupPayload(QJsonObject &payload);

// Fires for any change to the mode or the code, including a whole-file
// restore through Settings::importAll().
[[nodiscard]] rpl::producer<> FakeCrashChanges();

} // namespace Lumina
