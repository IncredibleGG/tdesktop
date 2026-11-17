/*
This file is part of LuminaGram,
a fork of Telegram Desktop.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

class PeerData;

namespace Ui {
class VerticalLayout;
} // namespace Ui

namespace Ui::Menu {
struct MenuCallback;
} // namespace Ui::Menu

namespace Window {
class SessionController;
} // namespace Window

namespace Lumina {

// "Hide repeated ads" (lumina_ad_shield_manager.h): the settings page, and
// the item of a group's own menu.

// LuminaGram -> Chats: the row that opens the feature's page (a box): the
// global switch (with the first-enable explainer), the sensitivity, the
// exemptions, the trial mode, "Clear local data", and the footer.
void AddAdShieldRows(
	not_null<Ui::VerticalLayout*> container,
	not_null<Window::SessionController*> controller);

// The chat's top-bar menu (window_peer_menu.cpp, history and topic
// sections). Adds nothing for a chat AdShield does not apply to. The item
// opens the per-group choice - follow the default / on in this group / off
// in this group - with "folded in the last 7 days: N" when N > 0.
void AddAdShieldPeerMenuAction(
	not_null<Window::SessionController*> controller,
	not_null<PeerData*> peer,
	const Ui::Menu::MenuCallback &addAction);

} // namespace Lumina
