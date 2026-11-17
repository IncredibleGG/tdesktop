/*
This file is part of LuminaGram,
a fork of Telegram Desktop.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "info/profile/info_profile_text.h"
#include "ui/text/text_variant.h"

class PeerData;
class UserData;

namespace Data {
class ForumTopic;
} // namespace Data

namespace Ui {
class VerticalLayout;
} // namespace Ui

namespace Window {
class SessionController;
} // namespace Window

namespace Lumina {

// The single seam LuminaGram has into the profile "details" block built by
// Info::Profile::DetailsFiller::makeInfo() (info/profile/info_profile_actions
// .cpp). That file is 3000+ lines of upstream-hot code, so all it holds is two
// calls into this header (Lumina::AddUserInfoRows / AddChatInfoRows) and the
// two call-scoped lambdas they hand over beside addInfoOneLine
// (addLuminaClickableRow and trackLuminaRow, see ProfileRowsContext below);
// every LuminaGram profile row is added from here instead. After an upstream
// merge, grep that file for `Lumina::`, `addLuminaClickableRow` and
// `trackLuminaRow` - if any of them is gone, the merge ate the seam.
//
// Mirrors the `addInfoOneLine` factory local to makeInfo(). Rows created
// through it are added to the same Ui::VerticalLayout and tracked by the same
// Ui::MultiSlideTracker as their upstream neighbours, so a row whose text
// producer emits an empty value slides itself away, and the details block as a
// whole still collapses when every row inside it is empty.
//
// Arguments are (label, text, contextCopyText); the padding and popup menu
// style arguments of the upstream factory keep their defaults.
//
// !! CALL-SCOPED ONLY. The wrapped lambda holds references into makeInfo()'s
// stack frame - in particular to its local Ui::MultiSlideTracker, which is
// destroyed as soon as makeInfo() returns. Call it synchronously from
// AddUserInfoRows() / AddChatInfoRows() and never copy it into a click
// handler, an rpl subscription, a lifetime-owned state or any other callback
// that runs later.
using InfoOneLineFactory = Fn<Info::Profile::TextWithLabel(
	v::text::data label,
	rpl::producer<TextWithEntities> text,
	QString contextCopyText)>;

// A full-width, clickable settings-button row (the same widget the upstream
// action buttons in info_profile_actions.cpp use), tracked by makeInfo()'s
// MultiSlideTracker just like every other row in the details block: when
// `shown` emits false the row slides away, and when every row in the block is
// hidden the block still collapses. Use it for a row that has to react to a
// tap; use addInfoOneLine for a plain labelled value.
//
// !! CALL-SCOPED, exactly like addInfoOneLine above: the wrapped lambda holds
// references into makeInfo()'s stack frame (its layout and its
// Ui::MultiSlideTracker). Invoke it synchronously from AddChatInfoRows() /
// AddUserInfoRows() and never copy it into a later callback.
//
// `onClick`, by contrast, is owned by the button and runs on tap long after
// makeInfo() has returned, so it must capture only things that outlive the
// section - context.controller and the peer pointers are safe (see the note on
// ProfileRowsContext), makeInfo()'s locals are not.
using ClickableRowFactory = Fn<void(
	rpl::producer<QString> text,
	rpl::producer<bool> shown,
	Fn<void()> onClick,
	const style::icon *icon)>;

// Also call-scoped: makeInfo() passes a temporary, so the reference dies at
// the end of the call. Copy out the individual members you need to keep - both
// not_null pointers outlive the section, `addInfoOneLine` does not.
struct ProfileRowsContext {
	// The details block's own Ui::VerticalLayout.
	//
	// Prefer `addInfoOneLine` for anything row-shaped. A widget added straight
	// to `container` is NOT registered with makeInfo()'s MultiSlideTracker,
	// and the whole block is toggled on `tracker.atLeastOneShownValue()` - so
	// on a peer where every upstream row happens to be empty an untracked
	// Lumina row is built but never shown. If a future row really cannot go
	// through the factories, wrap it in a Ui::SlideWrap and register it through
	// `trackRow` (the way AddIconInfoRow does) rather than adding it bare.
	not_null<Ui::VerticalLayout*> container;
	not_null<Window::SessionController*> controller;
	InfoOneLineFactory addInfoOneLine;

	// Optional: only set by the chat/channel call site today. A row added
	// through it is tracked exactly like an addInfoOneLine row. May be empty in
	// contexts that do not provide it, so check it before calling.
	ClickableRowFactory addClickableRow;

	// Optional: registers a row LuminaGram built itself and already added to
	// `container` with makeInfo()'s Ui::MultiSlideTracker, so it slides and
	// collapses exactly like an addInfoOneLine row. AddIconInfoRow() below is
	// the one user; go through it rather than calling this directly.
	//
	// !! CALL-SCOPED, exactly like addInfoOneLine: it holds a reference to
	// makeInfo()'s local tracker. Never copy it into a later callback.
	Fn<void(not_null<Ui::SlideWrap<Ui::VerticalLayout>*>)> trackRow;
};

// A labelled one-line value row, like `context.addInfoOneLine`, with a
// leading icon: the icon sits in the column the upstream shared-media and
// action buttons draw theirs in, and the text moves over to their 79px text
// column, so a LuminaGram value row lines up with every other icon row of
// the profile instead of with the icon-less upstream text rows (phone, bio,
// username). `icon` must be a static style icon sized for a 32px info slot
// (see luminaInfoIcon* in lumina.style).
//
// The row is selectable, double-click selects the value and the context menu
// copies `contextCopyText` - the same as an addInfoOneLine row - and it is
// tracked the same way: an empty value slides the row, icon included, away.
// Without `context.trackRow` it falls back to `context.addInfoOneLine`, i.e.
// the same row without the icon. Call-scoped like the factories it uses.
Info::Profile::TextWithLabel AddIconInfoRow(
	const ProfileRowsContext &context,
	v::text::data label,
	rpl::producer<TextWithEntities> text,
	const style::icon &icon,
	QString contextCopyText = QString());

// Both functions are called once per built details section, from the matching
// branch of makeInfo().
//
// Rows added here must be reactive exactly the way their neighbours are: build
// the text producer from `peer->session().changes().peerFlagsValue(...)` (or
// another rpl source) rather than from a value computed once, or the row will
// never update after the peer is refreshed from the server, and an initially
// empty row will stay hidden forever.

// The user branch. Deliberately called for bots as well - private contact
// notes have to be available on bot profiles too - so a row that does not
// apply to bots must check `user->isBot()` itself. Likewise it is called for
// `user->isSelf()` and for Saved Messages sublist sections (where makeInfo()
// builds the details of `sublist->sublistPeer()`); a row that should not show
// up there has to test for it.
void AddUserInfoRows(
	const ProfileRowsContext &context,
	not_null<UserData*> user);

// The chat / channel branch. `topic` is not null when the section describes a
// forum topic rather than the peer itself; peer-level rows must skip that case,
// the same way the upstream location row does.
void AddChatInfoRows(
	const ProfileRowsContext &context,
	not_null<PeerData*> peer,
	Data::ForumTopic *topic);

} // namespace Lumina
