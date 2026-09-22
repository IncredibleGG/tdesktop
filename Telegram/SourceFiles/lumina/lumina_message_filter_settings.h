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

// The keyword-filter block on the LuminaGram Privacy sub-page: a row that opens
// a small keyword-list manager (add / edit / remove). The list and the filter
// itself live in lumina/lumina_message_filter.h.
void AddMessageFilterRows(
	not_null<Ui::VerticalLayout*> container,
	not_null<Window::SessionController*> controller);

} // namespace Lumina
