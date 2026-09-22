/*
This file is part of LuminaGram,
a fork of Telegram Desktop.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "base/basic_types.h"

namespace Ui {
class VerticalLayout;
} // namespace Ui

namespace Window {
class SessionController;
} // namespace Window

namespace Lumina {

// The three message-display gate rows on the LuminaGram appearance sub-page, in
// the F-02 sub-page shape: settings/sections/settings_lumina_appearance.cpp
// holds this one call and nothing else for this feature.
//
// Three toggles, all default off: hide reactions, wide channel posts and the
// sticker timestamp. What each one actually gates lives in
// lumina/lumina_message_display.h.
void AddMessageDisplayRows(
	not_null<Ui::VerticalLayout*> container,
	not_null<Window::SessionController*> controller);

} // namespace Lumina
