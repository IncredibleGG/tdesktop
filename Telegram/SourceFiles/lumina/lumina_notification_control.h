/*
This file is part of LuminaGram,
a fork of Telegram Desktop.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include <rpl/producer.h>

namespace Ui {
class VerticalLayout;
} // namespace Ui

namespace Window {
class SessionController;
} // namespace Window

namespace Lumina {

// Feature #18 - notification fine control. Two independent toggles, both
// defaulting to FALSE (off = stock Telegram behaviour, byte for byte):
//
//   `mutePinnedNotifications`        - suppress the OS notification for a
//       "X pinned a message" service message.
//   `muteMentionReplyNotifications`  - suppress the notification for a message
//       that @mentions you or replies to one of your messages.
//
// Both only ever suppress the NOTIFICATION. The message is not marked read and
// not dropped; the suppression happens inside System::skipNotification(), the
// same gate a muted chat already passes through, so nothing about read state,
// unread counters or the message itself changes.
//
// Keys live in Store::Prefs (tdata/luminagram.json). getBool() defaults to
// false, so an absent key - a fresh profile, an untouched install, a restored
// backup that predates this feature - reads as off and behaviour is unchanged.

[[nodiscard]] bool MutePinnedNotifications();
[[nodiscard]] rpl::producer<bool> MutePinnedNotificationsValue();
void SetMutePinnedNotifications(bool value);

[[nodiscard]] bool MuteMentionReplyNotifications();
[[nodiscard]] rpl::producer<bool> MuteMentionReplyNotificationsValue();
void SetMuteMentionReplyNotifications(bool value);

// The two rows on the LuminaGram privacy sub-page. Called once from
// Settings::LuminaPrivacy::setupContent().
void AddNotificationControlRows(
	not_null<Ui::VerticalLayout*> container,
	not_null<Window::SessionController*> controller);

} // namespace Lumina
