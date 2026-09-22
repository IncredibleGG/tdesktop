/*
This file is part of LuminaGram,
a fork of Telegram Desktop.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "lumina/lumina_profile_chat_date.h"

#include "apiwrap.h"
#include "base/unixtime.h"
#include "data/data_changes.h"
#include "data/data_channel.h"
#include "data/data_chat.h"
#include "data/data_peer.h"
#include "data/data_session.h"
#include "history/history_item.h"
#include "lang/lang_keys.h"
#include "lumina/lumina_locale.h"
#include "lumina/lumina_settings.h"
#include "main/main_session.h"

#include <memory>

namespace Lumina {
namespace {

const auto kKey = u"showChatDate"_q;

// A group's / channel's first cloud message is message id 1, and its timestamp
// is the peer's REAL creation date. This is deliberately NOT chat->date /
// channel->date: for a channel that field is the *current user's join date*
// (it only equals the creation date when the viewer is the creator), so using
// it mislabels a join as a creation. Matching the reference client (Swiftgram
// fetches MessageId(peer, Cloud, 1) and uses firstMessage.timestamp).
constexpr auto kFirstMessageId = MsgId(1);

// Emits once whenever message #1 for `peer` becomes available: immediately when
// it is already cached, otherwise after a network fetch completes. Also starts
// that fetch. requestMessageData() routes basic groups through
// messages.getMessages and channels through channels.getMessages on its own.
//
// The fetch callback is guarded by a lifetime-owned flag, so a row destroyed
// before the reply arrives never calls into a dead consumer.
[[nodiscard]] rpl::producer<> FirstMessageResolved(not_null<PeerData*> peer) {
	return [=](auto consumer) {
		auto lifetime = rpl::lifetime();
		if (!peer->isChannel()) {
			// Only channels/supergroups source their creation date from message #1;
			// basic groups use chat->date, so no fetch is needed.
			return lifetime;
		}
		const auto session = &peer->session();
		if (session->data().message(peer, kFirstMessageId)) {
			consumer.put_next({});
		} else {
			const auto alive = std::make_shared<bool>(true);
			session->api().requestMessageData(peer, kFirstMessageId, [=] {
				if (*alive) {
					consumer.put_next({});
				}
			});
			lifetime.add([alive] { *alive = false; });
		}
		return lifetime;
	};
}

} // namespace

bool ChatCreationDateRowEnabled() {
	return Settings::Instance().getBool(kKey, true);
}

void SetChatCreationDateRowEnabled(bool value) {
	Settings::Instance().set(kKey, value);
}

rpl::producer<> ChatCreationDateRowChanges() {
	return Settings::Instance().changesFor(kKey);
}

TimeId ChatCreationDate(not_null<PeerData*> peer) {
	if (const auto chat = peer->asChat()) {
		// Basic (legacy) group: message ids are account-global, not per-chat, so
		// message #1 does not apply. For basic groups chat->date IS the real
		// creation date, so use it directly.
		return chat->date;
	} else if (peer->isChannel()) {
		// Channel / supergroup: the timestamp of message #1 is the true creation
		// date. When that message is not (yet) cached this returns 0, which hides
		// the row until the fetch kicked off by FirstMessageResolved() lands - and
		// stays 0 (row hidden) if the message is genuinely unavailable or deleted.
		// It NEVER falls back to channel->date (the viewer's join date).
		const auto item = peer->session().data().message(peer, kFirstMessageId);
		return item ? item->date() : 0;
	}
	return 0;
}

QString ChatCreationDateText(not_null<PeerData*> peer) {
	if (!ChatCreationDateRowEnabled()) {
		return QString();
	}
	const auto date = ChatCreationDate(peer);
	if (date <= 0) {
		return QString();
	}
	// langDayOfMonthFull() drops the year for anything dated inside the
	// current one, which is right for a message header and wrong for a
	// creation date - "12 March" alone says nothing about how old a channel
	// is. This is that function's with-year branch, used unconditionally.
	const auto parsed = base::unixtime::parse(date).date();
	return tr::lng_month_day_year(
		tr::now,
		lt_month,
		Lang::MonthDay(parsed.month())(tr::now),
		lt_day,
		QString::number(parsed.day()),
		lt_year,
		QString::number(parsed.year()));
}

void AddChatCreationDateRow(
		const ProfileRowsContext &context,
		not_null<PeerData*> peer,
		Data::ForumTopic *topic) {
	if (topic) {
		return;
	}
	// The value is driven by message #1 arriving (FirstMessageResolved), by the
	// preference toggle, and by language changes. FullInfo/Name keep the older
	// emissions that made the row appear; ChatCreationDateText() re-reads the
	// cached message #1 on every emission.
	auto text = rpl::merge(
		ChatCreationDateRowChanges(),
		LangChanges(),
		FirstMessageResolved(peer),
		peer->session().changes().peerFlagsValue(
			peer,
			Data::PeerUpdate::Flag::Name
			| Data::PeerUpdate::Flag::FullInfo) | rpl::to_empty
	) | rpl::map([=] {
		return tr::marked(ChatCreationDateText(peer));
	});
	context.addInfoOneLine(
		TrValue(u"LuminaProfileChatCreated"_q),
		std::move(text),
		QString());
}

} // namespace Lumina
