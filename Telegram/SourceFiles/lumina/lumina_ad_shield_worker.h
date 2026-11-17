/*
This file is part of LuminaGram,
a fork of Telegram Desktop.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "lumina/lumina_ad_shield_account.h"
#include "lumina/lumina_ad_shield_fs.h"
#include "lumina/lumina_ad_shield_images.h"

#include <crl/crl_time.h>

#include <QtCore/QByteArray>

#include <map>
#include <memory>

namespace Lumina::AdShield {

// Everything the main thread hands to the AdShield queue is a plain copy:
// the queue never touches a HistoryItem, a PeerData or Lumina::Settings.

// A photo of an observed message, for the similar-image hook (SPEC 5.4).
struct PhotoProbe {
	std::int64_t msgId = 0;
	QByteArray inlineBytes;
	int width = 0;
	int height = 0;
};

// The chat information a ChatContext is built from, with the version the
// main thread gave it. The queue keeps one ChatContext per chat and rebuilds
// it only when the version changes.
struct ContextSnapshot {
	std::shared_ptr<const ChatContextData> data;
	std::uint64_t version = 0;
};

struct ObserveBatch {
	std::int64_t userId = 0;
	std::int64_t chatId = 0;
	ContextSnapshot context;
	std::vector<MessageInput> messages;
	std::vector<PhotoProbe> photos;
	std::int64_t now = 0;
};

struct DecideRequest {
	std::int64_t userId = 0;
	std::int64_t chatId = 0;
	ContextSnapshot context;
	Settings settings;
	std::vector<std::int64_t> ids;
	std::int64_t now = 0;
};

using DecisionList = std::vector<std::pair<std::int64_t, Decision>>;

// The AdShield queue: lives on a crl::object_on_queue, one per process, and
// only while some account has the feature effectively on (or while a
// logout / clear has data on disk to remove).
//
// Lifecycle (SPEC 8.4): the constructor purges orphans under the root; every
// change marks its chat dirty, flushDue() writes a chat once it has been quiet
// for kFlushDelay, flushAll() writes everything (backgrounding, quitting), and
// both enforce the 10 MiB total cap afterwards. At most kMaxLoadedChats chats
// stay in memory; the least recently used one is written and unloaded.
//
// Logs carry counts and timings only.
class Worker final {
public:
	static constexpr auto kFlushDelay = crl::time(5000);
	static constexpr auto kMaxLoadedChats = std::size_t(8);
	static constexpr auto kMaxChatSlots = std::size_t(64);

	Worker(QString root, Key key);
	~Worker();

	void observe(ObserveBatch &&batch);
	void remove(
		std::int64_t userId,
		std::int64_t chatId,
		const std::vector<std::int64_t> &ids,
		std::int64_t now);
	[[nodiscard]] DecisionList decide(const DecideRequest &request);
	[[nodiscard]] MarkResult markNotAd(
		std::int64_t userId,
		std::int64_t chatId,
		const ContextSnapshot &context,
		std::int64_t msgId,
		std::int64_t now);

	// Messages of the last 7 days (by message date) that decide() folds now,
	// for the "folded in the last 7 days" line of the group menu.
	[[nodiscard]] int countFolded(
		std::int64_t userId,
		std::int64_t chatId,
		const ContextSnapshot &context,
		const Settings &settings,
		std::int64_t now);

	// Writes the chats that have been quiet for kFlushDelay. Returns whether
	// any chat is still waiting to be written.
	bool flushDue(crl::time now);
	void flushAll();

	// Logout, or the feature became fully inactive for this account:
	// forgetAll() it, even when it was never loaded in this run (its
	// directory may be left from an earlier run).
	void forget(std::int64_t userId);

	// "Clear local data": every account is forgotten, the whole root goes,
	// the similar-image tables go. Writing starts again on the next observe.
	void clearAll();

	// Panic wipe: as clearAll(), and nothing is written by this object again.
	void wipe();

private:
	struct ChatSlot {
		ContextSnapshot snapshot;
		std::unique_ptr<ChatContext> context;
		SimilarImages images;
		bool dirty = false;
		crl::time lastChange = 0;
		crl::time lastUse = 0;
	};
	using ChatKey = std::pair<std::int64_t, std::int64_t>;

	[[nodiscard]] ChatSlot &slot(std::int64_t userId, std::int64_t chatId);
	[[nodiscard]] const ChatContext &context(
		ChatSlot &slot,
		const ContextSnapshot &snapshot);
	[[nodiscard]] Account *account(std::int64_t userId);
	[[nodiscard]] std::shared_ptr<Store> store(
		std::int64_t userId,
		std::int64_t chatId);
	void markDirty(ChatSlot &slot);
	void substitutePhotos(ChatSlot &slot, ObserveBatch &batch);
	void limitLoaded(const ChatKey &keep);
	void limitSlots(const ChatKey &keep);
	bool flushChat(const ChatKey &key, ChatSlot &slot);
	void afterFlush();

	QString _root;
	std::string _rootUtf8;
	Key _key = {};
	QtFileSystem _fs;
	std::map<std::int64_t, std::unique_ptr<Account>> _accounts;
	std::map<ChatKey, ChatSlot> _chats;
	bool _wiped = false;

};

} // namespace Lumina::AdShield
