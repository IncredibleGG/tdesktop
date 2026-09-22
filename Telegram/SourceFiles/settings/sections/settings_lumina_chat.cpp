/*
This file is part of LuminaGram,
a fork of Telegram Desktop.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "settings/sections/settings_lumina_chat.h"

#include "lumina/lumina_quick_replies_settings.h"
#include "lumina/lumina_undo_send_settings.h"
#include "lumina/lumina_select_author.h"
#include "lumina/lumina_message_actions_settings.h"
#include "lumina/lumina_ai_editor.h"
#include "lumina/lumina_link_preview_settings.h"
#include "lumina/lumina_voice_confirm_settings.h"
#include "lumina/lumina_call_confirm_settings.h"
#include "lumina/lumina_double_tap_edit_settings.h"
#include "lumina/lumina_menu_customize_settings.h"
#include "lumina/lumina_text_replace_settings.h"
#include "lumina/lumina_photo_quality.h"
#include "lumina/lumina_locale.h"
#include "lumina/lumina_settings.h"
#include "ui/widgets/buttons.h"
#include "ui/wrap/vertical_layout.h"
#include "ui/rp_widget.h"
#include "ui/vertical_list.h"

#include "styles/style_settings.h"

#include <QtCore/QJsonValue>

namespace Settings {
namespace {

void AddToggle(
		not_null<Ui::VerticalLayout*> container,
		rpl::producer<QString> label,
		const QString &key) {
	const auto button = container->add(object_ptr<Ui::SettingsButton>(
		container,
		std::move(label),
		st::settingsButtonNoIcon
	))->toggleOn(rpl::single(Lumina::Settings::Instance().getBool(key)));
	button->toggledChanges(
	) | rpl::on_next([=](bool value) {
		Lumina::Settings::Instance().set(key, value);
	}, button->lifetime());
}

// Batch 3, #10: input-row detail toggles. Each defaults to false, i.e. the
// button keeps its stock visibility until the user opts in.
void AddInputRowToggles(not_null<Ui::VerticalLayout*> container) {
	Ui::AddSkip(container);
	Ui::AddSubsectionTitle(
		container,
		Lumina::TrValue(u"LuminaInputRowTitle"_q));
	AddToggle(
		container,
		Lumina::TrValue(u"LuminaHideVoiceButton"_q),
		u"hideVoiceRecordButton"_q);
	AddToggle(
		container,
		Lumina::TrValue(u"LuminaHideSendAsButton"_q),
		u"hideSendAsButton"_q);
	Ui::AddSkip(container);
	Ui::AddDividerText(
		container,
		Lumina::TrValue(u"LuminaInputRowInfo"_q));
}

} // namespace

Type LuminaChatId() {
	return LuminaChat::Id();
}

LuminaChat::LuminaChat(
	QWidget *parent,
	not_null<Window::SessionController*> controller)
: Section(parent, controller) {
	const auto content = Ui::CreateChild<Ui::VerticalLayout>(this);
	setupContent(content);
	Ui::ResizeFitChild(this, content);
}

LuminaChat::~LuminaChat() = default;

rpl::producer<QString> LuminaChat::title() {
	return Lumina::TrValue(u"LuminaChatSettings"_q);
}

// F-02 sub-page contract: apart from the one row below, which already ships
// and is already wired (`HistoryItem::forbidsSaving`), this body stays an
// ordered list of one call per feature, each declared in that feature's own
// `lumina/*.h`.
// Planned owners, in the order their rows should appear:
//   W4-B forward without author / without captions, save to Saved Messages,
//        message details;
//   W4-C select all messages from this author;
//   W4-E undo-send window;
//   W4-F confirm before sending voice / video messages.
void LuminaChat::setupContent(not_null<Ui::VerticalLayout*> container) {
	Lumina::AddAiEditorRows(container, controller());
	Lumina::AddTextReplaceRows(container, controller());
	Lumina::AddVoiceConfirmRows(container, controller());
	Lumina::AddCallConfirmRows(container, controller());
	Lumina::AddLinkPreviewRows(container, controller());
	Lumina::AddMessageActionsRows(container, controller());
	Lumina::AddDoubleTapEditRows(container, controller());
	Lumina::AddMenuCustomizeRows(container, controller());
	Lumina::AddSelectAuthorRows(container, controller());
	Lumina::AddUndoSendRows(container, controller());
	Lumina::AddReplyTemplatesRows(container, controller());
	Lumina::AddPhotoQualityRows(container);
	AddInputRowToggles(container);
}

} // namespace Settings
