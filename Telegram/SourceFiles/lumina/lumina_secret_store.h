/*
This file is part of LuminaGram,
a fork of Telegram Desktop.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include <QtCore/QString>

#include <optional>

namespace Lumina {

// API keys at rest: the translation keys (`translateKey_<provider id>`) and
// the speech-to-text keys (`sttKey_<engine id>`).
//
// Lumina::Settings is the only caller. For every key IsApiKeySetting()
// accepts, Settings::set() / importAll() store the SEALED form, getString()
// answers the opened form, and loading the store seals every plaintext value
// an older version (7.3.3 and earlier) or an older backup left behind - the
// plaintext disappears from disk with the next atomic rewrite of
// luminagram_private.json, and only once the sealed value has been checked to
// open back to the same key. Owners of the keys (lumina_translate_providers,
// lumina_transcribers) keep calling Settings exactly as before.
//
// Backups never carry the keys: Settings::exportForBackup() leaves them out.
// An older .lgbak that does carry them still imports - after the existing
// "Translation and AI services" confirmation - and they are sealed on the way
// into the store.
//
// PER PLATFORM.
//
// Windows: DPAPI (CryptProtectData, current user scope, no UI). The blob is
// bound to the Windows account, so a tdata folder copied to another account
// or another PC reads its API keys back as unset; everything else in it keeps
// working, and the keys are entered again there.
//
// macOS: NOT sealed - the keys stay in luminagram_private.json as before.
// Release builds are ad-hoc signed, and the Keychain identifies an ad-hoc
// signed program by its cdhash, which every update changes: an item written by
// one version cannot be read by the next without the user typing the login
// keychain password (errSecAuthFailed when no UI is allowed, both with the
// default ACL and with an "any application" ACL, because the partition list
// still pins the cdhash), and a "Deny" there would lose the keys. The
// data-protection keychain needs an entitlement an ad-hoc signature cannot
// carry (errSecMissingEntitlement). Revisit once releases are signed with a
// stable Developer ID.
//
// Linux: NOT sealed - as before. libsecret is not part of the build image,
// and a synchronous Secret Service call blocks the main thread when no
// service is running.
//
// Main thread only, like Lumina::Settings.

// `translateKey_<id>` and `sttKey_<id>`, with a non-empty id.
[[nodiscard]] bool IsApiKeySetting(const QString &key);

// Whether this build seals API keys at all (see above).
[[nodiscard]] bool ApiKeySealingAvailable();

// True for a value SealApiKey() produced, on any platform.
[[nodiscard]] bool IsSealedApiKey(const QString &stored);

// The sealed form of `plain`, already checked to open back to `plain`.
// std::nullopt when this platform does not seal or sealing failed: the caller
// keeps the plaintext then, so a key is never lost to a failed seal.
[[nodiscard]] std::optional<QString> SealApiKey(const QString &plain);

// Reverses SealApiKey(). A value that is not sealed - an older version's
// plaintext, or a platform that does not seal - comes back unchanged. A sealed
// value that cannot be opened here (another Windows account or PC, a damaged
// blob, a profile moved from Windows to another system) reads back empty,
// exactly like a key that was never set.
[[nodiscard]] QString OpenApiKey(const QString &stored);

} // namespace Lumina
