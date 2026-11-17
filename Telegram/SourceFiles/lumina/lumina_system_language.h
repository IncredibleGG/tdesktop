/*
This file is part of LuminaGram,
a fork of Telegram Desktop.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "base/basic_types.h"
#include "lang/lang_instance.h"

#include <QtCore/QString>

namespace MTP {
class Sender;
} // namespace MTP

namespace Lumina {

// The interface language a first launch starts in.
//
// Stock Telegram Desktop starts in English and only offers the language the
// server suggests ("Continue in ..." on the intro). The server picks that
// suggestion from the system language it is sent, and for Chinese it gets the
// script wrong: a Traditional Chinese system such as zh-Hant-MY (Malaysia)
// was offered Simplified Chinese. LuminaGram decides on its own, the way the
// Android app does in LocaleController: the user's first preferred system
// language picks the language pack, and only until the user picks one.
//
//   * Chinese goes by SCRIPT, not region: a tag with Hant (or the legacy
//     Windows "CHT") is Traditional, Hans ("CHS") is Simplified, whatever the
//     region. Only a tag without a script falls back to the region - TW, HK
//     and MO are Traditional, everything else (CN, SG, MY, none) Simplified.
//     The packs are the Telegram translations platform's Chinese
//     (Traditional) "zh-hant" and Chinese (Simplified) "zh-hans"; the server
//     is asked for them with langpack.getLanguage and answers with the id it
//     keeps them under, which is what gets stored.
//   * Every other language gets its own pack from the server's language list
//     (langpack.getLanguages): the language itself first ("de"), then
//     language and region ("pt-br"), as Android matches its locale. No pack -
//     or English - means the app stays in English.
//
// It never overrides a choice. SystemLanguageMayApply() is false as soon as a
// language was ever saved (picked in Settings, the intro link, a "Continue in
// English" click, a .strings file, or the "ignored the suggestion" default
// that Step::createSession() saves) and while any account is logged in.

// The language tag of the user's first preferred system UI language.
[[nodiscard]] QString PreferredSystemLanguage();

// "zh-hant" / "zh-hans" for a Chinese language tag, empty for any other.
[[nodiscard]] QString ChineseLanguagePackFor(const QString &languageTag);

// True while the interface language is still LuminaGram's to pick.
[[nodiscard]] bool SystemLanguageMayApply();

// Looks up the cloud pack for the system language and calls `done` with it,
// or with an empty Language when there is none, it is English or a request
// failed. `done` is called exactly once, unless `api` is destroyed first (the
// requests die with it). A found pack (or "none") is remembered for the rest
// of the run; a failure is not, so a later call asks again.
void ResolveSystemLanguagePack(
	MTP::Sender &api,
	Fn<void(Lang::Language)> done);

} // namespace Lumina
