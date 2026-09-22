/*
This file is part of LuminaGram,
a fork of Telegram Desktop.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "lumina/lumina_profile_rows.h"

#include <rpl/producer.h>

#include <QtCore/QString>

class PeerData;

namespace Data {
class ForumTopic;
} // namespace Data

namespace Lumina {

// The "Created" row on a group or channel profile.
//
// The value is the timestamp of the peer's FIRST cloud message (message id 1),
// which is its real creation date. It is deliberately NOT the `date` field of
// the chat / channel constructor: for a channel that field is the CURRENT
// USER'S JOIN date (ChannelData::date; it only equals the creation date when
// the viewer is the creator), so showing it under a "Created" label mislabels
// a join as a creation - the bug this row was rewritten to fix. This matches
// the reference client, which reads firstMessage.timestamp of MessageId 1.
//
// Message #1 is fetched on demand (ApiWrap::requestMessageData, which picks
// messages.getMessages for basic groups and channels.getMessages for channels
// on its own) and the row updates reactively when it arrives. Until then, and
// whenever message #1 is unavailable or deleted, the value is 0, which hides
// the row - it NEVER falls back to the join date.
//
// THE LABEL IS ALWAYS "Created", now truthfully: the value really is the
// creation date, so - unlike Android, which relabels the raw join date field
// "Joined" - there is no meaning to hide behind.
//
// Best-effort caveat for basic (legacy) groups: their first message is not
// addressable as id 1 in the shared (non-channel) message-id space, so the
// fetch usually finds nothing and the row hides. It still never shows a wrong
// date. Migrated supergroups show the message-#1 timestamp of the supergroup
// (not the pre-migration basic group's original creation date).

// Preference `showChatDate`, Store::Prefs, default false. Same key and same
// default as Android (LuminaChatActivity.java:76).
[[nodiscard]] bool ChatCreationDateRowEnabled();
void SetChatCreationDateRowEnabled(bool value);

// Fires when the preference above changes, including through a whole-file
// restore by Settings::importAll().
[[nodiscard]] rpl::producer<> ChatCreationDateRowChanges();

// The creation timestamp of a group or channel - the date of its first cloud
// message (id 1) - 0 when message #1 is not (yet) available or when the peer
// is neither a group nor a channel.
[[nodiscard]] TimeId ChatCreationDate(not_null<PeerData*> peer);

// What the row shows. Empty - which makes the row slide itself away - when the
// preference is off or there is no date.
[[nodiscard]] QString ChatCreationDateText(not_null<PeerData*> peer);

// Adds the row through the F-05 seam. Call from Lumina::AddChatInfoRows(),
// passing its `topic` through: a forum topic's section describes the topic,
// not the peer, so it gets no peer-level row - the same rule the upstream
// location row follows at info_profile_actions.cpp:1832.
void AddChatCreationDateRow(
	const ProfileRowsContext &context,
	not_null<PeerData*> peer,
	Data::ForumTopic *topic);

} // namespace Lumina
