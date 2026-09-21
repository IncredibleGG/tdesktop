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

// The "Chat ID" row on a group or channel profile: the peer's numeric Telegram
// id - the bare chat id for a basic group (`peerToChat(peer->id).bare`) or the
// bare channel id for a supergroup / channel (`peerToChannel(peer->id).bare`),
// the same value Android prints on the profile screen.
//
// UNCONDITIONAL, exactly like the user-id row (lumina_profile_user_id): it
// shows on every group and channel and there is no preference to switch it off.
// A real peer's id is never zero, so the row never resolves to an empty value
// and is therefore always shown. It is copyable the same way its neighbours
// are: the value label goes through the same addInfoOneLine factory, which
// makes it selectable and gives it a "Copy" context-menu entry.
//
// The bare id is used - not the -100 / bot-API form - to match the user-id row
// and Android's own display, so the two id rows read consistently.
//
// Purely local: the id is already part of the peer, so nothing is requested
// from the server to build this row.

// The bare numeric id as text. Empty for a peer that is neither a group nor a
// channel (a user peer never reaches this row).
[[nodiscard]] QString ChatIdText(not_null<PeerData*> peer);

// Adds the row through the F-05 seam. Call from Lumina::AddChatInfoRows(),
// passing its `topic` through: a forum topic's section describes the topic,
// not the peer, so it gets no peer-level row - the same rule the upstream
// location row follows at info_profile_actions.cpp:1832.
void AddChatIdRow(
	const ProfileRowsContext &context,
	not_null<PeerData*> peer,
	Data::ForumTopic *topic = nullptr);

} // namespace Lumina
