/*
This file is part of LuminaGram,
a fork of Telegram Desktop.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "lumina/lumina_ad_shield_worker.h"

#include <algorithm>
#include <set>
#include <tuple>

namespace Lumina::AdShield {
namespace {

constexpr auto kWeek = std::int64_t(7) * kDay;

[[nodiscard]] std::uint64_t SenderKey(
		const Key &key,
		std::int64_t senderId) {
	return Hash(key, "sender", std::string_view(std::to_string(senderId)));
}

} // namespace

Worker::Worker(QString root, Key key)
: _root(std::move(root))
, _rootUtf8(_root.toStdString())
, _key(key)
, _fs(_rootUtf8) {
	const auto removed = PurgeOrphans(_fs, _rootUtf8, _key);
	LOG(("Lumina AdShield: queue started, %1 orphan entries removed."
		).arg(removed));
}

Worker::~Worker() = default;

Worker::ChatSlot &Worker::slot(std::int64_t userId, std::int64_t chatId) {
	const auto key = ChatKey(userId, chatId);
	auto &result = _chats[key];
	result.lastUse = crl::now();
	if (_chats.size() > kMaxChatSlots) {
		// Never erases `key`, and std::map keeps other references valid.
		limitSlots(key);
	}
	return result;
}

const ChatContext &Worker::context(
		ChatSlot &slot,
		const ContextSnapshot &snapshot) {
	// The slot keeps its context data alive, so a different pointer is
	// always a different context; the version is unique per run as well.
	const auto stale = !slot.context
		|| (snapshot.data
			&& (slot.snapshot.data != snapshot.data
				|| slot.snapshot.version != snapshot.version));
	if (stale) {
		slot.snapshot = snapshot;
		slot.context = std::make_unique<ChatContext>(snapshot.data
			? *snapshot.data
			: ChatContextData{ .type = ChatType::Other });
	}
	return *slot.context;
}

Account *Worker::account(std::int64_t userId) {
	if (_wiped) {
		return nullptr;
	}
	auto i = _accounts.find(userId);
	if (i != _accounts.end() && i->second->forgotten()) {
		_accounts.erase(i);
		i = _accounts.end();
	}
	if (i == _accounts.end()) {
		i = _accounts.emplace(
			userId,
			std::make_unique<Account>(userId, _key, &_fs, _rootUtf8)).first;
	}
	return i->second.get();
}

std::shared_ptr<Store> Worker::store(
		std::int64_t userId,
		std::int64_t chatId) {
	const auto owner = account(userId);
	if (!owner) {
		return nullptr;
	}
	auto result = owner->chat(chatId);
	limitLoaded(ChatKey(userId, chatId));
	return result;
}

void Worker::markDirty(ChatSlot &slot) {
	slot.dirty = true;
	slot.lastChange = crl::now();
}

void Worker::substitutePhotos(ChatSlot &slot, ObserveBatch &batch) {
	if (batch.photos.empty()) {
		return;
	}
	auto probes = std::map<std::int64_t, const PhotoProbe*>();
	for (const auto &probe : batch.photos) {
		probes.emplace(probe.msgId, &probe);
	}
	// Messages are already in (date, id) order, so within one batch the
	// earliest of several similar photos becomes the one the others map to.
	for (auto &message : batch.messages) {
		if (message.media.kind != MediaKind::Photo
			|| message.media.id.empty()) {
			continue;
		}
		const auto i = probes.find(message.id);
		if (i == probes.end()) {
			continue;
		}
		const auto hash = InlineThumbnailHash(i->second->inlineBytes);
		if (!hash) {
			continue;
		}
		message.media.id = slot.images.canonical(
			message.id,
			SenderKey(_key, message.sender.id),
			*hash,
			AspectGroup(i->second->width, i->second->height),
			message.media.id,
			message.date,
			batch.now);
	}
}

void Worker::observe(ObserveBatch &&batch) {
	if (_wiped || batch.messages.empty()) {
		return;
	}
	const auto started = crl::now();
	auto &chat = slot(batch.userId, batch.chatId);
	const auto &chatContext = context(chat, batch.context);
	const auto chatStore = store(batch.userId, batch.chatId);
	if (!chatStore) {
		return;
	}
	std::sort(
		batch.messages.begin(),
		batch.messages.end(),
		[](const MessageInput &a, const MessageInput &b) {
			return std::tie(a.date, a.id) < std::tie(b.date, b.id);
		});
	substitutePhotos(chat, batch);
	auto observed = 0;
	for (const auto &message : batch.messages) {
		const auto result = chatStore->observe(
			message,
			chatContext,
			batch.now);
		if (result == ObserveResult::Observed) {
			++observed;
		}
	}
	if (observed) {
		markDirty(chat);
	}
	DEBUG_LOG(("Lumina AdShield: observed %1 of %2 messages in %3 ms."
		).arg(observed
		).arg(batch.messages.size()
		).arg(crl::now() - started));
}

void Worker::remove(
		std::int64_t userId,
		std::int64_t chatId,
		const std::vector<std::int64_t> &ids,
		std::int64_t now) {
	if (_wiped || ids.empty()) {
		return;
	}
	auto &chat = slot(userId, chatId);
	const auto chatStore = store(userId, chatId);
	if (chatStore && chatStore->remove(ids, now)) {
		markDirty(chat);
	}
}

DecisionList Worker::decide(const DecideRequest &request) {
	auto result = DecisionList();
	if (_wiped || request.ids.empty()) {
		return result;
	}
	const auto started = crl::now();
	auto &chat = slot(request.userId, request.chatId);
	const auto &chatContext = context(chat, request.context);
	const auto chatStore = store(request.userId, request.chatId);
	if (!chatStore) {
		return result;
	}
	result.reserve(request.ids.size());
	auto folded = 0;
	for (const auto id : request.ids) {
		const auto decision = chatStore->decide(
			id,
			chatContext,
			request.settings,
			request.now);
		if (decision.fold) {
			++folded;
		}
		result.emplace_back(id, decision);
	}
	DEBUG_LOG(("Lumina AdShield: decided %1 messages (%2 fold) in %3 ms."
		).arg(result.size()
		).arg(folded
		).arg(crl::now() - started));
	return result;
}

MarkResult Worker::markNotAd(
		std::int64_t userId,
		std::int64_t chatId,
		const ContextSnapshot &snapshot,
		std::int64_t msgId,
		std::int64_t now) {
	if (_wiped) {
		return MarkResult::Refused;
	}
	auto &chat = slot(userId, chatId);
	const auto &chatContext = context(chat, snapshot);
	const auto chatStore = store(userId, chatId);
	if (!chatStore) {
		return MarkResult::Refused;
	}
	const auto result = chatStore->markNotAd(msgId, chatContext, now);
	if (result == MarkResult::Recorded) {
		markDirty(chat);
	}
	return result;
}

int Worker::countFolded(
		std::int64_t userId,
		std::int64_t chatId,
		const ContextSnapshot &snapshot,
		const Settings &settings,
		std::int64_t now) {
	if (_wiped) {
		return 0;
	}
	auto &chat = slot(userId, chatId);
	const auto &chatContext = context(chat, snapshot);
	const auto chatStore = store(userId, chatId);
	if (!chatStore) {
		return 0;
	}
	// Counted as the chat shows them: an album is one fold, and all of its
	// parts share one decision, so one part of each is enough.
	auto ids = std::vector<std::int64_t>();
	auto albums = std::set<std::int64_t>();
	for (const auto &[rid, row] : chatStore->rows()) {
		if (row.msgId == 0 || row.date < now - kWeek || row.date > now) {
			continue;
		} else if (row.album != 0 && !albums.emplace(row.album).second) {
			continue;
		}
		ids.push_back(std::int64_t(row.msgId));
	}
	auto result = 0;
	for (const auto id : ids) {
		if (chatStore->decide(id, chatContext, settings, now).fold) {
			++result;
		}
	}
	return result;
}

void Worker::limitLoaded(const ChatKey &keep) {
	struct Loaded {
		crl::time lastUse = 0;
		ChatKey key;
	};
	auto loaded = std::vector<Loaded>();
	for (const auto &[userId, owner] : _accounts) {
		for (const auto chatId : owner->loadedChats()) {
			const auto key = ChatKey(userId, chatId);
			const auto i = _chats.find(key);
			loaded.push_back({
				.lastUse = (i != _chats.end()) ? i->second.lastUse : 0,
				.key = key,
			});
		}
	}
	if (loaded.size() <= kMaxLoadedChats) {
		return;
	}
	std::sort(loaded.begin(), loaded.end(), [](
			const Loaded &a,
			const Loaded &b) {
		return std::tie(a.lastUse, a.key) < std::tie(b.lastUse, b.key);
	});
	auto excess = loaded.size() - kMaxLoadedChats;
	auto flushed = false;
	for (const auto &entry : loaded) {
		if (!excess) {
			break;
		} else if (entry.key == keep) {
			continue;
		}
		const auto i = _chats.find(entry.key);
		if (i != _chats.end() && i->second.dirty) {
			flushChat(entry.key, i->second);
			flushed = true;
		}
		const auto owner = _accounts.find(entry.key.first);
		if (owner != _accounts.end()) {
			owner->second->unload(entry.key.second);
		}
		--excess;
	}
	if (flushed) {
		afterFlush();
	}
}

void Worker::limitSlots(const ChatKey &keep) {
	// Only the context and the similar-image table live in a slot without a
	// loaded store; both are rebuilt from the next batch. Dirty slots stay.
	while (_chats.size() > kMaxChatSlots) {
		auto oldest = _chats.end();
		for (auto i = _chats.begin(); i != _chats.end(); ++i) {
			if (i->first == keep || i->second.dirty) {
				continue;
			}
			const auto owner = _accounts.find(i->first.first);
			const auto loaded = (owner != _accounts.end())
				&& owner->second->loaded(i->first.second);
			if (loaded) {
				continue;
			}
			if (oldest == _chats.end()
				|| i->second.lastUse < oldest->second.lastUse) {
				oldest = i;
			}
		}
		if (oldest == _chats.end()) {
			return;
		}
		_chats.erase(oldest);
	}
}

bool Worker::flushChat(const ChatKey &key, ChatSlot &slot) {
	slot.dirty = false;
	const auto i = _accounts.find(key.first);
	if (i == _accounts.end() || i->second->forgotten()) {
		return false;
	}
	const auto result = i->second->flush(key.second);
	if (!result) {
		LOG(("Lumina AdShield Error: could not write a chat file."));
	}
	return result;
}

void Worker::afterFlush() {
	const auto removed = EnforceTotalCap(_fs, _rootUtf8);
	if (removed) {
		LOG(("Lumina AdShield: total size cap removed %1 chat files."
			).arg(removed));
	}
}

bool Worker::flushDue(crl::time now) {
	if (_wiped) {
		return false;
	}
	auto flushed = 0;
	auto remaining = false;
	for (auto &[key, chat] : _chats) {
		if (!chat.dirty) {
			continue;
		} else if (chat.lastChange + kFlushDelay > now) {
			remaining = true;
			continue;
		}
		flushChat(key, chat);
		++flushed;
	}
	if (flushed) {
		afterFlush();
		DEBUG_LOG(("Lumina AdShield: wrote %1 chat files.").arg(flushed));
	}
	return remaining;
}

void Worker::flushAll() {
	if (_wiped) {
		return;
	}
	auto flushed = 0;
	for (auto &[key, chat] : _chats) {
		if (chat.dirty) {
			flushChat(key, chat);
			++flushed;
		}
	}
	if (flushed) {
		afterFlush();
	}
}

void Worker::forget(std::int64_t userId) {
	const auto i = _accounts.find(userId);
	if (i != _accounts.end()) {
		i->second->forgetAll();
		_accounts.erase(i);
	} else if (!_wiped) {
		// Not loaded in this run: its directory may still be on disk.
		Account(userId, _key, &_fs, _rootUtf8).forgetAll();
	}
	for (auto j = _chats.begin(); j != _chats.end();) {
		if (j->first.first == userId) {
			j = _chats.erase(j);
		} else {
			++j;
		}
	}
	LOG(("Lumina AdShield: forgot one account."));
}

void Worker::clearAll() {
	for (const auto &[userId, owner] : _accounts) {
		owner->forgetAll();
	}
	_accounts.clear();
	_chats.clear();
	WipeRoot(_fs, _rootUtf8);
	LOG(("Lumina AdShield: local data cleared."));
}

void Worker::wipe() {
	clearAll();
	_wiped = true;
}

} // namespace Lumina::AdShield
