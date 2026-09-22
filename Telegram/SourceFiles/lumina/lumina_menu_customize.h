/*
This file is part of LuminaGram,
a fork of Telegram Desktop.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include <rpl/producer.h>

#include <QtCore/QString>

namespace Lumina {

// Batch 4, #17. Per-action "hide this from the message context menu" switches.
//
// Each action defaults to SHOWN, i.e. the menu reads exactly as stock Telegram
// Desktop until the user hides something. The gate is applied at the point each
// entry is added, in both menu builders (HistoryView::AddMessageActions() for
// the list-based sections and HistoryInner::showContextMenu() for the main
// history), so a hidden action is hidden everywhere the menu is shown.
//
// This is a local display choice only: nothing about the messages changes, and
// every hidden action is still reachable through its keyboard shortcut or the
// composer. The set is deliberately a small, coherent group of the most
// commonly toggled entries rather than every possible row.
//
// Keys, all Store::Prefs, all default false (= shown):
//   menuHideReply, menuHideCopy, menuHideForward,
//   menuHidePin,   menuHideReport, menuHideSelect
enum class MenuAction {
	Reply,
	Copy,
	Forward,
	Pin,
	Report,
	Select,
};

// The preference key for one action.
[[nodiscard]] QString MenuActionKey(MenuAction action);

// True when the action should be ADDED to the menu (the default). This is the
// call the menu builders make.
[[nodiscard]] bool MenuActionShown(MenuAction action);

// True when the action is hidden. Just !MenuActionShown(); the settings UI uses
// it so its toggles read "hide ...".
[[nodiscard]] bool MenuActionHidden(MenuAction action);
[[nodiscard]] rpl::producer<bool> MenuActionHiddenValue(MenuAction action);
void SetMenuActionHidden(MenuAction action, bool hidden);

} // namespace Lumina
