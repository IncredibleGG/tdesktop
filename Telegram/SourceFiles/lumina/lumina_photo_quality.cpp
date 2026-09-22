/*
This file is part of LuminaGram,
a fork of Telegram Desktop.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "lumina/lumina_photo_quality.h"

#include "lumina/lumina_locale.h"
#include "lumina/lumina_settings.h"
#include "ui/vertical_list.h"
#include "ui/widgets/checkbox.h"
#include "ui/wrap/vertical_layout.h"

#include "styles/style_settings.h"

#include <QtCore/QJsonValue>

#include <algorithm>
#include <atomic>

namespace Lumina {
namespace {

const auto kKeyOutgoingPhotoQuality = u"outgoingPhotoQuality"_q;

// The worker-thread mirror of the preference. See lumina_photo_quality.h for
// why the file loader reads this instead of the store.
std::atomic<int> Cached/* = */{ kOutgoingPhotoQualityDefault };

[[nodiscard]] int Clamp(int value) {
	return std::clamp(
		value,
		kOutgoingPhotoQualityMin,
		kOutgoingPhotoQualityMax);
}

[[nodiscard]] QString ChoiceText(int percent) {
	const auto number = QString::number(percent);
	return (percent == kOutgoingPhotoQualityDefault)
		? Tr(u"LuminaPhotoQualityChoiceDefault"_q, number)
		: Tr(u"LuminaPhotoQualityChoice"_q, number);
}

} // namespace

int OutgoingPhotoQuality() {
	return Clamp(Settings::Instance().getInt(
		kKeyOutgoingPhotoQuality,
		kOutgoingPhotoQualityDefault));
}

void SetOutgoingPhotoQuality(int value) {
	Settings::Instance().set(
		kKeyOutgoingPhotoQuality,
		Clamp(value),
		Store::Prefs);
	RefreshOutgoingPhotoQualityCache();
}

rpl::producer<int> OutgoingPhotoQualityValue() {
	return rpl::single(
		rpl::empty
	) | rpl::then(
		Settings::Instance().changesFor(kKeyOutgoingPhotoQuality)
	) | rpl::map([] {
		return OutgoingPhotoQuality();
	});
}

void RefreshOutgoingPhotoQualityCache() {
	Cached.store(OutgoingPhotoQuality());
}

int OutgoingPhotoQualityCached() {
	return Cached.load();
}

void AddPhotoQualityRows(not_null<Ui::VerticalLayout*> container) {
	Ui::AddSkip(container);
	Ui::AddSubsectionTitle(
		container,
		TrValue(u"LuminaPhotoQualityHeader"_q));
	Ui::AddSkip(container, st::settingsSendTypeSkip);

	const auto group = std::make_shared<Ui::RadiobuttonGroup>(
		OutgoingPhotoQuality());
	for (const auto percent : kOutgoingPhotoQualityChoices) {
		const auto row = container->add(
			object_ptr<Ui::Radiobutton>(
				container,
				group,
				percent,
				ChoiceText(percent),
				st::settingsSendType),
			st::settingsSendTypePadding);
		LangChanges(
		) | rpl::on_next([=] {
			row->setText(ChoiceText(percent));
		}, row->lifetime());
	}

	// The preference can also move under the page - a restored backup writes
	// every key it carries - so the rows follow the preference rather than
	// only the clicks. `syncing` keeps that path from writing the value back.
	const auto syncing = container->lifetime().make_state<bool>(false);
	group->setChangedCallback([=](int value) {
		if (*syncing) {
			return;
		}
		SetOutgoingPhotoQuality(value);
	});
	OutgoingPhotoQualityValue(
	) | rpl::on_next([=](int percent) {
		*syncing = true;
		group->setValue(percent);
		*syncing = false;
	}, container->lifetime());

	Ui::AddSkip(container, st::settingsSendTypeSkip);
	Ui::AddDividerText(container, TrValue(u"LuminaPhotoQualityInfo"_q));
}

} // namespace Lumina
