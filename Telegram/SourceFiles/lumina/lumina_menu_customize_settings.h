/*
This file is part of LuminaGram,
a fork of Telegram Desktop.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

namespace Ui {
class VerticalLayout;
} // namespace Ui

namespace Window {
class SessionController;
} // namespace Window

namespace Lumina {

// The "message menu" customization block on the LuminaGram Chats sub-page: one
// "hide ..." toggle per Lumina::MenuAction, each defaulting to off (= the
// action stays visible). The switches themselves live in
// lumina/lumina_menu_customize.h.
void AddMenuCustomizeRows(
	not_null<Ui::VerticalLayout*> container,
	not_null<Window::SessionController*> controller);

} // namespace Lumina
