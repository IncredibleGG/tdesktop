/*
This file is part of LuminaGram,
a fork of Telegram Desktop.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "lumina/lumina_ad_fold.h"

#include "base/call_delayed.h"
#include "base/unique_qptr.h"
#include "base/weak_ptr.h"
#include "data/data_groups.h"
#include "data/data_peer.h"
#include "data/data_session.h"
#include "history/history.h"
#include "history/history_inner_widget.h" // HistoryMainElementDelegateMixin
#include "history/history_item.h"
#include "history/history_item_helpers.h" // PreparedServiceText
#include "history/view/history_view_cursor_state.h"
#include "history/view/history_view_element.h"
#include "history/view/history_view_list_widget.h"
#include "history/view/history_view_message.h"
#include "history/view/history_view_service_message.h"
#include "history/view/media/history_view_media.h"
#include "lang/lang_keys.h"
#include "lumina/lumina_ad_shield_manager.h"
#include "lumina/lumina_locale.h"
#include "lumina/lumina_message_filter.h"
#include "main/main_session.h"
#include "ui/chat/chat_style.h"
#include "ui/click_handler.h"
#include "ui/painter.h"
#include "ui/text/text_utilities.h"
#include "ui/widgets/popup_menu.h"
#include "window/window_session_controller.h"

#include <QtCore/QPointer>

#include "styles/style_chat.h"
#include "styles/style_menu_icons.h"
#include "styles/style_widgets.h"

namespace Lumina::AdShield {
namespace {

using HistoryView::Context;
using HistoryView::Element;
using HistoryView::ElementChatMode;
using HistoryView::ElementDelegate;
using HistoryView::PaintContext;
using HistoryView::PointState;
using HistoryView::SelectedQuote;
using HistoryView::StateRequest;
using HistoryView::TextState;

// Ordinary rows that are hidden themselves (the keyword filter, album parts
// other than the first, empty messages) are looked through when deciding
// whether two fold rows are consecutive - but only this many in a row.
constexpr auto kLookThroughLimit = 64;

// The most fold rows one segment counts or expands at once.
constexpr auto kSegmentLimit = 4096;

// "Not an ad" from an expanded header: the messages stay expanded (without
// the header) until their cluster comes back unfolded, at most this long.
constexpr auto kNotAdTimeout = crl::time(5000);

// "All new messages folded": how long after the chat was shown the
// decisions of its new messages may still arrive.
constexpr auto kToastWindow = crl::time(6000);

// A middle dot between the parts of the expanded header, with a space on
// both sides; right after a full-width bracket (Chinese), only after it.
[[nodiscard]] QString Separator(const QString &after) {
	const auto dot = QString(QChar(0x00B7)) + QChar(' ');
	return after.endsWith(QChar(0xFF09)) ? dot : (QChar(' ') + dot);
}

class FoldRow;

[[nodiscard]] const FoldRow *AsFold(const Element *view);

// The row `delta` places away from `view` in the list that owns it, hidden
// rows included: the history blocks, or the rows of a ListWidget.
[[nodiscard]] Element *Near(not_null<const Element*> view, int delta) {
	if (view->block()) {
		return (delta < 0) ? view->previousInBlocks() : view->nextInBlocks();
	} else if (const auto list = dynamic_cast<HistoryView::ListWidget*>(
			view->delegate().get())) {
		return list->luminaAdFoldRow(view->data(), delta);
	}
	return nullptr;
}

// The view of `item` in the same list as `sibling`.
[[nodiscard]] Element *ViewInSameList(
		not_null<const Element*> sibling,
		not_null<const HistoryItem*> item) {
	if (sibling->block()) {
		return item->mainView();
	} else if (const auto list = dynamic_cast<HistoryView::ListWidget*>(
			sibling->delegate().get())) {
		return list->luminaAdFoldRow(item, 0);
	}
	return nullptr;
}

// An ordinary row that does not separate two fold rows.
[[nodiscard]] bool SeeThrough(not_null<const Element*> view) {
	return view->data()->isEmpty() || view->isHidden();
}

// The first row in that direction that is not looked through: a shown
// ordinary row, or a fold row (head or member). Album parts, keyword-
// filtered and empty rows are looked through, at most kLookThroughLimit.
[[nodiscard]] Element *Solid(not_null<const Element*> view, int delta);

// The message and, for an album, all of its parts.
[[nodiscard]] std::vector<MsgId> WithAlbum(not_null<const HistoryItem*> item) {
	auto result = std::vector<MsgId>();
	if (const auto group = item->history()->owner().groups().find(item)) {
		result.reserve(group->items.size());
		for (const auto &part : group->items) {
			result.push_back(part->id);
		}
	} else {
		result.push_back(item->id);
	}
	return result;
}

class FoldRow final : public Element {
public:
	FoldRow(
		not_null<ElementDelegate*> delegate,
		not_null<HistoryItem*> data,
		Element *replacing);

	// Hidden for the reasons any row is (an album part, the keyword
	// filter): such a fold row is looked through like an ordinary one.
	[[nodiscard]] bool hiddenAnyway() const {
		return Element::isHidden();
	}

	// The fold row that makes this one a member of its segment, if any.
	[[nodiscard]] const FoldRow *previousInSegment() const;
	[[nodiscard]] const FoldRow *nextInSegment() const;
	[[nodiscard]] const FoldRow *segmentHead() const;

	// The head's line: counts the whole segment.
	[[nodiscard]] QString computeLabel() const;

	int marginTop() const override;
	int marginBottom() const override;
	bool isHidden() const override;
	void draw(Painter &p, const PaintContext &context) const override;
	PointState pointState(QPoint point) const override;
	TextState textState(
		QPoint point,
		StateRequest request) const override;
	void updatePressed(QPoint point) override;
	TextForMimeData selectedText(TextSelection selection) const override;
	SelectedQuote selectedQuote(TextSelection selection) const override;
	TextSelection selectionFromQuote(
		const SelectedQuote &quote) const override;
	TextSelection adjustSelection(
		TextSelection selection,
		TextSelectType type) const override;
	QRect innerGeometry() const override;
	void animateReaction(Ui::ReactionFlyAnimationArgs &&args) override;

private:
	[[nodiscard]] QRect countGeometry() const;
	[[nodiscard]] QRect bubbleGeometry(QRect g) const;
	void refreshLabel();

	QSize performCountOptimalSize() override;
	QSize performCountCurrentSize(int newWidth) override;

	Ui::Text::String _label;
	QString _labelText;
	ClickHandlerPtr _link;

};

const FoldRow *AsFold(const Element *view) {
	return dynamic_cast<const FoldRow*>(view);
}

[[nodiscard]] std::vector<MsgId> SegmentIds(not_null<const FoldRow*> head) {
	auto result = std::vector<MsgId>();
	auto row = head.get();
	for (auto i = 0; row && i != kSegmentLimit; ++i) {
		const auto ids = WithAlbum(row->data());
		result.insert(end(result), begin(ids), end(ids));
		row = row->nextInSegment();
	}
	return result;
}

// What a message of the main list is shown as.
enum class Kind : uchar {
	Stock,
	Fold,
	Trial,
	Header,
};

// Runtime state of the fold UI: which messages have special views, the
// expanded segments with a header, pending "not an ad" marks and the
// one-time toast. Main thread only; deliberately leaked like the manager.
class FoldUi final {
public:
	[[nodiscard]] static FoldUi &Instance();

	[[nodiscard]] Kind desiredKind(not_null<HistoryItem*> item) const;
	void remember(not_null<HistoryItem*> item, Kind kind);
	[[nodiscard]] bool special(not_null<const HistoryItem*> item) const;
	void rememberList(not_null<HistoryView::ListWidget*> list);
	[[nodiscard]] bool hasHeader(not_null<const HistoryItem*> item) const;

	void expand(
		not_null<History*> history,
		MsgId head,
		std::vector<MsgId> ids);
	void collapse(not_null<History*> history, MsgId head);
	void notAdFromHeader(not_null<History*> history, MsgId head);
	void chatShown(
		not_null<History*> history,
		Window::SessionController *window);

private:
	struct NotAd {
		std::vector<MsgId> ids;
		crl::time deadline = 0;
	};
	// A message with a special view, and what its fold row showed.
	struct Shown {
		Kind kind = Kind::Stock;
		int occurrence = 0;
		bool replyToMe = false;
	};
	struct Chat {
		Main::Session *session = nullptr;
		base::flat_map<MsgId, Shown> special;
		base::flat_map<MsgId, std::vector<MsgId>> heads;
		base::flat_map<MsgId, NotAd> notAd;
	};
	struct Toast {
		Main::Session *session = nullptr;
		base::weak_ptr<Window::SessionController> window;
		MsgId readTill = 0;
		int unread = 0;
		crl::time deadline = 0;
	};
	enum class Verdict {
		Wait,
		No,
		Yes,
	};

	FoldUi();

	[[nodiscard]] Chat &chat(not_null<History*> history);
	void hookSession(not_null<Main::Session*> session);
	void forgetSession(not_null<Main::Session*> session);
	void apply(const DecisionsChange &change);
	// A row of a ListWidget to lay out again once every view of the batch
	// was made again (a pointer to it could be gone by then).
	struct ListRow {
		QPointer<HistoryView::ListWidget> list;
		FullMsgId itemId;
	};

	// Returns whether rows of the history blocks were marked for layout.
	bool settle(not_null<HistoryItem*> item, std::vector<ListRow> &listRows);
	void scheduleFlush(not_null<Main::Session*> session);
	void checkNotAd(not_null<History*> history);
	void checkToast(not_null<History*> history);
	[[nodiscard]] Verdict allNewFolded(
		not_null<History*> history,
		const Toast &toast) const;

	base::flat_map<not_null<History*>, Chat> _chats;
	base::flat_map<not_null<History*>, Toast> _toasts;
	base::flat_map<not_null<History*>, Main::Session*> _toasted;
	base::flat_set<not_null<Main::Session*>> _sessions;
	base::flat_set<not_null<Main::Session*>> _flushes;
	std::vector<QPointer<HistoryView::ListWidget>> _lists;
	rpl::lifetime _lifetime;

};

void ExpandSegmentOf(not_null<const FoldRow*> row) {
	const auto head = row->segmentHead();
	FoldUi::Instance().expand(
		head->history(),
		head->data()->id,
		SegmentIds(head));
}

FoldRow::FoldRow(
	not_null<ElementDelegate*> delegate,
	not_null<HistoryItem*> data,
	Element *replacing)
: Element(delegate, data, replacing, Flag(0)) {
	// A fold row never shows the message's media: drop it, so nothing is
	// prepared or loaded for it. An album keeps its grouped media object
	// (Data::Groups needs it), unpainted.
	if (!data->history()->owner().groups().find(data)) {
		overrideMedia(nullptr);
	}
	_link = std::make_shared<LambdaClickHandler>([weak = base::make_weak(
			this)] {
		if (const auto strong = weak.get()) {
			ExpandSegmentOf(strong);
		}
	});
}

Element *Solid(not_null<const Element*> view, int delta) {
	auto row = Near(view, delta);
	for (auto i = 0; row && i != kLookThroughLimit; ++i) {
		if (const auto fold = AsFold(row)) {
			if (!fold->hiddenAnyway()) {
				return row;
			}
		} else if (!SeeThrough(row)) {
			return row;
		}
		row = Near(row, delta);
	}
	return nullptr;
}

const FoldRow *FoldRow::previousInSegment() const {
	return AsFold(Solid(this, -1));
}

const FoldRow *FoldRow::nextInSegment() const {
	return AsFold(Solid(this, 1));
}

const FoldRow *FoldRow::segmentHead() const {
	auto result = this;
	for (auto i = 0; i != kSegmentLimit; ++i) {
		const auto previous = result->previousInSegment();
		if (!previous) {
			break;
		}
		result = previous;
	}
	return result;
}

QString FoldRow::computeLabel() const {
	auto units = 0;
	auto occurrence = 0;
	auto replyToMe = false;
	auto row = this;
	for (auto i = 0; row && i != kSegmentLimit; ++i) {
		// An album is one row (its first part), so it counts once.
		++units;
		if (const auto decision = CachedDecision(row->data())) {
			if (!i) {
				occurrence = decision->occurrence;
			}
			replyToMe = replyToMe || decision->replyToMe;
		}
		row = row->nextInSegment();
	}
	if (replyToMe) {
		return (units == 1)
			? Tr(u"LuminaAdShieldFoldReplyOne"_q)
			: Tr(u"LuminaAdShieldFoldReply"_q, QString::number(units));
	} else if (units == 1) {
		return Tr(
			u"LuminaAdShieldFoldOne"_q,
			QString::number(std::max(occurrence, 1)));
	}
	return Tr(u"LuminaAdShieldFoldMany"_q, QString::number(units));
}

void FoldRow::refreshLabel() {
	auto text = computeLabel();
	if (_labelText != text || _label.isEmpty()) {
		_labelText = std::move(text);
		_label.setText(st::serviceTextStyle, _labelText);
	}
}

bool FoldRow::isHidden() const {
	return hiddenAnyway() || (previousInSegment() != nullptr);
}

int FoldRow::marginTop() const {
	auto result = isHidden() ? 0 : st::msgServiceMargin.top();
	result += displayedDateHeight();
	if (const auto bar = Get<HistoryView::UnreadBar>()) {
		result += bar->height();
	}
	if (const auto bar = Get<HistoryView::ForumThreadBar>()) {
		result += bar->height();
	}
	if (const auto margins = Get<HistoryView::ViewAddedMargins>()) {
		result += margins->top;
	}
	return result;
}

int FoldRow::marginBottom() const {
	auto result = isHidden() ? 0 : st::msgServiceMargin.bottom();
	if (const auto margins = Get<HistoryView::ViewAddedMargins>()) {
		result += margins->bottom;
	}
	return result;
}

QRect FoldRow::countGeometry() const {
	auto result = QRect(0, 0, width(), height());
	if (delegate()->elementChatMode() == ElementChatMode::Wide) {
		result.setWidth(std::min(
			result.width(),
			st::msgMaxWidth
				+ 2 * st::msgPhotoSkip
				+ 2 * st::msgMargin.left()));
	}
	auto margins = st::msgServiceMargin;
	margins.setTop(marginTop());
	return result.marginsRemoved(margins);
}

QRect FoldRow::bubbleGeometry(QRect g) const {
	const auto &padding = st::msgServicePadding;
	const auto width = std::min(
		g.width(),
		_label.maxWidth() + padding.left() + padding.right());
	return QRect(
		g.left() + (g.width() - width) / 2,
		g.top(),
		width,
		g.height());
}

QSize FoldRow::performCountOptimalSize() {
	if (!isHidden()) {
		refreshLabel();
	}
	const auto &padding = st::msgServicePadding;
	return {
		_label.maxWidth() + padding.left() + padding.right(),
		_label.minHeight(),
	};
}

QSize FoldRow::performCountCurrentSize(int newWidth) {
	if (isHidden()) {
		return { newWidth, marginTop() + marginBottom() };
	}
	refreshLabel();
	auto contentWidth = newWidth;
	if (delegate()->elementChatMode() == ElementChatMode::Wide) {
		accumulate_min(
			contentWidth,
			st::msgMaxWidth + 2 * st::msgPhotoSkip + 2 * st::msgMargin.left());
	}
	const auto &padding = st::msgServicePadding;
	contentWidth -= st::msgServiceMargin.left() + st::msgServiceMargin.left();
	contentWidth = std::max(contentWidth, padding.left() + padding.right() + 1);
	const auto textWidth = contentWidth - padding.left() - padding.right();
	const auto textHeight = (textWidth >= _label.maxWidth())
		? _label.minHeight()
		: _label.countHeight(textWidth);
	return {
		newWidth,
		(marginTop()
			+ padding.top()
			+ textHeight
			+ padding.bottom()
			+ marginBottom()),
	};
}

void FoldRow::draw(Painter &p, const PaintContext &context) const {
	const auto g = countGeometry();
	if (g.width() < 1) {
		return;
	}
	if (const auto bar = Get<HistoryView::UnreadBar>()) {
		auto aboveh = 0;
		if (const auto date = Get<HistoryView::DateBadge>()) {
			aboveh += date->height();
		}
		if (const auto thread = Get<HistoryView::ForumThreadBar>()) {
			aboveh += thread->height();
		}
		const auto barh = bar->height();
		if (context.clip.intersects(QRect(0, aboveh, width(), barh))) {
			p.translate(0, aboveh);
			bar->paint(p, context, 0, width(), delegate()->elementChatMode());
			p.translate(0, -aboveh);
		}
	}
	if (isHidden()) {
		return;
	}
	paintHighlight(p, context, g.height());

	const auto st = context.st;
	const auto trect = g - st::msgServicePadding;
	p.translate(0, g.top() - st::msgServiceMargin.top());
	HistoryView::ServiceMessagePainter::PaintComplexBubble(
		p,
		st,
		g.left(),
		g.width(),
		_label,
		trect);
	p.translate(0, -g.top() + st::msgServiceMargin.top());

	p.setBrush(Qt::NoBrush);
	p.setPen(st->msgServiceFg());
	p.setFont(st::msgServiceFont);
	_label.draw(p, {
		.position = trect.topLeft(),
		.availableWidth = trect.width(),
		.align = style::al_top,
		.palette = &st->serviceTextPalette(),
		.now = context.now,
		.fullWidthSelection = false,
	});
}

PointState FoldRow::pointState(QPoint point) const {
	const auto g = countGeometry();
	if (g.width() < 1 || isHidden()) {
		return PointState::Outside;
	}
	return g.contains(point) ? PointState::Inside : PointState::Outside;
}

TextState FoldRow::textState(QPoint point, StateRequest request) const {
	auto result = TextState(data());
	const auto g = countGeometry();
	if (g.width() < 1 || isHidden()) {
		return result;
	}
	if (bubbleGeometry(g).contains(point)) {
		result.link = _link;
	}
	return result;
}

void FoldRow::updatePressed(QPoint point) {
}

TextForMimeData FoldRow::selectedText(TextSelection selection) const {
	return {};
}

SelectedQuote FoldRow::selectedQuote(TextSelection selection) const {
	return {};
}

TextSelection FoldRow::selectionFromQuote(const SelectedQuote &quote) const {
	return {};
}

TextSelection FoldRow::adjustSelection(
		TextSelection selection,
		TextSelectType type) const {
	return {};
}

QRect FoldRow::innerGeometry() const {
	return countGeometry();
}

void FoldRow::animateReaction(Ui::ReactionFlyAnimationArgs &&args) {
}

// The history list (HistoryInner) or a ListWidget showing a forum topic, a
// comments thread or a history section. Never a preview (draft options,
// gifts, chat intro...), whose delegates report the same contexts.
[[nodiscard]] bool IsMainList(
		not_null<HistoryItem*> item,
		not_null<ElementDelegate*> delegate) {
	const auto context = delegate->elementContext();
	if (context != Context::History && context != Context::Replies) {
		return false;
	} else if (delegate == item->history()->delegateMixin()->delegate()) {
		return true;
	}
	return dynamic_cast<HistoryView::ListWidget*>(delegate.get()) != nullptr;
}

[[nodiscard]] PreparedServiceText TrialMark() {
	return PreparedServiceText{
		.text = TextWithEntities{ Tr(u"LuminaAdShieldTrialMark"_q) },
	};
}

[[nodiscard]] PreparedServiceText ExpandedHeader(
		not_null<HistoryItem*> item) {
	const auto session = base::make_weak(&item->history()->session());
	const auto peerId = item->history()->peer->id;
	const auto head = item->id;
	const auto withHistory = [=](Fn<void(not_null<History*>)> action) {
		return std::make_shared<LambdaClickHandler>([=] {
			if (const auto strong = session.get()) {
				if (const auto history = strong->data().historyLoaded(
						peerId)) {
					action(history);
				}
			}
		});
	};
	const auto title = Tr(u"LuminaAdShieldExpanded"_q);
	const auto collapse = Tr(u"LuminaAdShieldCollapse"_q);
	auto text = TextWithEntities{ title + Separator(title) };
	text.append(Ui::Text::Link(collapse, 1));
	text.append(Separator(collapse));
	text.append(Ui::Text::Link(Tr(u"LuminaAdShieldNotAd"_q), 2));
	return PreparedServiceText{
		.text = std::move(text),
		.links = {
			withHistory([=](not_null<History*> history) {
				FoldUi::Instance().collapse(history, head);
			}),
			withHistory([=](not_null<History*> history) {
				FoldUi::Instance().notAdFromHeader(history, head);
			}),
		},
	};
}

FoldUi &FoldUi::Instance() {
	static const auto result = new FoldUi();
	return *result;
}

FoldUi::FoldUi() {
	DecisionsChanges(
	) | rpl::on_next([=](const DecisionsChange &change) {
		apply(change);
	}, _lifetime);
}

FoldUi::Chat &FoldUi::chat(not_null<History*> history) {
	const auto i = _chats.find(history);
	if (i != _chats.end()) {
		return i->second;
	}
	const auto session = &history->session();
	hookSession(session);
	auto &result = _chats.emplace(history, Chat()).first->second;
	result.session = session;
	return result;
}

void FoldUi::hookSession(not_null<Main::Session*> session) {
	if (!_sessions.emplace(session).second) {
		return;
	}
	// Session::clear() destroys every message; the session's end destroys
	// the histories themselves. Entries of either only ever compare the
	// pointers from here on.
	session->data().sessionDataAboutToBeCleared(
	) | rpl::on_next([=] {
		forgetSession(session);
	}, session->lifetime());
	session->lifetime().add([=] {
		forgetSession(session);
		_sessions.remove(session);
	});
}

void FoldUi::forgetSession(not_null<Main::Session*> session) {
	const auto mine = [&](const auto &pair) {
		return pair.second.session == session.get();
	};
	for (auto i = _chats.begin(); i != _chats.end();) {
		i = mine(*i) ? _chats.erase(i) : (i + 1);
	}
	for (auto i = _toasts.begin(); i != _toasts.end();) {
		i = mine(*i) ? _toasts.erase(i) : (i + 1);
	}
	for (auto i = _toasted.begin(); i != _toasted.end();) {
		i = (i->second == session.get()) ? _toasted.erase(i) : (i + 1);
	}
}

Kind FoldUi::desiredKind(not_null<HistoryItem*> item) const {
	if (ShouldFold(item)) {
		return Kind::Fold;
	} else if (ShouldMarkTrial(item)) {
		return Kind::Trial;
	} else if (hasHeader(item) && Expanded(item) && !TrialMode()) {
		const auto decision = CachedDecision(item);
		if (decision && decision->fold) {
			return Kind::Header;
		}
	}
	return Kind::Stock;
}

void FoldUi::remember(not_null<HistoryItem*> item, Kind kind) {
	if (kind == Kind::Stock) {
		const auto i = _chats.find(item->history());
		if (i != _chats.end()) {
			i->second.special.remove(item->id);
		}
		return;
	}
	auto shown = Shown{ .kind = kind };
	if (const auto decision = CachedDecision(item)) {
		shown.occurrence = decision->occurrence;
		shown.replyToMe = decision->replyToMe;
	}
	chat(item->history()).special[item->id] = shown;
}

void FoldUi::rememberList(not_null<HistoryView::ListWidget*> list) {
	_lists.erase(
		ranges::remove_if(_lists, [](const auto &list) { return !list; }),
		end(_lists));
	const auto known = ranges::any_of(_lists, [&](const auto &existing) {
		return existing.data() == list.get();
	});
	if (!known) {
		_lists.emplace_back(list.get());
	}
}

bool FoldUi::special(not_null<const HistoryItem*> item) const {
	const auto i = _chats.find(item->history());
	return (i != _chats.end()) && i->second.special.contains(item->id);
}

bool FoldUi::hasHeader(not_null<const HistoryItem*> item) const {
	const auto i = _chats.find(item->history());
	return (i != _chats.end()) && i->second.heads.contains(item->id);
}

void FoldUi::expand(
		not_null<History*> history,
		MsgId head,
		std::vector<MsgId> ids) {
	if (ids.empty()) {
		return;
	}
	auto &entry = chat(history);
	entry.notAd.remove(head);
	entry.heads[head] = ids;
	SetExpanded(history, ids, true);
}

void FoldUi::collapse(not_null<History*> history, MsgId head) {
	auto &entry = chat(history);
	const auto i = entry.heads.find(head);
	if (i == entry.heads.end()) {
		return;
	}
	const auto ids = std::move(i->second);
	entry.heads.erase(i);
	SetExpanded(history, ids, false);
}

void FoldUi::notAdFromHeader(not_null<History*> history, MsgId head) {
	auto &entry = chat(history);
	const auto i = entry.heads.find(head);
	if (i == entry.heads.end()) {
		return;
	}
	auto ids = std::move(i->second);
	entry.heads.erase(i);
	const auto item = history->owner().message(history->peer->id, head);
	if (!item) {
		SetExpanded(history, ids, false);
		return;
	}
	// Stay expanded (the header goes away) until the cluster comes back
	// unfolded; whatever is still folded then (another cluster, or a mark
	// that could not be recorded) folds again.
	entry.notAd[head] = NotAd{
		.ids = std::move(ids),
		.deadline = crl::now() + kNotAdTimeout,
	};
	MarkNotAd(item);
	history->owner().requestItemViewRefresh(item);
	const auto session = base::make_weak(&history->session());
	base::call_delayed(kNotAdTimeout + crl::time(50), [=] {
		if (session.get()) {
			FoldUi::Instance().checkNotAd(history);
		}
	});
}

void FoldUi::chatShown(
		not_null<History*> history,
		Window::SessionController *window) {
	ChatShown(history);
	if (!window
		|| _toasted.contains(history)
		|| !ChatEnabled(history->peer)
		|| !history->unreadCountKnown()
		|| history->unreadCount() <= 0) {
		return;
	}
	hookSession(&history->session());
	_toasts[history] = Toast{
		.session = &history->session(),
		.window = base::make_weak(window),
		.readTill = history->inboxReadTillId(),
		.unread = history->unreadCount(),
		.deadline = crl::now() + kToastWindow,
	};
	// Decisions cached earlier in this run may already answer it; the
	// window makes the chat its active one right after this.
	const auto session = base::make_weak(&history->session());
	crl::on_main([=] {
		if (session.get()) {
			FoldUi::Instance().checkToast(history);
		}
	});
	base::call_delayed(kToastWindow + crl::time(50), [=] {
		if (session.get()) {
			FoldUi::Instance().checkToast(history);
		}
	});
}

void FoldUi::apply(const DecisionsChange &change) {
	const auto history = change.history;
	const auto owner = &history->owner();
	const auto peerId = history->peer->id;

	auto ids = change.ids;
	auto known = base::flat_map<MsgId, Shown>();
	{
		auto &entry = chat(history);
		if (ids.empty()) {
			// The whole chat: the feature went off there, data was cleared
			// or the decisions were dropped. Every special view goes back
			// to the stock one (or is made special again by CreateView).
			for (const auto &[id, shown] : entry.special) {
				ids.push_back(id);
			}
			known = base::take(entry.special);
			entry.heads.clear();
			entry.notAd.clear();
		} else {
			for (const auto id : ids) {
				if (const auto i = entry.special.find(id)
					; i != entry.special.end()) {
					known.emplace(id, i->second);
				}
			}
		}
	}

	auto listRows = std::vector<ListRow>();
	auto blocks = false;
	for (const auto id : ids) {
		const auto item = owner->message(peerId, id);
		if (!item) {
			chat(history).special.remove(id);
			continue;
		}
		const auto i = known.find(id);
		const auto was = (i != known.end()) ? i->second : Shown();
		const auto now = desiredKind(item);
		if (now != was.kind) {
			// Made again: a fold row, the stock view, the expanded header
			// or the trial mark (CreateView() remembers which).
			owner->requestItemViewRefresh(item);
			blocks = settle(item, listRows) || blocks;
			continue;
		} else if (now == Kind::Stock) {
			continue;
		}
		// The same view: only a fold row's line may read differently (the
		// "7 days" count, a reply to me); a changed group size or any other
		// field does not show.
		remember(item, now);
		const auto decision = CachedDecision(item);
		const auto occurrence = decision ? decision->occurrence : 0;
		const auto replyToMe = decision && decision->replyToMe;
		if (now == Kind::Fold
			&& (occurrence != was.occurrence || replyToMe != was.replyToMe)) {
			blocks = settle(item, listRows) || blocks;
		}
	}

	// A ListWidget re-attaches dates and bubbles around a row it is asked
	// to lay out again; the history blocks were re-counted in settle().
	auto requested = base::flat_set<not_null<Element*>>();
	for (const auto &[list, itemId] : listRows) {
		const auto item = list ? owner->message(itemId) : nullptr;
		if (const auto row = item ? list->luminaAdFoldRow(item, 0) : nullptr) {
			if (requested.emplace(row).second) {
				row->setPendingResize();
				owner->requestViewResize(row);
			}
		}
	}
	if (blocks) {
		owner->notifyHistoryChangeDelayed(history);
		scheduleFlush(&history->session());
	}
	checkNotAd(history);
	checkToast(history);
}

bool FoldUi::settle(
		not_null<HistoryItem*> item,
		std::vector<ListRow> &listRows) {
	// After a row changed what it shows, the rows whose layout depends on
	// it are laid out again: its own segment head and the head of the
	// segment before it (they count their segment), and the next row that
	// is not looked through (it may have become a head or a member).
	const auto around = [&](
			not_null<Element*> view,
			HistoryView::ListWidget *list) {
		auto rows = std::vector<not_null<Element*>>{ view };
		const auto add = [&](const Element *row) {
			if (row) {
				const auto raw = const_cast<Element*>(row);
				if (!ranges::contains(rows, not_null<Element*>(raw))) {
					rows.push_back(raw);
				}
			}
		};
		if (const auto fold = AsFold(view)) {
			add(fold->segmentHead());
		}
		if (const auto previous = AsFold(Solid(view, -1))) {
			add(previous->segmentHead());
		}
		add(Solid(view, 1));
		for (const auto &row : rows) {
			row->setPendingResize();
		}
		if (list) {
			for (const auto &row : rows) {
				listRows.push_back({ list, row->data()->fullId() });
			}
			return;
		}
		// History blocks re-count dates, forum thread bars and bubble
		// attachments only when their neighbours change; a row that became
		// hidden or shown changes what the rows after it see.
		for (const auto &row : rows) {
			row->previousInBlocksChanged();
			auto next = row->nextInBlocks();
			for (auto i = 0; next && i != kLookThroughLimit; ++i) {
				next->previousInBlocksChanged();
				if (!SeeThrough(next)) {
					break;
				}
				next = next->nextInBlocks();
			}
		}
	};
	const auto main = item->mainView();
	if (main) {
		around(main, nullptr);
	}
	for (const auto &list : _lists) {
		if (list) {
			if (const auto view = list->luminaAdFoldRow(item, 0)) {
				around(view, list.data());
			}
		}
	}
	return (main != nullptr);
}

void FoldUi::scheduleFlush(not_null<Main::Session*> session) {
	// Views made again or marked for layout outside an API update are laid
	// out once the history widget hears about it (see also
	// lumina_message_widgets.cpp); once per batch, after the current event.
	if (!_flushes.emplace(session).second) {
		return;
	}
	crl::on_main(session, [=] {
		_flushes.remove(session);
		session->data().sendHistoryChangeNotifications();
	});
}

void FoldUi::checkNotAd(not_null<History*> history) {
	const auto i = _chats.find(history);
	if (i == _chats.end() || i->second.notAd.empty()) {
		return;
	}
	const auto owner = &history->owner();
	const auto peerId = history->peer->id;
	const auto now = crl::now();
	auto done = std::vector<MsgId>();
	auto refold = std::vector<MsgId>();
	for (const auto &[head, pending] : i->second.notAd) {
		const auto item = owner->message(peerId, head);
		const auto decision = item ? CachedDecision(item) : nullptr;
		if (!decision || !decision->fold || now >= pending.deadline) {
			done.push_back(head);
			refold.insert(end(refold), begin(pending.ids), end(pending.ids));
		}
	}
	for (const auto head : done) {
		i->second.notAd.remove(head);
	}
	if (refold.empty()) {
		return;
	}
	// Not from inside the DecisionsChanges() event that brought us here.
	const auto session = base::make_weak(&history->session());
	crl::on_main([=] {
		if (session.get()) {
			SetExpanded(history, refold, false);
		}
	});
}

FoldUi::Verdict FoldUi::allNewFolded(
		not_null<History*> history,
		const Toast &toast) const {
	auto total = 0;
	auto waiting = false;
	for (const auto &block : history->blocks) {
		for (const auto &view : block->messages) {
			const auto item = view->data();
			if (item->out()
				|| !item->isRegular()
				|| item->id <= toast.readTill) {
				continue;
			}
			++total;
			if (item->isService()
				|| MessageTextFiltered(item->originalText().text)) {
				return Verdict::No;
			}
			const auto decision = CachedDecision(item);
			if (!decision) {
				waiting = true;
			} else if (!decision->fold || !ShouldFold(item)) {
				return Verdict::No;
			}
		}
	}
	if (waiting || !total) {
		return Verdict::Wait;
	}
	// Some new messages are not loaded: "all of them" is not known.
	return (total < toast.unread) ? Verdict::No : Verdict::Yes;
}

void FoldUi::checkToast(not_null<History*> history) {
	const auto i = _toasts.find(history);
	if (i == _toasts.end()) {
		return;
	}
	const auto window = i->second.window.get();
	if (!window || crl::now() > i->second.deadline) {
		_toasts.erase(i);
		return;
	}
	const auto verdict = allNewFolded(history, i->second);
	if (verdict == Verdict::Wait) {
		return;
	}
	const auto session = i->second.session;
	_toasts.erase(i);
	if (verdict == Verdict::Yes
		&& window->activeChatCurrent().history() == history) {
		_toasted.emplace(history, session);
		window->showToast(Tr(u"LuminaAdShieldAllFolded"_q));
	}
}

} // namespace

void StartFoldUi() {
	[[maybe_unused]] const auto &ui = FoldUi::Instance();
}

std::unique_ptr<Element> CreateView(
		not_null<HistoryItem*> item,
		not_null<ElementDelegate*> delegate,
		Element *replacing) {
	NoteViewCreated(item, delegate);

	// The history blocks keep the unread bar's place on a view made again
	// but not the bar itself (a ListWidget makes it again on its own): a
	// view this file makes again, or a fold row going back to the stock
	// view, keeps it.
	const auto historyList = [&] {
		return (delegate == item->history()->delegateMixin()->delegate());
	};
	const auto barToKeep = [&] {
		return replacing
			&& replacing->Has<HistoryView::UnreadBar>()
			&& historyList();
	};
	const auto withBar = [&](std::unique_ptr<Element> result) {
		result->createUnreadBar(tr::lng_unread_bar_some());
		return result;
	};
	const auto stock = [&] {
		return std::make_unique<HistoryView::Message>(
			delegate,
			item,
			replacing);
	};

	if (!Observing() || !IsMainList(item, delegate)) {
		return (barToKeep() && AsFold(replacing))
			? withBar(stock())
			: nullptr;
	}
	auto &ui = FoldUi::Instance();
	if (const auto list = dynamic_cast<HistoryView::ListWidget*>(
			delegate.get())) {
		ui.rememberList(list);
	}
	const auto kind = ui.desiredKind(item);
	const auto keepBar = barToKeep()
		&& ((kind != Kind::Stock) || ui.special(item));
	ui.remember(item, kind);
	if (kind == Kind::Stock) {
		return keepBar ? withBar(stock()) : nullptr;
	}
	auto result = std::unique_ptr<Element>();
	if (kind == Kind::Fold) {
		result = std::make_unique<FoldRow>(delegate, item, replacing);
	} else {
		result = stock();
		// Only on the row that is shown (the first part of an album), and
		// never over a line the message already has.
		if (!result->isHiddenByGroup()
			&& !result->Has<HistoryView::ServicePreMessage>()) {
			result->setServicePreMessage((kind == Kind::Trial)
				? TrialMark()
				: ExpandedHeader(item));
		}
	}
	return keepBar ? withBar(std::move(result)) : std::move(result);
}

bool IsFoldRow(not_null<const Element*> view) {
	return AsFold(view.get()) != nullptr;
}

void NoteChatShown(
		not_null<History*> history,
		Window::SessionController *window) {
	if (Observing()) {
		FoldUi::Instance().chatShown(history, window);
	}
}

bool RevealForJump(Element *view) {
	if (!view || !Observing()) {
		return false;
	}
	// A part of an album: the album is folded behind its first part.
	if (const auto group = view->history()->owner().groups().find(
			view->data())) {
		if (const auto first = ViewInSameList(view, group->items.front())) {
			view = first;
		}
	}
	const auto row = AsFold(view);
	if (!row || row->hiddenAnyway()) {
		return false;
	}
	ExpandSegmentOf(row);
	return true;
}

bool RevealForJump(not_null<HistoryItem*> item) {
	if (!Observing() || !ShouldFold(item)) {
		return false;
	} else if (RevealForJump(item->mainView())) {
		return true;
	}
	// Not in the chat history's list: the message (its album) alone.
	const auto group = item->history()->owner().groups().find(item);
	const auto first = group ? group->items.front().get() : item.get();
	FoldUi::Instance().expand(item->history(), first->id, WithAlbum(item));
	return true;
}

void RevealForReply(not_null<HistoryItem*> item) {
	if (!Observing() || !ShouldFold(item)) {
		return;
	}
	const auto session = &item->history()->session();
	const auto id = item->fullId();
	crl::on_main(session, [=] {
		if (const auto item = session->data().message(id)) {
			RevealForJump(item);
		}
	});
}

bool ShowFoldMenu(
		base::unique_qptr<Ui::PopupMenu> &menu,
		not_null<QWidget*> parent,
		Element *view,
		QPoint globalPosition) {
	const auto row = AsFold(view);
	if (!row || row->isHidden()) {
		return false;
	}
	const auto weak = base::make_weak(row);
	const auto peer = row->history()->peer;
	const auto session = base::make_weak(&peer->session());
	menu = base::make_unique_q<Ui::PopupMenu>(
		parent,
		st::popupMenuWithIcons);
	menu->addAction(Tr(u"LuminaAdShieldShow"_q), [=] {
		if (const auto strong = weak.get()) {
			ExpandSegmentOf(strong);
		}
	}, &st::menuIconExpand);
	menu->addAction(Tr(u"LuminaAdShieldNotAd"_q), [=] {
		if (const auto strong = weak.get()) {
			MarkNotAd(strong->segmentHead()->data());
		}
	}, &st::menuIconUnblock);
	menu->addAction(Tr(u"LuminaAdShieldTurnOffHere"_q), [=] {
		if (session.get()) {
			SetChatMode(peer, ChatMode::Off);
		}
	}, &st::menuIconCancel);
	menu->popup(globalPosition);
	return true;
}

QString FoldRowAccessibleName(not_null<const Element*> view) {
	const auto row = AsFold(view.get());
	if (!row || row->isHidden()) {
		return QString();
	}
	// The line itself says how to show the messages; the hint adds the
	// menu. A Chinese line ends with an ideograph and takes a full stop.
	const auto label = row->computeLabel();
	const auto last = label.isEmpty() ? QChar() : label.back();
	const auto stop = (last.unicode() >= 0x3000 && last.unicode() <= 0x9FFF)
		? QString(QChar(0x3002))
		: u". "_q;
	return label + stop + Tr(u"LuminaAdShieldFoldAccessibleHint"_q);
}

bool ActivateFoldRow(Element *view) {
	const auto row = AsFold(view);
	if (!row || row->isHidden()) {
		return false;
	}
	ExpandSegmentOf(row);
	return true;
}

} // namespace Lumina::AdShield
