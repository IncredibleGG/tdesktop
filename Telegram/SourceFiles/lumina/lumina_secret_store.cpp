/*
This file is part of LuminaGram,
a fork of Telegram Desktop.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "lumina/lumina_secret_store.h"

#include "base/flat_map.h"

#ifdef Q_OS_WIN
#include <windows.h>
#include <wincrypt.h>
#include <dpapi.h>
#endif // Q_OS_WIN

namespace Lumina {
namespace {

// The same spellings lumina_translate_providers.cpp and lumina_transcribers.cpp
// build their key names from (ApiKeyPrefix() in each). Renaming one there
// means renaming it here, or that key silently goes back to plaintext.
[[nodiscard]] QString TranslateKeyPrefix() {
	return u"translateKey_"_q;
}

[[nodiscard]] QString SttKeyPrefix() {
	return u"sttKey_"_q;
}

// "lgsec1:<scheme>:<base64>". The scheme says what can open the value, so a
// profile moved between systems is recognised as sealed everywhere instead of
// being mistaken for a (very strange) plaintext key and sent to a provider.
[[nodiscard]] QString SealedPrefix() {
	return u"lgsec1:"_q;
}

// Opening a DPAPI blob is a round trip into LSASS, and the keys are read on
// every settings repaint and every translate request. Cached by sealed string,
// failures included (an unopenable blob does not become openable later in the
// same session). A key is replaced only when the user edits it, so a handful
// of entries exist at any time; the bound only keeps a pathological session
// from growing it without limit.
constexpr auto kMaxCached = 32;

[[nodiscard]] base::flat_map<QString, QString> &OpenedCache() {
	static auto result = base::flat_map<QString, QString>();
	return result;
}

void Remember(const QString &stored, const QString &opened) {
	auto &cache = OpenedCache();
	if (cache.size() >= kMaxCached) {
		cache.clear();
	}
	cache[stored] = opened;
}

#ifdef Q_OS_WIN

[[nodiscard]] QString DpapiPrefix() {
	return SealedPrefix() + u"dpapi:"_q;
}

// Not a secret: it only means a program that lifts a blob out of
// luminagram_private.json and hands it to CryptUnprotectData under the same
// Windows account also has to know this string.
[[nodiscard]] DATA_BLOB Entropy() {
	static auto bytes = QByteArray("LuminaGram API key v1");
	return DATA_BLOB{
		DWORD(bytes.size()),
		reinterpret_cast<BYTE*>(bytes.data()),
	};
}

[[nodiscard]] std::optional<QByteArray> Protect(const QByteArray &plain) {
	auto input = DATA_BLOB{
		DWORD(plain.size()),
		reinterpret_cast<BYTE*>(const_cast<char*>(plain.constData())),
	};
	auto entropy = Entropy();
	auto output = DATA_BLOB{ 0, nullptr };
	if (!CryptProtectData(
			&input,
			L"LuminaGram",
			&entropy,
			nullptr,
			nullptr,
			CRYPTPROTECT_UI_FORBIDDEN,
			&output)) {
		LOG(("Lumina Error: CryptProtectData failed (%1), "
			"API key kept as it was.").arg(qint64(GetLastError())));
		return std::nullopt;
	}
	const auto result = QByteArray(
		reinterpret_cast<const char*>(output.pbData),
		int(output.cbData));
	LocalFree(output.pbData);
	return result;
}

[[nodiscard]] std::optional<QByteArray> Unprotect(const QByteArray &sealed) {
	auto input = DATA_BLOB{
		DWORD(sealed.size()),
		reinterpret_cast<BYTE*>(const_cast<char*>(sealed.constData())),
	};
	auto entropy = Entropy();
	auto output = DATA_BLOB{ 0, nullptr };
	if (!CryptUnprotectData(
			&input,
			nullptr,
			&entropy,
			nullptr,
			nullptr,
			CRYPTPROTECT_UI_FORBIDDEN,
			&output)) {
		LOG(("Lumina Error: CryptUnprotectData failed (%1), "
			"the API key reads as unset.").arg(qint64(GetLastError())));
		return std::nullopt;
	}
	const auto result = QByteArray(
		reinterpret_cast<const char*>(output.pbData),
		int(output.cbData));
	SecureZeroMemory(output.pbData, output.cbData);
	LocalFree(output.pbData);
	return result;
}

#endif // Q_OS_WIN

} // namespace

bool IsApiKeySetting(const QString &key) {
	const auto translate = TranslateKeyPrefix();
	const auto stt = SttKeyPrefix();
	return (key.startsWith(translate) && key.size() > translate.size())
		|| (key.startsWith(stt) && key.size() > stt.size());
}

bool ApiKeySealingAvailable() {
#ifdef Q_OS_WIN
	return true;
#else // Q_OS_WIN
	return false;
#endif // Q_OS_WIN
}

bool IsSealedApiKey(const QString &stored) {
	return stored.startsWith(SealedPrefix());
}

std::optional<QString> SealApiKey(const QString &plain) {
	if (plain.isEmpty() || IsSealedApiKey(plain)) {
		return std::nullopt;
	}
#ifdef Q_OS_WIN
	const auto sealed = Protect(plain.toUtf8());
	if (!sealed) {
		return std::nullopt;
	}
	const auto result = DpapiPrefix() + QString::fromLatin1(sealed->toBase64());

	// Never store what cannot be read back: the caller drops the plaintext
	// as soon as this returns a value.
	const auto check = Unprotect(*sealed);
	if (!check || QString::fromUtf8(*check) != plain) {
		LOG(("Lumina Error: a sealed API key did not open back to the same "
			"value, kept as it was."));
		return std::nullopt;
	}
	Remember(result, plain);
	return result;
#else // Q_OS_WIN
	return std::nullopt;
#endif // Q_OS_WIN
}

QString OpenApiKey(const QString &stored) {
	if (!IsSealedApiKey(stored)) {
		return stored;
	}
	const auto &cache = OpenedCache();
	if (const auto i = cache.find(stored); i != cache.end()) {
		return i->second;
	}
	auto result = QString();
#ifdef Q_OS_WIN
	if (stored.startsWith(DpapiPrefix())) {
		const auto blob = QByteArray::fromBase64(
			stored.mid(DpapiPrefix().size()).toLatin1());
		if (const auto opened = blob.isEmpty()
				? std::optional<QByteArray>()
				: Unprotect(blob)) {
			result = QString::fromUtf8(*opened);
		}
	} else {
		LOG(("Lumina Error: an API key sealed by another system "
			"reads as unset here."));
	}
#else // Q_OS_WIN
	LOG(("Lumina Error: an API key sealed on Windows reads as unset here."));
#endif // Q_OS_WIN
	Remember(stored, result);
	return result;
}

} // namespace Lumina
