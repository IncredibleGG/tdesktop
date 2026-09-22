/*
This file is part of LuminaGram,
a fork of Telegram Desktop.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "lumina/lumina_message_filter_settings.h"

#include "lang/lang_keys.h"
#include "lumina/lumina_locale.h"
#include "lumina/lumina_message_filter.h"
#include "settings/settings_common.h"
#include "ui/layers/generic_box.h"
#include "ui/ui_utility.h"
#include "ui/vertical_list.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/fields/input_field.h"
#include "ui/wrap/vertical_layout.h"
#include "window/window_session_controller.h"

#include "styles/style_layers.h"
#include "styles/style_menu_icons.h"
#include "styles/style_settings.h"

namespace Lumina {
namespace {

constexpr auto kPreviewMaxLength = 48;

[[nodiscard]] rpl::producer<QString> LabelValue(Fn<QString()> compute) {
	return rpl::single(
		rpl::empty
	) | rpl::then(
		rpl::merge(MessageFilterChanges(), LangChanges())
	) | rpl::map([compute = std::move(compute)] {
		return compute();
	});
}

[[nodiscard]] QString Preview(const QString &text) {
	auto result = text;
	result.replace(QChar(u'\n'), QChar(u' '));
	result.replace(QChar(u'\r'), QChar(u' '));
	result = result.trimmed();
	if (result.size() > kPreviewMaxLength) {
		return result.left(kPreviewMaxLength) + QChar(0x2026);
	}
	return result;
}

// Writes through Lumina::SetMessageFilterKeywords() and nothing else; the list
// behind it rebuilds from MessageFilterChanges().
void EditKeywordBox(not_null<Ui::GenericBox*> box, QString original) {
	const auto adding = original.isEmpty();
	box->setTitle(TrValue(adding
		? u"LuminaMessageFilterAdd"_q
		: u"LuminaMessageFilterEdit"_q));

	const auto field = box->addRow(object_ptr<Ui::InputField>(
		box,
		st::defaultInputField,
		Ui::InputField::Mode::SingleLine,
		TrValue(u"LuminaMessageFilterHint"_q),
		original));
	field->setMaxLength(kMessageFilterKeywordMaxLength);

	box->setFocusCallback([=] {
		field->setFocusFast();
	});

	const auto save = [=] {
		const auto text = field->getLastText().trimmed();
		if (text.isEmpty()) {
			field->showError();
			return;
		}
		auto list = MessageFilterKeywords();
		if (adding) {
			list.push_back(text);
		} else {
			const auto i = ranges::find(list, original);
			if (i != end(list)) {
				*i = text;
			} else {
				list.push_back(text);
			}
		}
		SetMessageFilterKeywords(std::move(list));
		box->closeBox();
	};
	field->submits() | rpl::on_next([=] { save(); }, field->lifetime());

	box->addButton(tr::lng_settings_save(), save);
	box->addButton(tr::lng_cancel(), [=] {
		box->closeBox();
	});
	if (!adding) {
		box->addLeftButton(tr::lng_box_delete(), [=] {
			auto list = MessageFilterKeywords();
			const auto i = ranges::find(list, original);
			if (i != end(list)) {
				list.erase(i);
			}
			SetMessageFilterKeywords(std::move(list));
			box->closeBox();
		}, st::attentionBoxButton);
	}
}

void KeywordsBox(not_null<Ui::GenericBox*> box) {
	box->setStyle(st::layerBox);
	box->setWidth(st::boxWideWidth);
	box->setTitle(TrValue(u"LuminaMessageFilterTitle"_q));

	const auto content = box->verticalLayout()->add(
		object_ptr<Ui::VerticalLayout>(box));

	const auto rebuild = std::make_shared<Fn<void()>>();
	*rebuild = [=] {
		const auto width = content->width();
		content->clear();

		const auto keywords = MessageFilterKeywords();
		const auto full = (int(keywords.size()) >= kMessageFilterMaxKeywords);

		Ui::AddSkip(content);
		if (!full) {
			const auto add = ::Settings::AddButtonWithIcon(
				content,
				TrValue(u"LuminaMessageFilterAdd"_q),
				st::settingsButtonActive,
				{ &st::menuIconAdd });
			add->setClickedCallback([=] {
				box->uiShow()->show(Box(EditKeywordBox, QString()));
			});
		}
		for (const auto &keyword : keywords) {
			const auto button = ::Settings::AddButtonWithLabel(
				content,
				rpl::single(Preview(keyword)),
				rpl::single(QString()),
				st::settingsButtonNoIcon);
			button->setClickedCallback([=] {
				box->uiShow()->show(Box(EditKeywordBox, keyword));
			});
		}
		Ui::AddSkip(content);
		Ui::AddDividerText(content, TrValue(keywords.empty()
			? u"LuminaMessageFilterEmpty"_q
			: full
			? u"LuminaMessageFilterFull"_q
			: u"LuminaMessageFilterListInfo"_q));
		content->resizeToWidth(width);
	};
	(*rebuild)();

	rpl::merge(
		MessageFilterChanges(),
		LangChanges()
	) | rpl::on_next([=] {
		Ui::PostponeCall(content, [=] {
			(*rebuild)();
		});
	}, content->lifetime());

	box->addButton(tr::lng_close(), [=] {
		box->closeBox();
	});
}

} // namespace

void AddMessageFilterRows(
		not_null<Ui::VerticalLayout*> container,
		not_null<Window::SessionController*> controller) {
	Ui::AddSkip(container);
	Ui::AddSubsectionTitle(container, TrValue(u"LuminaMessageFilterTitle"_q));

	::Settings::AddButtonWithLabel(
		container,
		TrValue(u"LuminaMessageFilterTitle"_q),
		LabelValue([] {
			const auto count = int(MessageFilterKeywords().size());
			return count
				? QString::number(count)
				: Tr(u"LuminaMessageFilterNone"_q);
		}),
		st::settingsButtonNoIcon
	)->setClickedCallback([=] {
		controller->show(Box(KeywordsBox));
	});

	Ui::AddSkip(container);
	Ui::AddDividerText(container, TrValue(u"LuminaMessageFilterInfo"_q));
}

} // namespace Lumina
