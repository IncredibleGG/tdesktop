/*
This file is part of LuminaGram,
a fork of Telegram Desktop.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "lumina/lumina_ad_shield_settings.h"

#include "data/data_peer.h"
#include "lang/lang_keys.h"
#include "lumina/lumina_ad_shield_manager.h"
#include "lumina/lumina_locale.h"
#include "settings/settings_common.h"
#include "ui/boxes/confirm_box.h"
#include "ui/layers/generic_box.h"
#include "ui/vertical_list.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/checkbox.h"
#include "ui/widgets/labels.h"
#include "ui/widgets/menu/menu_add_action_callback.h"
#include "ui/wrap/slide_wrap.h"
#include "ui/wrap/vertical_layout.h"
#include "window/window_session_controller.h"

#include "styles/style_layers.h"
#include "styles/style_menu_icons.h"
#include "styles/style_settings.h"

namespace Lumina {
namespace {

using AdShield::ChatMode;
using AdShield::Sensitivity;

// Re-emits on every AdShield preference change and every language change,
// so a label computed from both stays right.
[[nodiscard]] rpl::producer<QString> ComputedValue(Fn<QString()> compute) {
	return rpl::single(
		rpl::empty
	) | rpl::then(
		rpl::merge(AdShield::PrefsChanges(), LangChanges())
	) | rpl::map([compute = std::move(compute)] {
		return compute();
	});
}

[[nodiscard]] QString StateText(bool on) {
	return Tr(on ? u"LuminaAdShieldStateOn"_q : u"LuminaAdShieldStateOff"_q);
}

[[nodiscard]] style::margins DescriptionPadding() {
	const auto &padding = st::settingsSendTypePadding;
	const auto left = padding.left()
		+ st::settingsSendType.margin.left()
		+ st::defaultRadio.diameter
		+ st::settingsSendType.textPosition.x();
	return style::margins(left, 0, padding.right(), st::settingsSendTypeSkip);
}

// The first time the feature is turned on - globally or for one group - it
// says what it does and does not do, and only "Turn on" turns it on.
void IntroBox(not_null<Ui::GenericBox*> box, Fn<void()> accepted) {
	box->setTitle(TrValue(u"LuminaAdShieldTitle"_q));
	box->setWidth(st::boxWideWidth);
	const auto keys = {
		u"LuminaAdShieldIntroLocal"_q,
		u"LuminaAdShieldIntroNoDelete"_q,
		u"LuminaAdShieldIntroHashes"_q,
		u"LuminaAdShieldIntroToggle"_q,
		u"LuminaAdShieldIntroScope"_q,
	};
	for (const auto &key : keys) {
		box->addRow(
			object_ptr<Ui::FlatLabel>(
				box,
				TrValue(key) | rpl::map([](const QString &text) {
					return QString::fromUtf8("\xE2\x80\xA2 ") + text;
				}),
				st::boxLabel),
			style::margins(
				st::boxRowPadding.left(),
				st::boxLittleSkip,
				st::boxRowPadding.right(),
				st::boxLittleSkip));
	}
	box->addButton(TrValue(u"LuminaAdShieldIntroEnable"_q), [=] {
		AdShield::SetIntroAccepted(true);
		box->closeBox();
		if (accepted) {
			accepted();
		}
	});
	box->addButton(tr::lng_cancel(), [=] {
		box->closeBox();
	});
}

void AddToggle(
		not_null<Ui::VerticalLayout*> container,
		const QString &labelKey,
		Fn<bool()> get,
		Fn<void(bool)> set) {
	const auto button = container->add(object_ptr<Ui::SettingsButton>(
		container,
		TrValue(labelKey),
		st::settingsButtonNoIcon));
	// Follows the preference rather than only the clicks, so a restored
	// backup or a second window is reflected; the click handler below is the
	// only writer.
	button->toggleOn(rpl::single(
		get()
	) | rpl::then(AdShield::PrefsChanges() | rpl::map([=] {
		return get();
	})), true);
	button->setClickedCallback([=] {
		set(!get());
	});
}

void AddSensitivity(not_null<Ui::VerticalLayout*> container) {
	Ui::AddSkip(container);
	Ui::AddSubsectionTitle(
		container,
		TrValue(u"LuminaAdShieldSensitivity"_q));
	Ui::AddSkip(container, st::settingsSendTypeSkip);

	struct Option {
		Sensitivity value;
		QString label;
		QString info;
	};
	const auto options = std::vector<Option>{
		{
			Sensitivity::Loose,
			u"LuminaAdShieldLoose"_q,
			u"LuminaAdShieldLooseInfo"_q,
		},
		{
			Sensitivity::Standard,
			u"LuminaAdShieldStandard"_q,
			u"LuminaAdShieldStandardInfo"_q,
		},
		{
			Sensitivity::Aggressive,
			u"LuminaAdShieldAggressive"_q,
			u"LuminaAdShieldAggressiveInfo"_q,
		},
	};
	const auto group = std::make_shared<Ui::RadiobuttonGroup>(
		int(AdShield::CurrentSensitivity()));
	for (const auto &option : options) {
		const auto label = option.label;
		const auto row = container->add(
			object_ptr<Ui::Radiobutton>(
				container,
				group,
				int(option.value),
				Tr(label),
				st::settingsSendType),
			st::settingsSendTypePadding);
		LangChanges(
		) | rpl::on_next([=] {
			row->setText(Tr(label));
		}, row->lifetime());
		const auto info = container->add(
			object_ptr<Ui::FlatLabel>(
				container,
				TrValue(option.info),
				st::settingsNotificationTypeDetails),
			DescriptionPadding());
		info->setAttribute(Qt::WA_TransparentForMouseEvents);
	}

	// `syncing` keeps a value that arrives from the store (a backup, another
	// window) from being written straight back.
	const auto syncing = container->lifetime().make_state<bool>(false);
	group->setChangedCallback([=](int value) {
		if (!*syncing) {
			AdShield::SetSensitivity(Sensitivity(value));
		}
	});
	AdShield::DecisionSettingsChanges(
	) | rpl::on_next([=] {
		*syncing = true;
		group->setValue(int(AdShield::CurrentSensitivity()));
		*syncing = false;
	}, container->lifetime());
	Ui::AddSkip(container, st::settingsSendTypeSkip);
}

void ClearDataConfirm(std::shared_ptr<Ui::Show> show) {
	show->show(Ui::MakeConfirmBox({
		.text = Tr(u"LuminaAdShieldClearConfirm"_q),
		.confirmed = [=](Fn<void()> close) {
			AdShield::ClearLocalData();
			close();
			show->showToast(Tr(u"LuminaAdShieldCleared"_q));
		},
		.confirmText = Tr(u"LuminaAdShieldClearButton"_q),
		.confirmStyle = &st::attentionBoxButton,
		.title = Tr(u"LuminaAdShieldClear"_q),
	}));
}

void SettingsBox(not_null<Ui::GenericBox*> box) {
	box->setStyle(st::layerBox);
	box->setWidth(st::boxWideWidth);
	box->setTitle(TrValue(u"LuminaAdShieldTitle"_q));
	const auto content = box->verticalLayout();
	const auto show = box->uiShow();

	// The global switch. The click is handled here, not by the toggle, so
	// that it can stop at the explainer the first time.
	Ui::AddSkip(content);
	const auto main = content->add(object_ptr<Ui::SettingsButton>(
		content,
		TrValue(u"LuminaAdShieldTitle"_q),
		st::settingsButtonNoIcon));
	main->toggleOn(rpl::single(
		AdShield::GlobalEnabled()
	) | rpl::then(AdShield::ActivationChanges() | rpl::map([] {
		return AdShield::GlobalEnabled();
	})), true);
	main->setClickedCallback([=] {
		if (AdShield::GlobalEnabled()) {
			AdShield::SetGlobalEnabled(false);
		} else if (AdShield::IntroAccepted()) {
			AdShield::SetGlobalEnabled(true);
		} else {
			show->show(Box(IntroBox, [] {
				AdShield::SetGlobalEnabled(true);
			}));
		}
	});
	Ui::AddSkip(content);
	Ui::AddDividerText(content, TrValue(u"LuminaAdShieldAbout"_q));

	AddSensitivity(content);
	Ui::AddDivider(content);

	Ui::AddSkip(content);
	Ui::AddSubsectionTitle(content, TrValue(u"LuminaAdShieldExempt"_q));
	AddToggle(
		content,
		u"LuminaAdShieldExemptContacts"_q,
		AdShield::ExemptContacts,
		AdShield::SetExemptContacts);
	AddToggle(
		content,
		u"LuminaAdShieldExemptBots"_q,
		AdShield::ExemptBots,
		AdShield::SetExemptBots);
	AddToggle(
		content,
		u"LuminaAdShieldExemptReplies"_q,
		AdShield::ExemptReplies,
		AdShield::SetExemptReplies);
	Ui::AddSkip(content);
	Ui::AddDividerText(content, TrValue(u"LuminaAdShieldExemptInfo"_q));

	Ui::AddSkip(content);
	AddToggle(
		content,
		u"LuminaAdShieldTrial"_q,
		AdShield::TrialMode,
		AdShield::SetTrialMode);
	Ui::AddSkip(content);
	Ui::AddDividerText(content, TrValue(u"LuminaAdShieldTrialInfo"_q));

	Ui::AddSkip(content);
	content->add(object_ptr<Ui::SettingsButton>(
		content,
		TrValue(u"LuminaAdShieldClear"_q),
		st::settingsAttentionButton
	))->setClickedCallback([=] {
		ClearDataConfirm(show);
	});
	Ui::AddSkip(content);
	Ui::AddDividerText(content, TrValue(u"LuminaAdShieldFooter"_q));

	box->addButton(tr::lng_close(), [=] {
		box->closeBox();
	});
}

void ChatModeBox(not_null<Ui::GenericBox*> box, not_null<PeerData*> peer) {
	box->setTitle(TrValue(u"LuminaAdShieldTitle"_q));
	box->setWidth(st::boxWideWidth);
	const auto content = box->verticalLayout();
	const auto show = box->uiShow();

	const auto followText = [] {
		return Tr(
			u"LuminaAdShieldChatFollow"_q,
			StateText(AdShield::GlobalEnabled()));
	};
	const auto texts = std::vector<std::pair<ChatMode, Fn<QString()>>>{
		{ ChatMode::Default, followText },
		{ ChatMode::On, [] { return Tr(u"LuminaAdShieldChatOn"_q); } },
		{ ChatMode::Off, [] { return Tr(u"LuminaAdShieldChatOff"_q); } },
	};
	Ui::AddSkip(content, st::settingsSendTypeSkip);
	const auto group = std::make_shared<Ui::RadiobuttonGroup>(
		int(AdShield::ChatModeFor(peer)));
	for (const auto &[mode, text] : texts) {
		const auto row = content->add(
			object_ptr<Ui::Radiobutton>(
				content,
				group,
				int(mode),
				text(),
				st::settingsSendType),
			st::settingsSendTypePadding);
		rpl::merge(
			AdShield::PrefsChanges(),
			LangChanges()
		) | rpl::on_next([=, text = text] {
			row->setText(text());
		}, row->lifetime());
	}

	const auto syncing = content->lifetime().make_state<bool>(false);
	const auto sync = [=] {
		*syncing = true;
		group->setValue(int(AdShield::ChatModeFor(peer)));
		*syncing = false;
	};
	group->setChangedCallback([=](int value) {
		if (*syncing) {
			return;
		}
		const auto mode = ChatMode(value);
		if (mode == ChatMode::On && !AdShield::IntroAccepted()) {
			// Not on until the explainer is confirmed.
			sync();
			show->show(Box(IntroBox, [=] {
				AdShield::SetChatMode(peer, ChatMode::On);
			}));
			return;
		}
		AdShield::SetChatMode(peer, mode);
	});
	AdShield::ActivationChanges(
	) | rpl::on_next(sync, content->lifetime());

	// "Folded in the last 7 days: N", only when N > 0. Counted on the
	// AdShield queue; the box may be gone by the time it arrives.
	const auto folded = content->add(
		object_ptr<Ui::SlideWrap<Ui::FlatLabel>>(
			content,
			object_ptr<Ui::FlatLabel>(
				content,
				QString(),
				st::settingsNotificationTypeDetails),
			st::settingsSendTypePadding));
	folded->toggle(false, anim::type::instant);
	const auto count = content->lifetime().make_state<int>(0);
	const auto refreshCount = [=] {
		folded->entity()->setText(
			Tr(u"LuminaAdShieldChatFolded"_q, QString::number(*count)));
		folded->toggle(*count > 0, anim::type::normal);
	};
	const auto requestCount = [=] {
		AdShield::RequestFoldedCount(peer, crl::guard(folded, [=](int n) {
			*count = n;
			refreshCount();
		}));
	};
	requestCount();
	AdShield::PrefsChanges(
	) | rpl::on_next(requestCount, content->lifetime());
	LangChanges(
	) | rpl::on_next(refreshCount, content->lifetime());

	Ui::AddSkip(content, st::settingsSendTypeSkip);
	box->addButton(tr::lng_close(), [=] {
		box->closeBox();
	});
}

} // namespace

void AddAdShieldRows(
		not_null<Ui::VerticalLayout*> container,
		not_null<Window::SessionController*> controller) {
	Ui::AddSkip(container);
	Ui::AddSubsectionTitle(container, TrValue(u"LuminaAdShieldTitle"_q));
	::Settings::AddButtonWithLabel(
		container,
		TrValue(u"LuminaAdShieldTitle"_q),
		ComputedValue([] { return StateText(AdShield::GlobalEnabled()); }),
		st::settingsButtonNoIcon
	)->setClickedCallback([=] {
		controller->show(Box(SettingsBox));
	});
	Ui::AddSkip(container);
	Ui::AddDividerText(container, TrValue(u"LuminaAdShieldAbout"_q));
}

void AddAdShieldPeerMenuAction(
		not_null<Window::SessionController*> controller,
		not_null<PeerData*> peer,
		const Ui::Menu::MenuCallback &addAction) {
	if (!AdShield::ChatEligible(peer)) {
		return;
	}
	addAction(Tr(u"LuminaAdShieldTitle"_q), [=] {
		controller->show(Box(ChatModeBox, peer));
	}, &st::menuIconCollapse);
}

} // namespace Lumina
