/*
This file is part of LuminaGram,
a fork of Telegram Desktop.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "lumina/lumina_menu_customize_settings.h"

#include "lumina/lumina_locale.h"
#include "lumina/lumina_menu_customize.h"
#include "ui/vertical_list.h"
#include "ui/widgets/buttons.h"
#include "ui/wrap/vertical_layout.h"

#include "styles/style_settings.h"

namespace Lumina {
namespace {

void AddOne(
		not_null<Ui::VerticalLayout*> container,
		MenuAction action,
		const QString &labelKey) {
	const auto button = container->add(object_ptr<Ui::SettingsButton>(
		container,
		TrValue(labelKey),
		st::settingsButtonNoIcon
	))->toggleOn(MenuActionHiddenValue(action));
	button->toggledChanges(
	) | rpl::on_next([=](bool value) {
		SetMenuActionHidden(action, value);
	}, button->lifetime());
}

} // namespace

void AddMenuCustomizeRows(
		not_null<Ui::VerticalLayout*> container,
		not_null<Window::SessionController*>) {
	Ui::AddSkip(container);
	Ui::AddSubsectionTitle(container, TrValue(u"LuminaMenuCustomizeTitle"_q));

	AddOne(container, MenuAction::Reply, u"LuminaMenuHideReply"_q);
	AddOne(container, MenuAction::Copy, u"LuminaMenuHideCopy"_q);
	AddOne(container, MenuAction::Forward, u"LuminaMenuHideForward"_q);
	AddOne(container, MenuAction::Pin, u"LuminaMenuHidePin"_q);
	AddOne(container, MenuAction::Report, u"LuminaMenuHideReport"_q);
	AddOne(container, MenuAction::Select, u"LuminaMenuHideSelect"_q);

	Ui::AddSkip(container);
	Ui::AddDividerText(container, TrValue(u"LuminaMenuCustomizeInfo"_q));
}

} // namespace Lumina
