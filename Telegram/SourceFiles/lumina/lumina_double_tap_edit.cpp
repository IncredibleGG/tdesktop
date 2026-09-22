/*
This file is part of LuminaGram,
a fork of Telegram Desktop.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "lumina/lumina_double_tap_edit.h"

#include "lumina/lumina_settings.h"

#include <QtCore/QJsonValue>

namespace Lumina {
namespace {

const auto kKey = u"doubleTapEdit"_q;

} // namespace

bool DoubleTapEditEnabled() {
	return Settings::Instance().getBool(kKey, false);
}

rpl::producer<bool> DoubleTapEditEnabledValue() {
	return rpl::single(
		rpl::empty
	) | rpl::then(
		Settings::Instance().changesFor(kKey)
	) | rpl::map([] {
		return DoubleTapEditEnabled();
	});
}

void SetDoubleTapEditEnabled(bool value) {
	Settings::Instance().set(kKey, value, Store::Prefs);
}

} // namespace Lumina
