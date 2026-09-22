/*
This file is part of LuminaGram,
a fork of Telegram Desktop.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "lumina/lumina_menu_customize.h"

#include "lumina/lumina_settings.h"

#include <QtCore/QJsonValue>

namespace Lumina {

QString MenuActionKey(MenuAction action) {
	switch (action) {
	case MenuAction::Reply: return u"menuHideReply"_q;
	case MenuAction::Copy: return u"menuHideCopy"_q;
	case MenuAction::Forward: return u"menuHideForward"_q;
	case MenuAction::Pin: return u"menuHidePin"_q;
	case MenuAction::Report: return u"menuHideReport"_q;
	case MenuAction::Select: return u"menuHideSelect"_q;
	}
	return QString();
}

bool MenuActionHidden(MenuAction action) {
	return Settings::Instance().getBool(MenuActionKey(action), false);
}

bool MenuActionShown(MenuAction action) {
	return !MenuActionHidden(action);
}

rpl::producer<bool> MenuActionHiddenValue(MenuAction action) {
	const auto key = MenuActionKey(action);
	return rpl::single(
		rpl::empty
	) | rpl::then(
		Settings::Instance().changesFor(key)
	) | rpl::map([action] {
		return MenuActionHidden(action);
	});
}

void SetMenuActionHidden(MenuAction action, bool hidden) {
	Settings::Instance().set(MenuActionKey(action), hidden, Store::Prefs);
}

} // namespace Lumina
