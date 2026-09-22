/*
This file is part of LuminaGram,
a fork of Telegram Desktop.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include <rpl/producer.h>

namespace Lumina {

// Three independent, per-chat-INDEPENDENT (global) display gates for the
// message list. Each defaults to false, so while it is off the chat renders
// exactly the way stock Telegram Desktop does; nothing below changes what is on
// screen until the matching toggle is turned on.
//
// They are display gates only: none of them sends anything, edits a message or
// touches account state. Each is read live at layout / paint time, so a change
// takes effect as messages are laid out again (scrolling, a resize, a new
// message) - the same way the "Show seconds in message time" and "Show exact
// numbers" gates on the same page already behave.
//
// Keys live in Store::Prefs (tdata/luminagram.json). Main thread only.

// `hideReactions`. Hide the reaction chips drawn under messages. Gated in
// Element::refreshReactions(): while this is on the reaction inline list is
// never built, so both the draw and the geometry it would have reserved are
// skipped and no empty gap is left behind.
[[nodiscard]] bool HideReactions();
void SetHideReactions(bool value);
[[nodiscard]] rpl::producer<bool> HideReactionsValue();

// `wideChannelPosts`. Render channel (broadcast) post bubbles at (near) full
// available width, like a reading layout. Gated in
// Message::resizeContentGetHeight(): for a text-only post it lifts the bubble
// width cap the same way the built-in "Unlimited message width" debug option
// does, but only for channel posts and only while this is on.
[[nodiscard]] bool WideChannelPosts();
void SetWideChannelPosts(bool value);
[[nodiscard]] rpl::producer<bool> WideChannelPostsValue();

// `stickerTimestamp`. Show the message timestamp on sticker, animated-sticker
// and round-video messages, which normally show it only while the pointer is
// over them. Gated in UnwrappedMedia::needInfoDisplay(): the timestamp is drawn
// as the usual translucent overlay in the corner of the media, so nothing about
// the message's size changes.
[[nodiscard]] bool StickerTimestamp();
void SetStickerTimestamp(bool value);
[[nodiscard]] rpl::producer<bool> StickerTimestampValue();

} // namespace Lumina
