/*
This file is part of LuminaGram,
a fork of Telegram Desktop.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "lumina/lumina_remember_folder_settings.h"

#include "lumina/lumina_remember_folder.h"
#include "lumina/lumina_locale.h"
#include "ui/vertical_list.h"
#include "ui/widgets/buttons.h"
#include "ui/wrap/vertical_layout.h"

#include "styles/style_settings.h"

namespace Lumina {

void AddRememberFolderRows(
		not_null<Ui::VerticalLayout*> container,
		not_null<Window::SessionController*>) {
	Ui::AddSkip(container);
	Ui::AddSubsectionTitle(
		container,
		TrValue(u"LuminaRememberFolderTitle"_q));

	const auto toggle = container->add(object_ptr<Ui::SettingsButton>(
		container,
		TrValue(u"LuminaRememberFolder"_q),
		st::settingsButtonNoIcon
	))->toggleOn(RememberLastFolderValue());
	toggle->toggledChanges(
	) | rpl::on_next([](bool value) {
		SetRememberLastFolder(value);
	}, toggle->lifetime());

	Ui::AddSkip(container);
	Ui::AddDividerText(
		container,
		TrValue(u"LuminaRememberFolderAbout"_q));
}

} // namespace Lumina
