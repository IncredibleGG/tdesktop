/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "mtproto/sender.h"
#include "spellcheck/spellcheck_types.h"

class ApiWrap;

namespace Main {
class Session;
} // namespace Main

namespace Api {

struct SummaryEntry {
	TextWithEntities result;
	LanguageId languageId;
	bool shown = false;
	bool loading = false;
	bool premiumRequired = false;
	mtpRequestId requestId = 0;
};

class Transcribes final {
public:
	explicit Transcribes(not_null<ApiWrap*> api);

	struct Entry {
		QString result;
		// LuminaGram inline voice transcript: the raw transcript before any
		// translation (empty on a stock/paid entry), and the chat-translation
		// target `result` is currently aligned to (default = untranslated). These
		// let the transcript be re-translated when chat translation is toggled
		// later, mirroring the paid SummaryEntry above.
		QString source;
		LanguageId languageId;
		bool shown = false;
		bool failed = false;
		bool toolong = false;
		bool pending = false;
		bool roundview = false;
		mtpRequestId requestId = 0;
	};

	void toggle(not_null<HistoryItem*> item);
	[[nodiscard]] const Entry &entry(not_null<HistoryItem*> item) const;

	// LuminaGram: free on-device voice-to-text (Apple Speech / whisper.cpp),
	// never Telegram's paid transcribe API, writes its transcript into the
	// SAME inline Entry the premium path renders. history_view_document.cpp
	// (voice notes) and the Gif->Document switch in data_media_types.cpp
	// (round videos) then show it under the bubble with no further changes.
	// Each mirrors the resize / view-refresh that stock load()/toggle()/apply()
	// perform. Driven from lumina/lumina_voice_to_text.cpp.
	void luminaStartInline(not_null<HistoryItem*> item, bool roundview);
	void luminaShowInline(
		not_null<HistoryItem*> item,
		const QString &result,
		const QString &source,
		LanguageId languageId);
	// Re-publish a transcript's translation without disturbing its shown /
	// spinner state (a background re-translation after a translation toggle).
	void luminaRetranslateInline(
		not_null<HistoryItem*> item,
		const QString &result,
		const QString &source,
		LanguageId languageId);
	void luminaFailInline(not_null<HistoryItem*> item);
	void luminaToggleInline(not_null<HistoryItem*> item);

	void toggleSummary(not_null<HistoryItem*> item);
	[[nodiscard]] const SummaryEntry &summary(
		not_null<const HistoryItem*> item) const;
	void checkSummaryToTranslate(FullMsgId id);

	void apply(const MTPDupdateTranscribedAudio &update);

	[[nodiscard]] bool freeFor(not_null<HistoryItem*> item) const;
	[[nodiscard]] bool isRated(not_null<HistoryItem*> item) const;
	void rate(not_null<HistoryItem*> item, bool isGood);

	[[nodiscard]] bool trialsSupport();
	[[nodiscard]] TimeId trialsRefreshAt();
	[[nodiscard]] int trialsCount();
	[[nodiscard]] crl::time trialsMaxLengthMs() const;

private:
	void load(not_null<HistoryItem*> item);
	void summarize(not_null<HistoryItem*> item);

	const not_null<Main::Session*> _session;
	MTP::Sender _api;

	int _trialsCount = -1;
	std::optional<bool> _trialsSupport;
	TimeId _trialsRefreshAt = -1;

	base::flat_map<FullMsgId, Entry> _map;
	base::flat_map<uint64, FullMsgId> _ids;

	base::flat_map<FullMsgId, SummaryEntry> _summaries;

	rpl::lifetime _lifetime;

};

} // namespace Api
