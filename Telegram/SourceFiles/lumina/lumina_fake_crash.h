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

// Fake-crash duress unlock.
//
// A SECOND, purely local code that is typed into the passcode lock screen and,
// instead of unlocking, ends the process. What it looks like is the user's
// choice (FakeCrashMode below): by default the window disappears on the spot,
// nothing is shown, nothing is written, and to anyone watching the app simply
// died; or the windows disappear and a system-style "LuminaGram has stopped
// working" alert is shown first, the way Android's default does it
// (PasscodeView.showFakeCrash()), and the process ends once it is dismissed.
// Either way the end is TriggerFakeCrash() - see there for why no real crash.
//
// All three keys live in Store::Private. The plan's F-01 table only names
// `fakeCrashCode` as sensitive, but `fakeCrashEnabled` is moved with it: the
// flag alone tells a reader of the pref file that a duress code exists, which
// is most of what it should not learn; `fakeCrashMode` follows for the same
// reason. Every write goes through the setters below, which is what keeps a
// bare set() from relocating any of them into the ordinary plaintext pref
// file.
//
// Everything here defaults off, and with it off the passcode screen behaves
// exactly as stock Telegram Desktop.

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

// What an accepted duress code does. Stored as Android's spellings ("exit",
// "dialog") under Android's key name, `fakeCrashMode`.
//
// The two platforms read a MISSING value differently, on purpose: each keeps
// the behaviour its existing users already set up and tested. Desktop has
// always closed on the spot, so here a missing value is Exit; Android has
// always shown its alert, so there it is Dialog.
enum class FakeCrashMode {
	Exit = 0,
	Dialog = 1,
};
[[nodiscard]] FakeCrashMode CurrentFakeCrashMode();
void SetFakeCrashMode(FakeCrashMode mode);

// Ends the process immediately. Does not return, does not unwind, does not
// save, does not log.
[[noreturn]] void TriggerFakeCrash();

// What the lock screen calls on an accepted duress code: carries out the
// stored mode. Does not return either. Exit is TriggerFakeCrash() and nothing
// else. Dialog hides every window first, shows the fake alert (modal, so this
// waits in it), and then calls TriggerFakeCrash() however the alert is
// dismissed.
[[noreturn]] void RunFakeCrash();

// Fires for any change to any of the three keys, including a whole-file
// restore through Settings::importAll().
[[nodiscard]] rpl::producer<> FakeCrashChanges();

} // namespace Lumina
