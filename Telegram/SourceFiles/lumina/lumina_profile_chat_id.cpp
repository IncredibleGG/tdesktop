/*
This file is part of LuminaGram,
a fork of Telegram Desktop.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "lumina/lumina_profile_chat_id.h"

#include "data/data_changes.h"
#include "data/data_peer.h"
#include "lang/lang_keys.h"
#include "lumina/lumina_locale.h"
#include "main/main_session.h"

namespace Lumina {

QString ChatIdText(not_null<PeerData*> peer) {
	if (peer->isChat()) {
		return QString::number(peerToChat(peer->id).bare);
	} else if (peer->isChannel()) {
		return QString::number(peerToChannel(peer->id).bare);
	}
	return QString();
}

void AddChatIdRow(
		const ProfileRowsContext &context,
		not_null<PeerData*> peer,
		Data::ForumTopic *topic) {
	if (topic) {
		return;
	} else if (!peer->isChat() && !peer->isChannel()) {
		return;
	}
	// The id itself never changes, but the row still needs one initial emission
	// to appear, and FullInfo delivers that immediately on subscription the
	// same way it does for the user-id row; LangChanges keeps the value in step
	// if the label's language is ever switched.
	auto text = rpl::merge(
		LangChanges(),
		peer->session().changes().peerFlagsValue(
			peer,
			Data::PeerUpdate::Flag::FullInfo) | rpl::to_empty
	) | rpl::map([=] {
		return tr::marked(ChatIdText(peer));
	});
	context.addInfoOneLine(
		TrValue(u"LuminaProfileChatId"_q),
		std::move(text),
		QString());
}

} // namespace Lumina
