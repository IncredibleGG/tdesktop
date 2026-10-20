/*
This file is part of LuminaGram,
a fork of Telegram Desktop.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "lumina/lumina_explain.h"

#include "data/data_media_types.h"
#include "data/data_peer.h"
#include "data/data_session.h"
#include "history/history.h"
#include "history/history_item.h"
#include "history/view/history_view_element.h"
#include "lang/lang_keys.h"
#include "lumina/lumina_http_guard.h"
#include "lumina/lumina_locale.h"
#include "lumina/lumina_message_filter.h"
#include "lumina/lumina_message_menu.h"
#include "lumina/lumina_settings.h"
#include "lumina/lumina_translate_gating.h" // AutomaticProcessingBlocked.
#include "lumina/lumina_translate_providers.h"
#include "lumina/lumina_translate_readlang.h"
#include "lumina/lumina_translate_settings.h" // DetectLlmModels.
#include "main/main_session.h"
#include "ui/layers/generic_box.h"
#include "ui/text/text_entity.h"
#include "ui/widgets/labels.h"
#include "ui/widgets/popup_menu.h"
#include "ui/wrap/slide_wrap.h"
#include "window/window_session_controller.h"

#include "styles/style_layers.h"
#include "styles/style_menu_icons.h"

#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QJsonParseError>
#include <QtCore/QUrl>
#include <QtNetwork/QNetworkAccessManager>
#include <QtNetwork/QNetworkReply>
#include <QtNetwork/QNetworkRequest>

namespace Lumina {
namespace {

// Android's key name, unchanged, so a settings backup means the same thing on
// both platforms.
const auto kKeyEnabled = u"explainMessage"_q;

// The same 15s ceiling every LuminaGram HTTP call uses
// (lumina/lumina_translate_providers.cpp): a dead endpoint must not leave the
// box stuck on "Explaining..." forever.
constexpr auto kRequestTimeoutMs = 15000;

// The conversation that goes with the message being explained, so that the
// model can tell what "it" or "that" points at, what a running joke refers
// to, or that a short reply is ironic - a single message read on its own can
// only ever be explained word by word.
//
// Up to this many messages from just before it in the same chat...
constexpr auto kContextMessagesMax = 8;
// ...holding at most this much text together; the oldest go first.
constexpr auto kContextCharsMax = 1500;
// No single message may use up the whole budget: a wall of text right above
// would otherwise push out everything else, so each one is cut to this.
constexpr auto kContextMessageCharsMax = 500;
// How many loaded messages are looked through to find them. A forum topic or
// a busy group can have many in between that belong to another topic or
// carry no text at all.
constexpr auto kContextScanMax = 200;

// The language the user READS. Same resolution the rest of the fork uses (see
// lumina/lumina_voice_to_text.cpp): the LuminaGram read-language override
// first, then the interface language. Handed to the prompt as an English name
// ("Traditional Chinese") because that is what an LLM prompt reads best with.
[[nodiscard]] QString ReadingLanguageName() {
	const auto stored = NormalizeLanguageCode(ReadLanguageCode());
	const auto code = stored.isEmpty() ? InterfaceLanguageCode() : stored;
	return LanguageEnglishName(code);
}

// The instruction, kept in code exactly as the translation prompt is
// (DefaultLlmPrompt()): it is an instruction to the model, not something the
// user reads, and the {lang} placeholder is what makes the model answer in the
// reader's own language rather than in English.
//
// Without context it is word for word what it always was. With context it
// says what the extra messages are and how to use them, and that only the
// marked TARGET is to be explained.
[[nodiscard]] QString ExplainPrompt(
		const QString &languageName,
		bool withContext) {
	auto result = QString(u"You are a cross-cultural communication assistant. "_q);
	if (withContext) {
		result += u"The user received the chat message marked TARGET below "
			"and wants to understand it. Earlier messages from the same "
			"conversation come with it as context. Speakers are anonymised: "
			"\"Me\" is the user, and A, B, C... are other people. Use the "
			"context to work out slang, references, irony, and what words "
			"like \"it\" or \"that\" refer to; if the context is irrelevant, "
			"ignore it. Explain only the TARGET message. Break it down into "
			"exactly these four short, clearly labelled sections, in this "
			"order:\n"_q;
	} else {
		result += u"The user received the chat message below and wants to "
			"understand it. Break it down into exactly these four short, "
			"clearly labelled sections, in this order:\n"_q;
	}
	result += u"1. Literal meaning: what the words say, plainly.\n"
		"2. Actual tone: the real intent, mood or subtext behind the "
		"words.\n"
		"3. Cultural and slang notes: any idioms, slang, memes, references "
		"or cultural context a non-native reader could miss; write a brief "
		"\"none\" if there is nothing notable.\n"
		"4. How to reply: one or two natural ways the user could respond.\n"
		"Keep every section concise. Do not repeat the whole message back. "
		"Write the entire answer, including the section headings, in "
		"{lang}."_q;
	return result.replace(u"{lang}"_q, languageName);
}

// choices[0].message.content of an OpenAI-compatible chat completion, trimmed,
// or empty for anything unparseable. Same shape LlmEngine::translate() reads.
[[nodiscard]] QString ParseContent(const QByteArray &body) {
	auto error = QJsonParseError{ 0, QJsonParseError::NoError };
	const auto document = QJsonDocument::fromJson(body, &error);
	if (error.error != QJsonParseError::NoError || !document.isObject()) {
		return QString();
	}
	const auto choices = document.object().value(u"choices"_q).toArray();
	if (choices.isEmpty()) {
		return QString();
	}
	const auto message = choices.at(0).toObject().value(
		u"message"_q).toObject();
	return message.value(u"content"_q).toString().trimmed();
}

// The text a message contributes as context, on one line, or empty when it
// contributes none: a service message, media without a caption, a
// self-destructing (view-once) media, and anything the user's own keyword
// filter hides from the chat - what they chose not to see is not sent.
[[nodiscard]] QString ContextText(not_null<HistoryItem*> item) {
	if (item->isService() || item->isSponsored()) {
		return QString();
	} else if (const auto media = item->media()) {
		if (media->ttlSeconds()) {
			return QString();
		}
	}
	const auto &original = item->originalText().text;
	if (original.trimmed().isEmpty() || MessageTextFiltered(original)) {
		return QString();
	}
	auto result = original.simplified();
	if (result.size() > kContextMessageCharsMax) {
		result = result.left(kContextMessageCharsMax);
		if (!result.isEmpty()
			&& result.at(result.size() - 1).isHighSurrogate()) {
			result.chop(1);
		}
		result = result.trimmed() + QChar(0x2026);
	}
	return result;
}

// The messages just before `target` in its own conversation that carry text,
// nearest first. "Its own conversation" is the topic in a forum and the
// sublist in a channel's direct messages, not the whole chat.
//
// They are read from what is loaded: the blocks of the chat the message is
// shown in, or - for a message shown somewhere else, like a forum topic - the
// channel's sequential message ids. Nothing is requested from the server.
[[nodiscard]] std::vector<not_null<HistoryItem*>> PrecedingItems(
		not_null<HistoryItem*> target) {
	const auto history = target->history();
	const auto forum = history->isForum();
	const auto monoforum = history->peer->isMonoforum();
	const auto topicRootId = forum ? target->topicRootId() : MsgId();
	const auto sublistPeerId = monoforum ? target->sublistPeerId() : PeerId();

	auto result = std::vector<not_null<HistoryItem*>>();
	const auto consider = [&](HistoryItem *item) {
		if (!item
			|| (item == target.get())
			|| (item->history() != history)
			|| (forum && item->topicRootId() != topicRootId)
			|| (monoforum && item->sublistPeerId() != sublistPeerId)
			|| ContextText(item).isEmpty()) {
			return;
		}
		result.push_back(item);
	};
	const auto enough = [&] {
		return int(result.size()) >= kContextMessagesMax;
	};
	auto scanned = 0;
	if (const auto view = target->mainView()) {
		for (auto i = view->previousInBlocks()
			; i && !enough() && (scanned < kContextScanMax)
			; i = i->previousInBlocks()) {
			++scanned;
			consider(i->data());
		}
	} else if (history->peer->isChannel() && target->isRegular()) {
		const auto &owner = history->owner();
		const auto peerId = history->peer->id;
		for (auto id = target->id - 1
			; (id > MsgId(0)) && !enough() && (scanned < kContextScanMax)
			; --id) {
			++scanned;
			consider(owner.message(peerId, id));
		}
	}
	return result;
}

// "Me" for the user, a letter for everybody else - the same letter for the
// same sender throughout one request, in the order they first speak. No name,
// username, phone number or id ever reaches the request.
class SpeakerLabels final {
public:
	[[nodiscard]] QString label(not_null<HistoryItem*> item) {
		if (item->out() || item->from()->isSelf()) {
			return u"Me"_q;
		}
		const auto id = item->from()->id;
		auto i = _labels.find(id);
		if (i == _labels.end()) {
			const auto index = int(_labels.size());
			auto label = (index < 26)
				? QString(QChar(ushort('A' + index)))
				: (u"P"_q + QString::number(index + 1));
			i = _labels.emplace(id, std::move(label)).first;
		}
		return i->second;
	}

private:
	base::flat_map<PeerId, QString> _labels;

};

// What is sent for one Explain: the user message, and whether it carries
// context (which picks the prompt).
struct ExplainInput {
	QString text;
	bool withContext = false;
};

// The message alone, exactly as before, in a chat whose content must not
// reach a third party except by the user pointing at it: the user's own Saved
// Messages, Telegram's service and official accounts, BotFather, support
// (Lumina::AutomaticProcessingBlocked(), the list the earlier privacy fix
// introduced). Explaining a message there is still the user's choice; sending
// its neighbours along is not. Desktop has no secret chats.
//
// Everywhere else: the message the target replies to, when it is loaded and
// has text, and up to kContextMessagesMax messages from just before it, within
// kContextCharsMax characters. The replied-to message is paid for first,
// because it is what the target answers; then the nearest messages, until the
// next one no longer fits - so it is always the oldest that are dropped.
[[nodiscard]] ExplainInput BuildExplainInput(not_null<HistoryItem*> item) {
	const auto target = ExplainMessageText(item);
	auto result = ExplainInput{ .text = target };
	if (target.isEmpty() || AutomaticProcessingBlocked(item->history()->peer)) {
		return result;
	}

	HistoryItem *replied = nullptr;
	if (const auto replyId = item->replyToFullId(); replyId.msg) {
		const auto candidate = item->history()->owner().message(replyId);
		if (candidate
			&& (candidate != item.get())
			&& !AutomaticProcessingBlocked(candidate->history()->peer)
			&& !ContextText(candidate).isEmpty()) {
			replied = candidate;
		}
	}

	auto budget = kContextCharsMax;
	if (replied) {
		const auto size = int(ContextText(replied).size());
		if (size > budget) {
			replied = nullptr;
		} else {
			budget -= size;
		}
	}
	auto kept = std::vector<not_null<HistoryItem*>>(); // Nearest first.
	auto repliedKept = false;
	for (const auto &other : PrecedingItems(item)) {
		if (other.get() == replied) {
			// Already paid for above.
			repliedKept = true;
			kept.push_back(other);
			continue;
		}
		const auto size = int(ContextText(other).size());
		if (size > budget) {
			break;
		}
		budget -= size;
		kept.push_back(other);
	}
	const auto repliedApart = (replied && !repliedKept) ? replied : nullptr;
	if (kept.empty() && !repliedApart) {
		return result;
	}
	ranges::reverse(kept); // Oldest first, the order they were written in.

	// Letters go out in reading order: the replied-to message (written
	// before the others), then the context, then the target.
	auto labels = SpeakerLabels();
	auto text = QString();
	if (repliedApart) {
		text += u"Earlier message that the TARGET replies to:\n"_q
			+ labels.label(repliedApart)
			+ u": "_q
			+ ContextText(repliedApart)
			+ u"\n\n"_q;
	}
	if (!kept.empty()) {
		text += u"Messages just before the TARGET, oldest first:\n"_q;
		for (const auto &other : kept) {
			text += labels.label(other);
			if (other.get() == replied) {
				text += u" (the TARGET replies to this)"_q;
			}
			text += u": "_q + ContextText(other) + QChar('\n');
		}
		text += QChar('\n');
	}
	text += u"TARGET message (explain this one):\n"_q
		+ labels.label(item)
		+ u": "_q
		+ target;
	return { .text = text, .withContext = true };
}

struct State {
	rpl::variable<QString> status;
	rpl::variable<QString> result;

	// True after the service answered that the configured model does not
	// exist there: the box then offers "Detect available models".
	rpl::variable<bool> offerDetect = false;
	bool detecting = false;

	// What is sent, built once when the box opens, so that a retry after a
	// new model is picked sends exactly the same thing.
	ExplainInput input;

	// Declared LAST so it is destroyed FIRST when the box closes: its
	// destructor aborts the reply below, and the abort must happen while the
	// variables it would write to are still alive.
	QNetworkAccessManager network;
};

// The one and only LLM call. It reuses the translation LLM's key, base URL and
// model (lumina/lumina_translate_providers.h) rather than storing any of its
// own, and it builds the request the same way LlmEngine::translate() does. It
// never logs: the body carries private messages and the header carries the
// user's key.
void RequestExplain(not_null<Ui::GenericBox*> box, not_null<State*> state) {
	const auto key = ProviderApiKey(LlmProviderId());
	if (key.isEmpty()) {
		// No key: the promise at the top of the header. Say where to add one,
		// and stop - never crash, never fall through to a request with no
		// Authorization header.
		state->status = Tr(u"LuminaExplainNoKey"_q);
		return;
	}
	auto baseUrl = LlmBaseUrl();
	while (baseUrl.endsWith(QChar('/'))) {
		baseUrl.chop(1);
	}
	if (baseUrl.isEmpty()) {
		baseUrl = DefaultLlmBaseUrl();
	}
	const auto url = QUrl(baseUrl + u"/chat/completions"_q);
	if (!url.isValid()) {
		state->status = TranslateFailureText(
			{ .error = TranslateError::Network },
			true);
		return;
	}
	const auto model = LlmModel();
	const auto &input = state->input;
	const auto body = QJsonDocument(QJsonObject{
		{ u"model"_q, model },
		{ u"temperature"_q, 0.3 },
		{ u"messages"_q, QJsonArray{
			QJsonObject{
				{ u"role"_q, u"system"_q },
				{ u"content"_q, ExplainPrompt(
					ReadingLanguageName(),
					input.withContext) },
			},
			QJsonObject{
				{ u"role"_q, u"user"_q },
				{ u"content"_q, input.text },
			},
		} },
	}).toJson(QJsonDocument::Compact);

	auto request = QNetworkRequest(url);
	request.setTransferTimeout(kRequestTimeoutMs);
	request.setRawHeader("Authorization", "Bearer " + key.toUtf8());
	request.setRawHeader("Content-Type", "application/json");
	// The key never follows a redirect to another host or down to http.
	// See lumina/lumina_http_guard.h.
	KeyedRequest(request);

	const auto reply = state->network.post(request, body);
	WatchKeyedRedirects(reply, url);
	QObject::connect(reply, &QNetworkReply::finished, crl::guard(box, [=] {
		// A refused redirect is a transport failure, not an answer: its 3xx
		// status must not read as anything the service said.
		const auto status = KeyedRedirectRefused(reply)
			? 0
			: reply->attribute(
				QNetworkRequest::HttpStatusCodeAttribute).toInt();
		const auto failure = reply->error();
		auto received = reply->readAll();
		reply->deleteLater();
		if (status >= 400 || failure != QNetworkReply::NoError) {
			// The same reasons the translation test row gives. The reply
			// body is only classified (does the model exist?) and dropped.
			const auto missing = LlmModelNotFound(status, received, model);
			state->offerDetect = missing;
			state->status = TranslateFailureText({
				.error = TranslateError::Network,
				.httpStatus = (status >= 400) ? status : 0,
				.missingModel = missing ? model : QString(),
			}, true);
			return;
		}
		const auto content = ParseContent(received);
		if (content.isEmpty()) {
			state->status = Tr(u"LuminaExplainError"_q);
			return;
		}
		state->status = QString();
		state->result = content;
	}));
}

void ExplainBox(
		not_null<Ui::GenericBox*> box,
		not_null<Window::SessionController*> controller,
		FullMsgId itemId) {
	box->setTitle(TrValue(u"LuminaExplainTitle"_q));

	const auto state = box->lifetime().make_state<State>();

	// Each section is slide-wrapped rather than emptied, so the box grows from
	// "Explaining..." to the answer without leaving a hole, and the status
	// line disappears once the answer is in.
	const auto addText = [&](not_null<rpl::variable<QString>*> value) {
		const auto wrap = box->addRow(
			object_ptr<Ui::SlideWrap<Ui::FlatLabel>>(
				box,
				object_ptr<Ui::FlatLabel>(
					box,
					value->value(),
					st::boxLabel)));
		wrap->toggleOn(value->value() | rpl::map([](const QString &text) {
			return !text.isEmpty();
		}));
		wrap->finishAnimating();
		return wrap->entity();
	};

	addText(&state->status);

	// "Detect available models", under the reason, only after the service
	// said the model does not exist. Picking one saves it on the translation
	// page and explains the same message again with it.
	const auto detect = box->addRow(
		object_ptr<Ui::SlideWrap<Ui::FlatLabel>>(
			box,
			object_ptr<Ui::FlatLabel>(
				box,
				TrValue(u"LuminaLlmDetectModels"_q) | rpl::map([](
						const QString &text) {
					return tr::link(text);
				}),
				st::boxLabel)));
	detect->toggleOn(state->offerDetect.value());
	detect->finishAnimating();
	detect->entity()->setClickHandlerFilter([=](
			const ClickHandlerPtr &,
			Qt::MouseButton button) {
		if (button != Qt::LeftButton || state->detecting) {
			return false;
		}
		state->detecting = true;
		DetectLlmModels(
			box->uiShow(),
			crl::guard(box, [=](QString) {
				state->offerDetect = false;
				state->result = QString();
				state->status = Tr(u"LuminaExplainWorking"_q);
				RequestExplain(box, state);
			}),
			crl::guard(box, [=] {
				state->detecting = false;
			}));
		return false;
	});

	addText(&state->result)->setSelectable(true);

	box->addButton(tr::lng_box_ok(), [=] {
		box->closeBox();
	});

	const auto show = controller->uiShow();
	box->addLeftButton(tr::lng_mediaview_copy(), [=] {
		const auto text = state->result.current();
		if (text.isEmpty()) {
			return;
		}
		TextUtilities::SetClipboardText(TextForMimeData::Simple(text));
		show->showToast(tr::lng_text_copied(tr::now));
	});

	// Resolved again here, not captured from the menu: a message can be
	// deleted while the menu is open.
	const auto session = &controller->session();
	const auto item = session->data().message(itemId);
	if (!item || ExplainMessageText(item).isEmpty()) {
		state->status = Tr(u"LuminaExplainError"_q);
		return;
	}
	state->input = BuildExplainInput(item);
	state->status = Tr(u"LuminaExplainWorking"_q);
	RequestExplain(box, state);
}

} // namespace

bool ExplainMessageEnabled() {
	// Default ON, exactly as Android's LuminaExplain defaults it. The row is
	// still absent on a message with no text, so an untouched profile only
	// ever sees it where it could actually do something.
	return Settings::Instance().getBool(kKeyEnabled, true);
}

void SetExplainMessageEnabled(bool value) {
	Settings::Instance().set(kKeyEnabled, value);
}

rpl::producer<> ExplainMessageEnabledChanges() {
	return Settings::Instance().changesFor(kKeyEnabled);
}

QString ExplainMessageText(HistoryItem *item) {
	return item ? item->originalText().text.trimmed() : QString();
}

void ShowExplainMessage(
		not_null<Window::SessionController*> controller,
		not_null<HistoryItem*> item) {
	controller->show(Box(ExplainBox, controller, item->fullId()));
}

void AddExplainMenuRow(
		not_null<Ui::PopupMenu*> menu,
		const MessageMenuContext &context) {
	if (!ExplainMessageEnabled() || context.hasSelection) {
		return;
	}
	const auto item = context.item;

	// The one guard: no text, no row - on a photo, a sticker or a voice note
	// as much as on an empty item.
	if (ExplainMessageText(item).isEmpty()) {
		return;
	}

	// Unlike voice-to-text, the row is NOT hidden when no LLM key is set: the
	// spec is that clicking it then explains, in the box, that a key has to be
	// added on the translation page. The row is where a user discovers the
	// feature, and hiding it would hide the instruction with it.
	const auto session = &item->history()->session();
	const auto itemId = item->fullId();
	const auto controller = context.controller;

	// The two menu builders own the menu they hand us, so they outlive it and
	// the raw capture is safe; the message is resolved again on click because
	// it can be deleted while the menu is open.
	menu->addAction(Tr(u"LuminaExplainMenuItem"_q), crl::guard(controller, [=] {
		if (const auto item = session->data().message(itemId)) {
			ShowExplainMessage(controller, item);
		}
	}), &st::menuIconInfo);
}

} // namespace Lumina
