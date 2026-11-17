/*
This file is part of LuminaGram,
a fork of Telegram Desktop.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "lumina/lumina_system_language.h"

#include "core/application.h"
#include "lang/lang_cloud_manager.h"
#include "lang/lang_instance.h"
#include "lang/lang_keys.h"
#include "mtproto/sender.h"

#include <QtCore/QLocale>
#include <QtCore/QStringList>

namespace Lumina {
namespace {

// The Telegram translations platform's Chinese packs (checked against
// t.me/setlanguage/<id>, which resolves them through the same langpack
// lookup: "zh-hant" is Chinese (Traditional), "zh-hans" Chinese
// (Simplified); their "-raw" / "-beta" spellings resolve to the same packs).
const auto kTraditionalChinese = u"zh-hant"_q;
const auto kSimplifiedChinese = u"zh-hans"_q;

// What a found pack (or "no pack") resolved to, for the rest of this run.
std::optional<Lang::Language> ResolvedPack;

[[nodiscard]] QStringList Subtags(const QString &languageTag) {
	return languageTag.toLower().replace('_', '-').split(
		'-',
		Qt::SkipEmptyParts);
}

// Android's LocaleController.getLocaleAlias(), for the old ISO 639 codes a
// system can still report.
[[nodiscard]] QString ModernLanguageCode(const QString &code) {
	if (code == u"in"_q) {
		return u"id"_q;
	} else if (code == u"iw"_q) {
		return u"he"_q;
	} else if (code == u"jw"_q) {
		return u"jv"_q;
	} else if (code == u"no"_q) {
		return u"nb"_q;
	} else if (code == u"tl"_q) {
		return u"fil"_q;
	} else if (code == u"ji"_q) {
		return u"yi"_q;
	}
	return code;
}

// The ids to look for in the server's language list, in Android's order:
// the language alone, then language and region ("pt-br").
[[nodiscard]] QStringList Candidates(const QString &languageTag) {
	const auto parts = Subtags(languageTag);
	if (parts.isEmpty()) {
		return {};
	}
	const auto language = ModernLanguageCode(parts.front());
	auto result = QStringList{ language };
	for (auto i = 1; i < parts.size(); ++i) {
		const auto &part = parts[i];
		// The region is two letters or three digits; a four letter part is
		// the script, which no Telegram pack id carries.
		const auto region = (part.size() == 2)
			|| (part.size() == 3 && part.front().isDigit());
		if (region) {
			result.push_back(language + '-' + part);
			break;
		}
	}
	return result;
}

void Finish(
		const Fn<void(Lang::Language)> &done,
		Lang::Language language) {
	if (Lang::LanguageIdOrDefault(language.id) == Lang::DefaultLanguageId()) {
		language = Lang::Language();
	}
	ResolvedPack = language;
	done(std::move(language));
}

void ResolveChinese(
		MTP::Sender &api,
		QStringList ids,
		Fn<void(Lang::Language)> done) {
	if (ids.isEmpty()) {
		Finish(done, Lang::Language());
		return;
	}
	const auto id = ids.takeFirst();
	// The handlers only ever run while `api` is alive: destroying a Sender
	// cancels its requests.
	const auto raw = &api;
	api.request(MTPlangpack_GetLanguage(
		MTP_string(Lang::CloudLangPackName()),
		MTP_string(id)
	)).done([=](const MTPLangPackLanguage &result) {
		if (result.data().vstrings_count().v > 0) {
			Finish(done, Lang::ParseLanguage(result));
		} else {
			ResolveChinese(*raw, ids, done);
		}
	}).fail([=](const MTP::Error &error) {
		if (error.type() == u"LANG_CODE_NOT_SUPPORTED"_q) {
			ResolveChinese(*raw, ids, done);
		} else {
			// A network or server failure says nothing about the pack, so
			// it is not remembered: the next call asks again.
			done(Lang::Language());
		}
	}).send();
}

void ResolveFromList(
		MTP::Sender &api,
		QStringList candidates,
		Fn<void(Lang::Language)> done) {
	api.request(MTPlangpack_GetLanguages(
		MTP_string(Lang::CloudLangPackName())
	)).done([=](const MTPVector<MTPLangPackLanguage> &result) {
		for (const auto &candidate : candidates) {
			for (const auto &language : result.v) {
				auto parsed = Lang::ParseLanguage(language);
				if (!parsed.id.compare(candidate, Qt::CaseInsensitive)) {
					Finish(done, std::move(parsed));
					return;
				}
			}
		}
		Finish(done, Lang::Language());
	}).fail([=] {
		done(Lang::Language());
	}).send();
}

} // namespace

QString PreferredSystemLanguage() {
	// The user's ordered preference list (AppleLanguages on macOS, the
	// preferred UI languages on Windows, LANGUAGE / LC_* on Linux), the
	// counterpart of Android's Locale.getDefault(). It keeps the script
	// ("zh-Hant-MY") that the single system locale may not.
	const auto languages = QLocale::system().uiLanguages();
	for (const auto &language : languages) {
		if (!language.isEmpty()) {
			return language;
		}
	}
	return Lang::GetInstance().systemLangCode();
}

QString ChineseLanguagePackFor(const QString &languageTag) {
	const auto parts = Subtags(languageTag);
	if (parts.isEmpty() || parts.front() != u"zh"_q) {
		return QString();
	}
	for (auto i = 1; i < parts.size(); ++i) {
		const auto &part = parts[i];
		if (part == u"hant"_q || part == u"cht"_q) {
			return kTraditionalChinese;
		} else if (part == u"hans"_q || part == u"chs"_q) {
			return kSimplifiedChinese;
		}
	}
	for (auto i = 1; i < parts.size(); ++i) {
		const auto &part = parts[i];
		if (part == u"tw"_q || part == u"hk"_q || part == u"mo"_q) {
			return kTraditionalChinese;
		}
	}
	return kSimplifiedChinese;
}

bool SystemLanguageMayApply() {
	if (!Core::IsAppLaunched()) {
		return false;
	}
	const auto &instance = Lang::GetInstance();
	return instance.id().isEmpty()
		&& !instance.isCustom()
		&& Core::App().canApplyLangPackWithoutRestart();
}

void ResolveSystemLanguagePack(
		MTP::Sender &api,
		Fn<void(Lang::Language)> done) {
	Expects(done != nullptr);

	if (ResolvedPack) {
		done(*ResolvedPack);
		return;
	}
	const auto system = PreferredSystemLanguage();
	const auto chinese = ChineseLanguagePackFor(system);
	if (!chinese.isEmpty()) {
		ResolveChinese(
			api,
			QStringList{ chinese, chinese + u"-raw"_q },
			std::move(done));
		return;
	}
	const auto candidates = Candidates(system);
	if (candidates.isEmpty()
		|| candidates.front() == Lang::DefaultLanguageId()) {
		Finish(done, Lang::Language());
		return;
	}
	ResolveFromList(api, candidates, std::move(done));
}

} // namespace Lumina
