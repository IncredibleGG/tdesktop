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

// The "confirm before starting a call" toggle on the LuminaGram Chats sub-page.
// One row over the `confirmStartCall` key, which defaults to false. The
// accessor lives in lumina/lumina_call_confirm.h, and calls/calls_instance.cpp
// is what reads it.
void AddCallConfirmRows(
	not_null<Ui::VerticalLayout*> container,
	not_null<Window::SessionController*> controller);

} // namespace Lumina
