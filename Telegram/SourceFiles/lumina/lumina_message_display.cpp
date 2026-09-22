/*
This file is part of LuminaGram,
a fork of Telegram Desktop.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "lumina/lumina_message_display.h"

#include "lumina/lumina_settings.h"

#include <QtCore/QJsonValue>

namespace Lumina {
namespace {

const auto kKeyHideReactions = u"hideReactions"_q;
const auto kKeyWideChannelPosts = u"wideChannelPosts"_q;
const auto kKeyStickerTimestamp = u"stickerTimestamp"_q;

[[nodiscard]] rpl::producer<bool> BoolValue(
		const QString &key,
		bool (*read)()) {
	return rpl::single(
		rpl::empty
	) | rpl::then(
		Settings::Instance().changesFor(key)
	) | rpl::map([=] {
		return read();
	});
}

} // namespace

bool HideReactions() {
	return Settings::Instance().getBool(kKeyHideReactions, false);
}

void SetHideReactions(bool value) {
	Settings::Instance().set(kKeyHideReactions, value, Store::Prefs);
}

rpl::producer<bool> HideReactionsValue() {
	return BoolValue(kKeyHideReactions, &HideReactions);
}

bool WideChannelPosts() {
	return Settings::Instance().getBool(kKeyWideChannelPosts, false);
}

void SetWideChannelPosts(bool value) {
	Settings::Instance().set(kKeyWideChannelPosts, value, Store::Prefs);
}

rpl::producer<bool> WideChannelPostsValue() {
	return BoolValue(kKeyWideChannelPosts, &WideChannelPosts);
}

bool StickerTimestamp() {
	return Settings::Instance().getBool(kKeyStickerTimestamp, false);
}

void SetStickerTimestamp(bool value) {
	Settings::Instance().set(kKeyStickerTimestamp, value, Store::Prefs);
}

rpl::producer<bool> StickerTimestampValue() {
	return BoolValue(kKeyStickerTimestamp, &StickerTimestamp);
}

} // namespace Lumina
