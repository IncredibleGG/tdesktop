/*
This file is part of LuminaGram,
a fork of Telegram Desktop.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include <QtCore/QByteArray>
#include <QtCore/QJsonObject>
#include <QtCore/QString>

#include <vector>

namespace Lumina {

// Passphrase-encrypted export and import of everything Lumina::Settings holds.
//
// This is the engine only: no widgets, no dialogs, no file pickers. The rows,
// the passphrase boxes and the two file dialogs live in
// lumina/lumina_backup_settings.h.
//
// WHAT TRAVELS. The plaintext is exactly Settings::Instance().exportForBackup()
// - every key of all three stores plus the reserved `_luminaStores` map that
// says which of them are not Store::Prefs, EXCEPT the translation and
// speech-to-text API keys. Since 7.3.4 those never leave the device in a
// backup (lumina_secret_store.h); the other private values do travel: the
// vault and fake-crash codes, the decoy note, saved originals of outgoing
// messages. Nothing else is touched: tdesktop's own settings, the accounts, the
// message database and the caches are not part of a backup and are not
// restored from one.
//
// A backup written by 7.3.3 or earlier still carries API keys in its
// (encrypted) payload. Importing one keeps working: the keys go through the
// provider confirmation below like any other route change, and on Windows the
// store seals them on the way in.
//
// ONE EXCEPTION, in both directions: the parked ToS-grey keys (stealthOnline,
// stealthTyping, stealthReadReceipts). F-02 removed every row that could set
// them but two of them still have live readers, so a file that carried them
// would be able to switch this client into permanent always-offline mode with
// no UI left to switch it back. Export drops them and import drops them again;
// see RemoveParkedKeys() in the .cpp.
//
// THE CONTAINER, and why it is not Android's. LuminaBackupActivity writes
// AES/CBC/PKCS5Padding with no authentication tag at all, so a wrong passphrase
// is only noticed when the padding of the garbage it produced happens to be
// invalid - which it is not, roughly once in every few hundred tries, and the
// result is then handed to the JSON parser and, if that survives, written into
// the preference store. Nothing in that container detects a file that was
// edited on purpose either. So this is a different container, deliberately:
//
//   magic   "LuminaGramBackup"      (as in Android, so the file is recognised)
//   version 2                        (Android's unauthenticated container is 1)
//   kdf     "PBKDF2-HMAC-SHA256"
//   iter    the iteration count THIS file was written with - always read back
//           from the file, never assumed, so raising the constant later leaves
//           every older backup importable
//   cipher  "AES-256-CBC+HMAC-SHA256"
//   salt    16 random bytes          iv   16 random bytes
//   data    base64 ciphertext        mac  base64 HMAC-SHA256 tag
//
// 64 bytes of key material come out of PBKDF2-HMAC-SHA256(passphrase, salt,
// iter); the first 32 encrypt, the last 32 authenticate. The tag is taken
// encrypt-then-MAC over magic || version || iter || salt || iv || ciphertext,
// and it is checked - in constant time - BEFORE anything is decrypted. A wrong
// passphrase, a truncated file and a doctored file are therefore all rejected
// with certainty rather than with probability.
//
// Version 1 files are recognised and refused with their own message, not
// silently accepted: there is no honest way to authenticate one.
//
// ATOMICITY. Import decrypts, authenticates and fully parses the payload before
// a single key is written, so a bad passphrase can never leave the store half
// restored. The write itself then goes through Settings::importAll() followed
// by saveNow(). Keys the backup does not carry are left alone; keys it does
// carry are overwritten - the same rule Android states in its own info text.
//
// WHERE YOUR TEXT GOES IS NOT RESTORED SILENTLY. A backup is a file somebody
// can hand you, and the provider settings decide where every translated
// message, every explained message and every transcribed voice note is sent.
// So import runs in two steps: DecryptBackup() yields the payload without
// writing anything, BackupProviderRouteChanges() says whether applying it
// would change a translation / AI / speech-to-text endpoint or base URL,
// select a custom-endpoint provider, or set or replace an API key - and which
// destinations that would send data to - and the UI asks before
// ApplyBackupPayload(). Declining applies everything else and keeps the
// current provider selection, endpoints, models and keys
// (DropProviderRouteKeys()).
//
// Each of the three store files is replaced atomically by QSaveFile, but the
// three of them are not replaced atomically as a SET: if the second write fails
// the first has already landed, and the on-disk profile is then part restored
// and part original until a later saveNow() retries the rest. Memory stays
// consistent throughout, so this is only visible after a crash or a kill
// between the failed write and the retry.
//
// THREADING AND COST. Main thread, synchronous, and the key derivation is meant
// to be expensive: expect a few hundred milliseconds of blocked UI per call.
// That is acceptable for a one-shot action the user just confirmed in a dialog,
// and it is the entire defence a short passphrase has. An imported file whose
// iteration count falls outside a sane range is refused before the count is
// ever used, so a hostile file cannot turn that pause into a hang.
enum class BackupError {
	None,
	PassphraseTooShort,
	FileUnreadable,     // missing, unopenable, or absurdly large
	NotABackup,         // not JSON, wrong magic, or structurally impossible
	Unauthenticated,    // Android's version 1 container, refused on purpose
	NewerFormat,        // written by a version of LuminaGram we do not know
	BadPassphrase,      // the tag did not match
	Damaged,            // tag matched, payload still unusable
	WriteFailed,
	CryptoFailed,
};

// The parsed envelope of a backup file. Structurally validated, NOT yet
// authenticated and NOT yet decrypted - holding one of these says only that the
// file looks like a LuminaGram backup, which is what lets the UI report "this
// is not a backup" before it asks for a passphrase.
struct BackupEnvelope {
	int version = 0;
	int iterations = 0;
	QByteArray salt;
	QByteArray iv;
	QByteArray data;
	QByteArray mac;
};

struct BackupRead {
	BackupError error = BackupError::None;
	BackupEnvelope envelope;
};

// The shortest passphrase that will be accepted, matching Android's minimum.
[[nodiscard]] int MinimumPassphraseLength();

// ".lgbak", as on Android, so one account's backups are interchangeable
// between the two clients as soon as Android also writes version 2.
[[nodiscard]] QString BackupFileExtension();

// Encrypts the whole preference store under `passphrase` and writes it to
// `path`, atomically. Never logs, and never writes anything on failure.
[[nodiscard]] BackupError WriteBackup(
	const QString &path,
	const QString &passphrase);

[[nodiscard]] BackupRead ReadBackupFile(const QString &path);
[[nodiscard]] BackupRead ReadBackupBytes(const QByteArray &content);

// Authenticates and decrypts. BackupError::None means `payload` now holds the
// restored settings - parked keys already dropped - and that NOTHING has been
// written yet: the store is untouched until ApplyBackupPayload().
[[nodiscard]] BackupError DecryptBackup(
	const BackupEnvelope &envelope,
	const QString &passphrase,
	QJsonObject &payload);

// What applying `payload` would do to where LuminaGram sends text and voice.
// `changed` is true when it would change the translation or speech-to-text
// endpoint / base URL, select a custom-endpoint provider (the OpenAI-compatible
// translation / Explain endpoint, the OpenAI-compatible Whisper endpoint), or
// set or replace an API key. `destinations` names where the data would then
// go - scheme, host and port of each endpoint, the fixed host of a vendor
// whose key is being set, or the bare provider id for a key this build has no
// engine for - without duplicates and in a stable order.
struct ProviderRouteChanges {
	bool changed = false;
	std::vector<QString> destinations;
};
[[nodiscard]] ProviderRouteChanges BackupProviderRouteChanges(
	const QJsonObject &payload);

// Removes every key that decides where text and voice are sent - provider
// selections, base URLs, models and API keys of translation and speech-to-text
// - so that applying what is left keeps the current ones exactly as they are.
void DropProviderRouteKeys(QJsonObject &payload);

// Writes `payload` into the store and requests a flush.
//
// !! IT DOES NOT REPORT WHETHER THE FLUSH SUCCEEDED. Settings::saveNow()
// returns void and only logs a store it could not write, so a read-only or
// full tdata directory produces "Backup restored" while the three JSON files
// still hold the old values - and Settings::saveNow() cancels the debounce
// timer without rescheduling it, so nothing retries until the next unrelated
// write or the destructor. Fixing this needs one change in lumina_settings:
// make saveNow() return whether every dirty store was written, and report
// WriteFailed when it says no.
void ApplyBackupPayload(const QJsonObject &payload);

// The message to show for a failure, in the current in-app language. Empty for
// BackupError::None.
[[nodiscard]] QString BackupErrorText(BackupError error);

} // namespace Lumina
