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

// The "double-tap to edit" toggle on the LuminaGram Chats sub-page. One row
// over the `doubleTapEdit` key, which defaults to false. The behaviour and the
// accessor live in lumina/lumina_double_tap_edit.h.
void AddDoubleTapEditRows(
	not_null<Ui::VerticalLayout*> container,
	not_null<Window::SessionController*> controller);

} // namespace Lumina
