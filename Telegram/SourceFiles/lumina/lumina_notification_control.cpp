/*
This file is part of LuminaGram,
a fork of Telegram Desktop.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "lumina/lumina_notification_control.h"

#include "lumina/lumina_locale.h"
#include "lumina/lumina_settings.h"
#include "ui/vertical_list.h"
#include "ui/widgets/buttons.h"
#include "ui/wrap/vertical_layout.h"

#include "styles/style_settings.h"

#include <rpl/rpl.h>

namespace Lumina {
namespace {

const auto kPinnedKey = u"mutePinnedNotifications"_q;
const auto kMentionKey = u"muteMentionReplyNotifications"_q;

[[nodiscard]] rpl::producer<bool> BoolValue(const QString &key) {
	return rpl::single(
		rpl::empty
	) | rpl::then(
		Settings::Instance().changesFor(key)
	) | rpl::map([key] {
		return Settings::Instance().getBool(key);
	});
}

} // namespace

bool MutePinnedNotifications() {
	return Settings::Instance().getBool(kPinnedKey);
}

rpl::producer<bool> MutePinnedNotificationsValue() {
	return BoolValue(kPinnedKey);
}

void SetMutePinnedNotifications(bool value) {
	Settings::Instance().set(kPinnedKey, value, Store::Prefs);
}

bool MuteMentionReplyNotifications() {
	return Settings::Instance().getBool(kMentionKey);
}

rpl::producer<bool> MuteMentionReplyNotificationsValue() {
	return BoolValue(kMentionKey);
}

void SetMuteMentionReplyNotifications(bool value) {
	Settings::Instance().set(kMentionKey, value, Store::Prefs);
}

void AddNotificationControlRows(
		not_null<Ui::VerticalLayout*> container,
		not_null<Window::SessionController*>) {
	Ui::AddSkip(container);
	Ui::AddSubsectionTitle(
		container,
		TrValue(u"LuminaNotifyControlSubtitle"_q));

	const auto pinned = container->add(object_ptr<Ui::SettingsButton>(
		container,
		TrValue(u"LuminaNotifyMutePinned"_q),
		st::settingsButtonNoIcon
	))->toggleOn(MutePinnedNotificationsValue());
	pinned->toggledChanges(
	) | rpl::on_next([](bool value) {
		SetMutePinnedNotifications(value);
	}, pinned->lifetime());
	Ui::AddSkip(container);
	Ui::AddDividerText(
		container,
		TrValue(u"LuminaNotifyMutePinnedInfo"_q));

	Ui::AddSkip(container);
	const auto mentions = container->add(object_ptr<Ui::SettingsButton>(
		container,
		TrValue(u"LuminaNotifyMuteMentions"_q),
		st::settingsButtonNoIcon
	))->toggleOn(MuteMentionReplyNotificationsValue());
	mentions->toggledChanges(
	) | rpl::on_next([](bool value) {
		SetMuteMentionReplyNotifications(value);
	}, mentions->lifetime());
	Ui::AddSkip(container);
	Ui::AddDividerText(
		container,
		TrValue(u"LuminaNotifyMuteMentionsInfo"_q));
}

} // namespace Lumina
