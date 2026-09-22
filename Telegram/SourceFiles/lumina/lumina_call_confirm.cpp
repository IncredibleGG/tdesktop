/*
This file is part of LuminaGram,
a fork of Telegram Desktop.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "lumina/lumina_call_confirm.h"

#include "lumina/lumina_settings.h"

#include <QtCore/QJsonValue>

namespace Lumina {
namespace {

const auto kKey = u"confirmStartCall"_q;

} // namespace

bool ConfirmCallStart() {
	return Settings::Instance().getBool(kKey, false);
}

rpl::producer<bool> ConfirmCallStartValue() {
	return rpl::single(
		rpl::empty
	) | rpl::then(
		Settings::Instance().changesFor(kKey)
	) | rpl::map([] {
		return ConfirmCallStart();
	});
}

void SetConfirmCallStart(bool value) {
	Settings::Instance().set(kKey, value, Store::Prefs);
}

} // namespace Lumina
