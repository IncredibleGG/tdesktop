/*
This file is part of LuminaGram,
a fork of Telegram Desktop.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "base/basic_types.h"

#include <QtCore/QPoint>
#include <QtCore/QString>

#include <memory>

class History;
class HistoryItem;
class QWidget;

namespace base {
template <typename T>
class unique_qptr;
} // namespace base

namespace Ui {
class PopupMenu;
} // namespace Ui

namespace Window {
class SessionController;
} // namespace Window

namespace HistoryView {
class Element;
class ElementDelegate;
} // namespace HistoryView

namespace Lumina::AdShield {

// "Hide repeated ads": the fold rows in the chat (the decisions behind them
// come from lumina_ad_shield_manager.h).
//
// WHERE. Only in the main message list: the chat history (HistoryInner) and
// the ListWidget of a forum topic, a comments thread or a history section.
// Search, shared media, pinned messages, reply quotes, the forward picker,
// chat-list previews, the admin log, scheduled messages and every preview
// keep the stock views.
//
// HOW. A folded message gets its own view, a fold row, instead of the usual
// HistoryView::Message. Consecutive fold rows form a segment: the first one
// (the head) shows one service-style line - "N repeated messages folded
// (suspected ads) - click to show" - and the others (members) report
// isHidden(), so they lay out at zero height exactly like grouped-away album
// parts or keyword-filtered messages. Nothing is ever removed from the
// message list. An album folds as one unit (its other parts are hidden by
// the group anyway). Clicking the row expands the whole segment for this run
// only; the first expanded message then carries a slim header "Repeated
// messages (suspected ads) - Fold - Not an ad". A right click (or the
// context menu key) on a fold row offers Show / Not an ad / Turn off in this
// group. In the trial run nothing is hidden: a small "Suspected repeated ad"
// line is shown above the messages that would fold.
//
// READ STATE. Members sit at zero height right below their head, inside the
// visible area whenever the bottom of the fold row is, so the stock "read
// what is painted" logic of HistoryInner / ListWidget reads them with it;
// a visible fold row never leaves an unread badge behind.

// Start(), lumina_ad_shield_manager.h: listens for changed decisions, so
// messages already on screen fold when the feature is turned on. Creates
// nothing else.
void StartFoldUi();

// HistoryItem::createView(), for every non-service message. Observes the
// message (lumina_ad_shield_manager.h) and returns its fold row, or a stock
// message view decorated with the expanded header or the trial mark, or
// nullptr for the stock view. One bool check while the feature is off.
[[nodiscard]] std::unique_ptr<HistoryView::Element> CreateView(
	not_null<HistoryItem*> item,
	not_null<HistoryView::ElementDelegate*> delegate,
	HistoryView::Element *replacing);

// Whether this view is a fold row (head or member).
[[nodiscard]] bool IsFoldRow(not_null<const HistoryView::Element*> view);

// A chat (HistoryWidget, with its window for the one-time toast) or a topic
// / section (ChatWidget, no toast) is being shown, before its messages are
// laid out: messages that were held back while they were on screen fold
// now, and if every new message of the chat turns out to be folded, the
// window says so once per chat per run.
void NoteChatShown(
	not_null<History*> history,
	Window::SessionController *window);

// Jumping or replying to a folded message shows its segment. `view` is the
// message's view in the list that jumps; with only an item, its view in the
// chat history is used, or the message alone is shown. Returns whether
// something was expanded (the views are recreated synchronously; the list
// still has to lay them out).
bool RevealForJump(HistoryView::Element *view);
bool RevealForJump(not_null<HistoryItem*> item);

// Replying to a folded message (the reply may start inside the list's own
// mouse handling): RevealForJump(item) right after the current event.
void RevealForReply(not_null<HistoryItem*> item);

// The context menu of a fold row, instead of the message menu. Returns false
// (and does nothing) for any other view.
bool ShowFoldMenu(
	base::unique_qptr<Ui::PopupMenu> &menu,
	not_null<QWidget*> parent,
	HistoryView::Element *view,
	QPoint globalPosition);

// Accessibility: the spoken name of a fold row (its text and what can be
// done with it), empty for other views; and "invoke" on a fold row expands
// it (returns false for other views).
[[nodiscard]] QString FoldRowAccessibleName(
	not_null<const HistoryView::Element*> view);
bool ActivateFoldRow(HistoryView::Element *view);

} // namespace Lumina::AdShield
