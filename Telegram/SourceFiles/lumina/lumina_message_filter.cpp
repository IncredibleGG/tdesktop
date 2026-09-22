/*
This file is part of LuminaGram,
a fork of Telegram Desktop.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "lumina/lumina_message_filter.h"

#include "base/flat_set.h"
#include "lumina/lumina_settings.h"

#include <QtCore/QJsonArray>
#include <QtCore/QJsonValue>

namespace Lumina {
namespace {

const auto kKey = u"messageFilterKeywords"_q;

// A cache of the parsed keyword list, so the hot isHidden() path never has to
// parse JSON. Loaded, and its refresh subscription installed, on the first
// query - which happens at layout / paint time, long after QApplication exists,
// never during static initialisation.
struct Cache {
	std::vector<QString> keywords;
	bool ready = false;
	rpl::lifetime lifetime;
};

[[nodiscard]] Cache &Data() {
	static auto result = Cache();
	return result;
}

void Reload() {
	auto &cache = Data();
	cache.keywords.clear();
	const auto array = Settings::Instance().getArray(kKey);
	for (const auto &value : array) {
		const auto text = value.toString().trimmed();
		if (!text.isEmpty()) {
			cache.keywords.push_back(text);
		}
	}
}

void EnsureReady() {
	auto &cache = Data();
	if (cache.ready) {
		return;
	}
	cache.ready = true;
	Reload();
	Settings::Instance().changesFor(
		kKey
	) | rpl::on_next([] {
		Reload();
	}, cache.lifetime);
}

} // namespace

bool MessageFilterActive() {
	EnsureReady();
	return !Data().keywords.empty();
}

bool MessageTextFiltered(const QString &text) {
	if (text.isEmpty()) {
		return false;
	}
	EnsureReady();
	const auto &keywords = Data().keywords;
	for (const auto &keyword : keywords) {
		if (text.contains(keyword, Qt::CaseInsensitive)) {
			return true;
		}
	}
	return false;
}

std::vector<QString> MessageFilterKeywords() {
	EnsureReady();
	return Data().keywords;
}

void SetMessageFilterKeywords(std::vector<QString> keywords) {
	auto array = QJsonArray();
	auto seen = base::flat_set<QString>();
	for (auto &keyword : keywords) {
		auto text = keyword.trimmed();
		if (text.isEmpty()) {
			continue;
		} else if (text.size() > kMessageFilterKeywordMaxLength) {
			text = text.left(kMessageFilterKeywordMaxLength);
		}
		if (!seen.emplace(text.toLower()).second) {
			continue;
		} else if (array.size() >= kMessageFilterMaxKeywords) {
			break;
		}
		array.append(text);
	}
	Settings::Instance().set(kKey, array, Store::Prefs);
}

rpl::producer<> MessageFilterChanges() {
	return Settings::Instance().changesFor(kKey);
}

} // namespace Lumina
