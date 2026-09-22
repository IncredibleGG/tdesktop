/*
This file is part of LuminaGram,
a fork of Telegram Desktop.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "lumina/lumina_double_tap_edit_settings.h"

#include "lumina/lumina_double_tap_edit.h"
#include "lumina/lumina_locale.h"
#include "ui/vertical_list.h"
#include "ui/widgets/buttons.h"
#include "ui/wrap/vertical_layout.h"

#include "styles/style_settings.h"

namespace Lumina {

void AddDoubleTapEditRows(
		not_null<Ui::VerticalLayout*> container,
		not_null<Window::SessionController*>) {
	Ui::AddSkip(container);
	Ui::AddSubsectionTitle(container, TrValue(u"LuminaDoubleTapEditTitle"_q));

	const auto button = container->add(object_ptr<Ui::SettingsButton>(
		container,
		TrValue(u"LuminaDoubleTapEditRow"_q),
		st::settingsButtonNoIcon
	))->toggleOn(DoubleTapEditEnabledValue());
	button->toggledChanges(
	) | rpl::on_next([=](bool value) {
		SetDoubleTapEditEnabled(value);
	}, button->lifetime());

	Ui::AddSkip(container);
	Ui::AddDividerText(container, TrValue(u"LuminaDoubleTapEditInfo"_q));
}

} // namespace Lumina
