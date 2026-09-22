/*
This file is part of LuminaGram,
a fork of Telegram Desktop.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include <rpl/producer.h>

#include <QtCore/QString>

#include <vector>

namespace Lumina {

// Batch 4, #22. A local, display-only keyword filter for the message list.
//
// The user keeps a list of keywords; any message whose text (caption included)
// contains one of them, case-insensitively, is HIDDEN from the visible message
// list on this device. Nothing is deleted, edited, reported or sent anywhere -
// this is purely "don't show me these", the user's own choice about their own
// screen. An empty list (the default) filters nothing, so the message list
// reads exactly as stock Telegram Desktop's does.
//
// The hide is implemented by making a matching Element report isHidden(), which
// is the same mechanism grouped-away media parts already use: the element
// collapses to zero height and is neither painted nor interactive. The filter
// therefore takes effect as messages are laid out again (scrolling, a resize,
// reopening the chat), the same way the other LuminaGram display gates do.
//
// Key `messageFilterKeywords` (a JSON array of strings), Store::Prefs.

// True when at least one keyword is set. Cheap.
[[nodiscard]] bool MessageFilterActive();

// True when `text` matches at least one keyword. Cheap when no keywords are
// set (the common case): it does no allocation and returns on the first line.
// Safe to call from layout / paint paths.
[[nodiscard]] bool MessageTextFiltered(const QString &text);

// The current keyword list, trimmed and de-duplicated, in order.
[[nodiscard]] std::vector<QString> MessageFilterKeywords();

// Normalises before writing: trims, drops empties, de-duplicates
// case-insensitively, clamps each keyword's length and the list's size.
void SetMessageFilterKeywords(std::vector<QString> keywords);

// Fires when the keyword list changes (a UI edit or a whole-file restore).
[[nodiscard]] rpl::producer<> MessageFilterChanges();

inline constexpr auto kMessageFilterMaxKeywords = 200;
inline constexpr auto kMessageFilterKeywordMaxLength = 128;

} // namespace Lumina
