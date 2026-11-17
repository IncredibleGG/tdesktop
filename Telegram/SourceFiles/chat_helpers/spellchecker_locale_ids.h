/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include <QtCore/QtGlobal>

namespace Spellchecker {

inline constexpr auto kManifestIdScheme = 6;

[[nodiscard]] int LocalIdScheme();

[[nodiscard]] int ConvertLangId(int langId, int fromScheme, int toScheme);

[[nodiscard]] inline int LangIdFromManifest(int langId) {
	return ConvertLangId(langId, kManifestIdScheme, LocalIdScheme());
}

[[nodiscard]] int RecoverLangIdFromSerialized(quint64 raw, int scheme);

// LuminaGram: settings written by an app version below this one carry
// Qt 5 language ids on this platform (0: no such versions exist here).
//
// Upstream's Windows builds moved to Qt 6 in 7.3.2 (7003002) and upstream
// uses that as the threshold for every non-mac x86 build. LuminaGram's own
// builds differ: its Windows builds stayed on Qt 5 through 7.3.4 (they were
// based on upstream 7.3.1 and built without the qt6 switch), and 7.4.0 is
// the first one built with Qt 6. Reading their settings as Qt 6 ids would
// silently turn the translation target, the "do not translate" languages
// and the spellchecker dictionaries into other languages. LuminaGram's
// Linux builds have always been Qt 6 builds, like mac and ARM ones, so
// nothing they wrote needs converting.
#if defined Q_OS_WIN && !defined Q_PROCESSOR_ARM
inline constexpr auto kFirstQt6LangIdsAppVersion = 7004000;
#else // Q_OS_WIN && !Q_PROCESSOR_ARM
inline constexpr auto kFirstQt6LangIdsAppVersion = 0;
#endif // Q_OS_WIN && !Q_PROCESSOR_ARM

[[nodiscard]] inline int LangIdSchemeForAppVersion(int version) {
	return (version > 0 && version < kFirstQt6LangIdsAppVersion)
		? 5
		: kManifestIdScheme;
}

inline constexpr auto kDictionariesIdSchemePref = "dictionaries_id_scheme";

} // namespace Spellchecker
