/*
This file is part of LuminaGram,
a fork of Telegram Desktop.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "lumina/lumina_call_confirm_settings.h"

#include "lumina/lumina_call_confirm.h"
#include "lumina/lumina_locale.h"
#include "ui/vertical_list.h"
#include "ui/widgets/buttons.h"
#include "ui/wrap/vertical_layout.h"

#include "styles/style_settings.h"

namespace Lumina {

void AddCallConfirmRows(
		not_null<Ui::VerticalLayout*> container,
		not_null<Window::SessionController*>) {
	Ui::AddSkip(container);
	Ui::AddSubsectionTitle(container, TrValue(u"LuminaCallConfirmTitle"_q));

	const auto button = container->add(object_ptr<Ui::SettingsButton>(
		container,
		TrValue(u"LuminaCallConfirmRow"_q),
		st::settingsButtonNoIcon
	))->toggleOn(ConfirmCallStartValue());
	button->toggledChanges(
	) | rpl::on_next([=](bool value) {
		SetConfirmCallStart(value);
	}, button->lifetime());

	Ui::AddSkip(container);
	Ui::AddDividerText(container, TrValue(u"LuminaCallConfirmInfo"_q));
}

} // namespace Lumina
