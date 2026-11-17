/*
This file is part of LuminaGram,
a fork of Telegram Desktop.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "base/basic_types.h"
#include "data/data_msg_id.h"
#include "data/data_peer_id.h"
#include "lumina/lumina_ad_shield_prefs.h"

#include <rpl/producer.h>

class History;
class HistoryItem;
class PeerData;

namespace Main {
class Session;
} // namespace Main

namespace HistoryView {
class ElementDelegate;
} // namespace HistoryView

namespace Lumina::AdShield {

// "Hide repeated ads" on desktop: the main-thread side.
//
// WHAT RUNS WHERE. Everything here is main thread. The engine (Account /
// Store / ChatContext, lumina_ad_shield_account.h) lives on one dedicated
// crl::object_on_queue (lumina_ad_shield_worker.h); this side only copies a
// message's fields into a MessageInput, posts batches, and keeps the
// (peer, message) -> Decision cache the chat list reads.
//
// OFF BY DEFAULT, AND OFF MEANS NOTHING. Until some account has the feature
// effectively on - the global switch, or "on in this group" - there is no
// queue, no key file, no directory, and NoteViewCreated() returns after one
// bool check. When an account becomes fully inactive again (global switch
// off and no group of it set to "on") its data is forgotten (forgetAll), and
// the queue goes away once no account needs it.
//
// WHAT IS OBSERVED. Messages of eligible groups (basic groups, supergroups,
// forums, gigagroups; never private chats, secret chats, Saved Messages or
// broadcast channels) whose view is created for the main message list - the
// chat history and forum topics - and that the keyword filter does not
// already hide. Service, sponsored, scheduled and local messages never are.
// Edits (also of link addresses and buttons) and pins are observed again,
// real deletions are anonymised (SPEC 6.11), freeing a message object
// (leaving a chat, reloading) only drops it from the cache. A decide pass
// after new messages covers only them; the whole chat is decided again at
// most every 2 s (and when the chat is opened), never on every message of a
// flood. Logs carry counts and timings only.
//
// STORAGE. One directory, tdata/lumina_adshield/, with one sub-directory per
// account (hex(H("acct", userId))) and one file per group (SPEC 8.1). The
// per-install SipHash key is tdata/luminagram_adshield.key: sealed with DPAPI
// on Windows (lumina_secret_store.h), PLAINTEXT on macOS and Linux - the
// account's local key is not reachable from here (Storage::Domain keeps it
// private). Without the key the files are unlinkable hashes; with it they
// still hold no text, no links, no usernames and no plain sender ids. Neither
// the directory nor the key is part of a Lumina backup.
//
// LIFECYCLE. The queue purges orphans when it starts, writes a group 5 s
// after its last change, writes everything when the app is deactivated or
// quits (FlushNow()), keeps at most 8 groups in memory and 10 MiB on disk.
// Logout -> SessionLoggingOut() forgets that account and its per-group
// modes; the panic wipe -> PanicWipe() removes the whole directory and the
// key.

// Called once from Core::Application::run(). Only subscribes to the
// preferences (and the fold UI to DecisionsChanges()); creates nothing.
void Start();

// Every view made for a non-service message (CreateView() in
// lumina_ad_fold.h, from HistoryItem::createView()). O(1) when the feature
// is off.
void NoteViewCreated(
	not_null<HistoryItem*> item,
	not_null<HistoryView::ElementDelegate*> delegate);

// Main::Session::finishLogout(), before the session data is cleared.
void SessionLoggingOut(not_null<Main::Session*> session);

// Core::Application::readyToQuit(): writes what is pending. Blocks until the
// queue has written it (a few small files at most).
void FlushNow();

// The panic wipe: synchronous. Every account's data, the directory and the
// key file are gone when this returns.
void PanicWipe();

// "Clear local data" in the settings: every logged-in account forgets its
// data, the per-group modes and the similar-image tables are cleared.
void ClearLocalData();

// Whether any account has the feature effectively on right now.
[[nodiscard]] bool Observing();

// Group, supergroup, forum or gigagroup. Never a private chat, a secret
// chat, Saved Messages, a broadcast channel or a channel's direct messages.
[[nodiscard]] bool ChatEligible(not_null<PeerData*> peer);

// The group's own mode (the group menu), and whether it is effectively on.
[[nodiscard]] ChatMode ChatModeFor(not_null<PeerData*> peer);
void SetChatMode(not_null<PeerData*> peer, ChatMode mode);
[[nodiscard]] bool ChatEnabled(not_null<PeerData*> peer);

// ---------------------------------------------------------------------------
// The API the chat list (the fold UI) calls. All O(1) and cheap enough for
// Element::isHidden / createView.

// The cached decision for a message, or nullptr when there is none (not an
// eligible group, the feature is off there, not decided yet, or decided and
// since freed). The pointer is valid until the next DecisionsChanges() event
// or the next return to the event loop, whichever comes first.
[[nodiscard]] const Decision *CachedDecision(
	not_null<const HistoryItem*> item);

// Fold this message now: decision.fold, the trial switch is off, its segment
// is not expanded and it was not already on screen when its cluster
// triggered (see ChatShown()).
[[nodiscard]] bool ShouldFold(not_null<const HistoryItem*> item);

// Trial mode: the message would fold; show the "suspected repeated ad" mark
// instead of folding.
[[nodiscard]] bool ShouldMarkTrial(not_null<const HistoryItem*> item);

// Runtime-only expansion of a fold segment (never stored). The UI computes
// the segment (consecutive folded messages) and passes all of its ids.
[[nodiscard]] bool Expanded(not_null<const HistoryItem*> item);
void SetExpanded(
	not_null<History*> history,
	const std::vector<MsgId> &ids,
	bool expanded);

// The chat was opened (shown again): messages whose cluster triggered while
// they were already on screen may fold now. Call it when a chat or a topic
// is shown, before its messages are laid out.
void ChatShown(not_null<History*> history);

// "Not an ad" for the cluster of this message (90 days, SPEC 6.4). The
// cluster unfolds once the decisions come back.
void MarkNotAd(not_null<HistoryItem*> item);

// Messages of the last 7 days the group would fold now; computed on the
// queue, `done` runs on the main thread (0 when the feature is off there).
void RequestFoldedCount(not_null<PeerData*> peer, Fn<void(int)> done);

struct DecisionsChange {
	not_null<History*> history;
	// Messages whose fold state may have changed. Empty means "any message
	// of this chat" (the feature was turned off there, data was cleared...).
	std::vector<MsgId> ids;
};

// Fires on the main thread when cached decisions, the expansion or the
// deferral of messages change, and for every message that folds (or would)
// when the trial switch changes. The fold UI (lumina_ad_fold.h) makes those
// messages' views again.
[[nodiscard]] rpl::producer<DecisionsChange> DecisionsChanges();

// Fires when the trial switch changes, after the DecisionsChanges() above.
[[nodiscard]] rpl::producer<> DisplayChanges();

} // namespace Lumina::AdShield
