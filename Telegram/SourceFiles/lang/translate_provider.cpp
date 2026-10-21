/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "lang/translate_provider.h"

#include "core/application.h"
#include "core/core_settings.h"
#include "data/data_msg_id.h"
#include "data/data_peer.h"
#include "data/data_session.h"
#include "history/history_item.h"
#include "lang/translate_mtproto_provider.h"
#include "lumina/lumina_translate_providers.h"
#include "platform/platform_translate_provider.h"

// LuminaGram: upstream's experimental "translate-url-template" option is not
// registered in this fork, so lang/translate_url_provider.cpp is never
// reached. That option accepted plain http, put the whole message text into a
// GET query string (where proxies and server logs keep it), set no timeout,
// and ran ahead of every LuminaGram provider - skipping the explicit-choice,
// protected-chat and https gates those providers enforce. A value left behind
// in tdata/experimental_options.json, or brought in through the Experimental
// settings import, is now an unknown option: base::options logs it and
// ignores it. A custom HTTP endpoint is the OpenAI-compatible provider in
// LuminaGram's own Translation settings.

namespace Ui {

std::unique_ptr<TranslateProvider> CreateTranslateProvider(
		not_null<Main::Session*> session) {
	if (Core::App().settings().usePlatformTranslation()
		&& Platform::IsTranslateProviderAvailable()) {
		return Platform::CreateTranslateProvider();
	}
	if (auto lumina = Lumina::CreateTranslateProvider(session)) {
		return lumina;
	}
	return CreateMTProtoTranslateProvider(session);
}

TranslateProviderRequest PrepareTranslateProviderRequest(
		not_null<TranslateProvider*> provider,
		not_null<PeerData*> peer,
		MsgId msgId,
		TextWithEntities text) {
	auto result = TranslateProviderRequest{
		.peerId = uint64(peer->id.value),
		.msgId = IsServerMsgId(msgId) ? msgId.bare : 0,
		.text = std::move(text),
	};
	if (provider->supportsMessageId()) {
		return result;
	}
	if (result.msgId) {
		if (const auto i = peer->owner().message(peer, MsgId(result.msgId))) {
			result.text = i->originalText();
		}
		result.msgId = 0;
	}
	return result;
}

} // namespace Ui
