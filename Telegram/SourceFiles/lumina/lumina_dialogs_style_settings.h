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

// The chat-list sub-page row for `compactChatList`, which Android keeps under
// LuminaChatListActivity's density header. One row and its own subsection
// title; toggling it re-lays out every open chat list immediately.
void AddCompactListRows(
	not_null<Ui::VerticalLayout*> container,
	not_null<Window::SessionController*> controller);

// The chat-list sub-page rows for `chatListPreviewLines`: a 1 / 2 / 3 radio
// list choosing how many lines of the last-message preview each row shows.
// Changing it re-lays out every open chat list immediately, no restart.
void AddPreviewLinesRows(
	not_null<Ui::VerticalLayout*> container,
	not_null<Window::SessionController*> controller);

} // namespace Lumina
