/*
This file is part of LuminaGram,
a fork of Telegram Desktop.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "lumina/lumina_message_display_settings.h"

#include "lumina/lumina_locale.h"
#include "lumina/lumina_message_display.h"
#include "ui/vertical_list.h"
#include "ui/widgets/buttons.h"
#include "ui/wrap/vertical_layout.h"

#include "styles/style_settings.h"

namespace Lumina {

void AddMessageDisplayRows(
		not_null<Ui::VerticalLayout*> container,
		not_null<Window::SessionController*>) {
	Ui::AddSkip(container);
	Ui::AddSubsectionTitle(container, TrValue(u"LuminaBubbleGatesTitle"_q));

	// Every row is toggled from the store rather than only from its own click,
	// so a value that arrived through a backup import, or was changed in a
	// second window, is reflected here.
	const auto hideReactions = container->add(object_ptr<Ui::SettingsButton>(
		container,
		TrValue(u"LuminaHideReactions"_q),
		st::settingsButtonNoIcon
	))->toggleOn(HideReactionsValue());
	hideReactions->toggledChanges(
	) | rpl::on_next([](bool value) {
		SetHideReactions(value);
	}, hideReactions->lifetime());
	Ui::AddSkip(container);
	Ui::AddDividerText(container, TrValue(u"LuminaHideReactionsInfo"_q));

	Ui::AddSkip(container);
	const auto widePosts = container->add(object_ptr<Ui::SettingsButton>(
		container,
		TrValue(u"LuminaWideChannelPosts"_q),
		st::settingsButtonNoIcon
	))->toggleOn(WideChannelPostsValue());
	widePosts->toggledChanges(
	) | rpl::on_next([](bool value) {
		SetWideChannelPosts(value);
	}, widePosts->lifetime());
	Ui::AddSkip(container);
	Ui::AddDividerText(container, TrValue(u"LuminaWideChannelPostsInfo"_q));

	Ui::AddSkip(container);
	const auto stickerTime = container->add(object_ptr<Ui::SettingsButton>(
		container,
		TrValue(u"LuminaStickerTimestamp"_q),
		st::settingsButtonNoIcon
	))->toggleOn(StickerTimestampValue());
	stickerTime->toggledChanges(
	) | rpl::on_next([](bool value) {
		SetStickerTimestamp(value);
	}, stickerTime->lifetime());
	Ui::AddSkip(container);
	Ui::AddDividerText(container, TrValue(u"LuminaStickerTimestampInfo"_q));
}

} // namespace Lumina
