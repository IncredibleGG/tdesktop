/*
This file is part of LuminaGram,
a fork of Telegram Desktop.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "lumina/lumina_ad_shield_manager.h"

#include "api/api_chat_participants.h"
#include "apiwrap.h"
#include "base/timer.h"
#include "base/unixtime.h"
#include "core/application.h"
#include "data/data_changes.h"
#include "data/data_channel.h"
#include "data/data_chat.h"
#include "data/data_media_types.h"
#include "data/data_document.h"
#include "data/data_peer.h"
#include "data/data_photo.h"
#include "data/data_session.h"
#include "data/data_user.h"
#include "history/history.h"
#include "history/history_item.h"
#include "history/history_item_components.h"
#include "history/history_item_reply_markup.h"
#include "history/view/history_view_element.h"
#include "lumina/lumina_ad_shield_worker.h"
#include "lumina/lumina_message_filter.h"
#include "lumina/lumina_secret_store.h"
#include "main/main_account.h"
#include "main/main_domain.h"
#include "main/main_session.h"
#include "settings.h" // cWorkingDir()

#include <crl/crl_object_on_queue.h>

#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QRandomGenerator>
#include <QtCore/QSaveFile>

#include <unordered_map>

namespace Lumina::AdShield {
namespace {

// Messages turned into MessageInputs per main-loop turn; the rest waits for
// the next turn, so opening a long history never stalls the UI.
constexpr auto kBatchLimit = 300;

// Decisions are asked for this long after the last observe, so a history
// slice arriving in several pieces costs one decide pass, not several.
constexpr auto kDecideDelay = crl::time(60);

// Pinned messages whose links count as trusted (SPEC 5.1), per group.
constexpr auto kMaxPinnedTexts = 16;

// While a group's admin list is not loaded, it is looked at again this
// often, at most this many times (then only peer updates bring it back).
constexpr auto kAdminsPollDelay = crl::time(2000);
constexpr auto kAdminsPollLimit = 30;

bool Active = false;
bool Constructed = false;

[[nodiscard]] std::u32string U32(const QString &text) {
	const auto ucs4 = text.toUcs4();
	auto result = std::u32string();
	result.reserve(ucs4.size());
	for (const auto c : ucs4) {
		result.push_back(char32_t(c));
	}
	return result;
}

// Bot API style ids, SPEC 3: users <id>, basic groups -<id>, supergroups
// and channels -100<id>.
[[nodiscard]] std::int64_t BotApiId(not_null<const PeerData*> peer) {
	if (peer->isUser()) {
		return std::int64_t(peerToUser(peer->id).bare);
	} else if (peer->isChat()) {
		return -std::int64_t(peerToChat(peer->id).bare);
	} else if (peer->isChannel()) {
		return -1000000000000LL - std::int64_t(peerToChannel(peer->id).bare);
	}
	return 0;
}

[[nodiscard]] ChatType ChatTypeFor(not_null<PeerData*> peer) {
	if (peer->isChat()) {
		return ChatType::Group;
	} else if (const auto channel = peer->asChannel()) {
		if (channel->isMonoforum()) {
			return ChatType::Other;
		} else if (channel->isForum()) {
			return ChatType::Forum;
		} else if (channel->isGigagroup()) {
			return ChatType::Gigagroup;
		} else if (channel->isMegagroup()) {
			return ChatType::Supergroup;
		}
	}
	return ChatType::Other;
}

[[nodiscard]] QString KeyToHex(const Key &key) {
	return QString::fromLatin1(QByteArray(
		reinterpret_cast<const char*>(key.data()),
		int(key.size())).toHex());
}

[[nodiscard]] std::optional<Key> KeyFromHex(const QString &hex) {
	const auto bytes = QByteArray::fromHex(hex.toLatin1());
	if (hex.size() != 32 || bytes.size() != 16) {
		return std::nullopt;
	}
	auto result = Key();
	for (auto i = 0; i != 16; ++i) {
		result[i] = static_cast<unsigned char>(bytes[i]);
	}
	return result;
}

[[nodiscard]] Key RandomKey() {
	auto words = std::array<quint32, 4>();
	QRandomGenerator::system()->fillRange(words.data(), int(words.size()));
	auto result = Key();
	for (auto i = 0; i != 16; ++i) {
		result[i] = static_cast<unsigned char>(
			(words[i / 4] >> (8 * (i % 4))) & 0xFF);
	}
	return result;
}

// What makes an already observed message worth observing again.
struct Stamp {
	std::size_t text = 0;
	std::uint64_t media = 0;
	bool pinned = false;

	friend inline bool operator==(const Stamp &a, const Stamp &b) {
		return (a.text == b.text)
			&& (a.media == b.media)
			&& (a.pinned == b.pinned);
	}
};

[[nodiscard]] Stamp ComputeStamp(not_null<HistoryItem*> item) {
	auto result = Stamp();
	result.text = std::size_t(qHash(item->originalText().text));
	if (const auto media = item->media()) {
		if (const auto photo = media->photo()) {
			result.media = std::uint64_t(photo->id);
		} else if (const auto document = media->document()) {
			result.media = std::uint64_t(document->id);
		} else {
			result.media = 1;
		}
	}
	result.pinned = item->isPinned();
	return result;
}

// Decisions that are not a judgement yet: a change from one of these to
// "fold" is the first real answer, not a cluster triggering under the
// reader's eyes.
[[nodiscard]] bool Settled(State state) {
	return (state != State::Pending)
		&& (state != State::Untracked)
		&& (state != State::Ineligible);
}

class Manager final {
public:
	[[nodiscard]] static Manager &Instance();

	void start();

	void note(not_null<HistoryItem*> item);
	void sessionLoggingOut(not_null<Main::Session*> session);
	void flushNow();
	void panicWipe();
	void clearLocalData();

	[[nodiscard]] ChatMode chatMode(not_null<PeerData*> peer) const;
	void setChatMode(not_null<PeerData*> peer, ChatMode mode);
	[[nodiscard]] bool chatEnabled(not_null<PeerData*> peer) const;

	[[nodiscard]] const Decision *cached(
		not_null<const HistoryItem*> item) const;
	[[nodiscard]] bool shouldFold(not_null<const HistoryItem*> item) const;
	[[nodiscard]] bool shouldMarkTrial(
		not_null<const HistoryItem*> item) const;
	[[nodiscard]] bool expanded(not_null<const HistoryItem*> item) const;
	void setExpanded(
		not_null<History*> history,
		const std::vector<MsgId> &ids,
		bool expanded);
	void chatShown(not_null<History*> history);
	void markNotAd(not_null<HistoryItem*> item);
	void requestFoldedCount(not_null<PeerData*> peer, Fn<void(int)> done);

	[[nodiscard]] rpl::producer<DecisionsChange> decisionsChanges() const {
		return _decisionsChanges.events();
	}
	[[nodiscard]] rpl::producer<> displayChanges() const {
		return _displayChanges.events();
	}

private:
	struct Observed {
		Stamp stamp;
		bool filtered = false;
	};
	struct ChatState {
		explicit ChatState(not_null<PeerData*> peer) : peer(peer) {
		}

		not_null<PeerData*> peer;
		std::int64_t chatId = 0;
		ContextSnapshot context;
		bool contextDirty = true;
		bool adminsRequested = false;
		std::vector<QString> pinnedTexts;
		base::flat_map<MsgId, Observed> observed;
		base::flat_set<MsgId> pending;
		std::vector<std::int64_t> removed;
		bool decideDirty = false;
		bool decideInFlight = false;
		// The next decisions follow a change the user made (sensitivity,
		// exemptions): nothing is "triggering under the reader's eyes".
		bool noDeferOnce = false;
		int adminsPolls = 0;
	};
	struct Cached {
		Decision decision;
		bool deferred = false;
	};
	struct SessionState {
		explicit SessionState(not_null<Main::Session*> session)
		: session(session) {
		}

		not_null<Main::Session*> session;
		quint64 uniqueId = 0;
		base::flat_map<PeerId, std::unique_ptr<ChatState>> chats;
		base::flat_set<PeerId> disabledChats;
		std::unordered_map<FullMsgId, Cached> decisions;
		base::flat_set<FullMsgId> expanded;
		rpl::lifetime lifetime;
	};

	Manager();

	[[nodiscard]] static bool SessionActive(quint64 uniqueId);
	[[nodiscard]] std::int64_t userIdFor(const SessionState &state) const {
		return std::int64_t(state.uniqueId);
	}

	[[nodiscard]] SessionState *sessionState(
		not_null<Main::Session*> session,
		bool create);
	[[nodiscard]] ChatState *chatState(
		SessionState &state,
		not_null<PeerData*> peer,
		bool create);
	void subscribe(SessionState &state);
	void hookTeardown(not_null<Main::Session*> session);
	void dropSession(quint64 uniqueId, bool notify = true);
	void dropChat(SessionState &state, PeerId peerId);
	void fireWholeChat(const SessionState &state, PeerId peerId);

	void refreshFlags();
	void refreshActivation();
	void scanLoaded(SessionState &state, PeerData *only);
	void noteLoaded(
		SessionState &state,
		ChatState &chat,
		not_null<History*> history);
	void reobserveAll(ChatState &chat);

	void scheduleProcess();
	void processPending();
	void refreshContext(SessionState &state, ChatState &chat);
	[[nodiscard]] ChatContextData buildContext(
		const SessionState &state,
		const ChatState &chat) const;
	[[nodiscard]] Sender buildSender(
		const SessionState &state,
		const ChatState &chat,
		not_null<HistoryItem*> item) const;
	[[nodiscard]] MessageInput buildInput(
		const SessionState &state,
		ChatState &chat,
		not_null<HistoryItem*> item,
		std::vector<PhotoProbe> &photos);
	void rememberPinned(ChatState &chat, const QString &text);

	void scheduleDecide();
	void runDecides();
	void pollAdmins();
	void applyDecisions(
		std::uint64_t generation,
		quint64 uniqueId,
		PeerId peerId,
		bool allowDefer,
		DecisionList &&list);

	void onEdited(SessionState &state, not_null<HistoryItem*> item);
	void onDestroying(
		SessionState &state,
		const std::vector<not_null<HistoryItem*>> &items);
	void onFreed(SessionState &state, not_null<const HistoryItem*> item);
	void onPeerUpdated(
		SessionState &state,
		not_null<PeerData*> peer,
		Data::PeerUpdate::Flags flags);
	void onPinnedChanged(SessionState &state, not_null<History*> history);

	[[nodiscard]] std::optional<Key> loadKey(bool create);
	bool ensureWorker(bool createKey);
	void destroyWorkerIfIdle();
	void retireWorker();
	void resetInFlight();
	void forgetData(quint64 uniqueId);
	void scheduleFlush();
	void flushTimerDone();

	[[nodiscard]] const SessionState *findState(
		not_null<const HistoryItem*> item) const;

	bool _started = false;
	bool _anyActive = false;
	bool _trial = false;
	bool _processScheduled = false;
	QString _root;
	QString _keyPath;

	std::unique_ptr<crl::object_on_queue<Worker>> _worker;
	std::uint64_t _generation = 0;

	// False while the queue runs on a throw-away key: it only cleans up after
	// a lost key file and must never write anything that has to be read
	// back later.
	bool _persistentKey = false;

	base::flat_map<quint64, std::unique_ptr<SessionState>> _sessions;
	base::flat_set<quint64> _inactiveSessions;
	base::flat_map<quint64, bool> _wasActive;
	base::flat_set<not_null<Main::Session*>> _teardownHooked;

	base::Timer _flushTimer;
	base::Timer _decideTimer;
	base::Timer _adminsTimer;

	rpl::event_stream<DecisionsChange> _decisionsChanges;
	rpl::event_stream<> _displayChanges;
	rpl::lifetime _lifetime;

};

Manager &Manager::Instance() {
	// Deliberately leaked: the queue object must not be destroyed during
	// static destruction, when crl's own statics may already be gone.
	// Everything pending is written by FlushNow() from readyToQuit().
	static const auto result = new Manager();
	Constructed = true;
	return *result;
}

Manager::Manager()
: _flushTimer([=] { flushTimerDone(); })
, _decideTimer([=] { runDecides(); })
, _adminsTimer([=] { pollAdmins(); }) {
}

bool Manager::SessionActive(quint64 uniqueId) {
	return GlobalEnabled() || AnyChatOn(uniqueId);
}

void Manager::start() {
	if (_started) {
		return;
	}
	_started = true;
	_root = cWorkingDir() + u"tdata/lumina_adshield"_q;
	_keyPath = cWorkingDir() + u"tdata/luminagram_adshield.key"_q;
	refreshFlags();

	ActivationChanges(
	) | rpl::on_next([=] {
		refreshActivation();
	}, _lifetime);

	DecisionSettingsChanges(
	) | rpl::on_next([=] {
		for (const auto &[uniqueId, state] : _sessions) {
			for (const auto &[peerId, chat] : state->chats) {
				chat->decideDirty = true;
				chat->noDeferOnce = true;
			}
		}
		scheduleDecide();
	}, _lifetime);

	TrialChanges(
	) | rpl::on_next([=] {
		_trial = TrialMode();
		_displayChanges.fire({});
	}, _lifetime);

	// The keyword filter runs first: what it hides is not AdShield's to see.
	MessageFilterChanges(
	) | rpl::on_next([=] {
		for (const auto &[uniqueId, state] : _sessions) {
			for (const auto &[peerId, chat] : state->chats) {
				reobserveAll(*chat);
			}
		}
		scheduleProcess();
	}, _lifetime);

	if (Core::IsAppLaunched()) {
		Core::App().appDeactivatedValue(
		) | rpl::filter(
			rpl::mappers::_1
		) | rpl::on_next([=] {
			if (_worker) {
				_worker->with([](Worker &worker) {
					worker.flushAll();
				});
			}
		}, _lifetime);
	}
}

void Manager::refreshFlags() {
	_anyActive = GlobalEnabled() || AnyChatOnAnywhere();
	_trial = TrialMode();
	Active = _anyActive;
}

void Manager::refreshActivation() {
	const auto wasAnyActive = _anyActive;
	refreshFlags();
	_inactiveSessions.clear();

	// Sessions with state: deactivate, or drop the groups that went off.
	auto dropped = std::vector<quint64>();
	for (const auto &[uniqueId, state] : _sessions) {
		if (!SessionActive(uniqueId)) {
			dropped.push_back(uniqueId);
			continue;
		}
		state->disabledChats.clear();
		auto off = std::vector<PeerId>();
		for (const auto &[peerId, chat] : state->chats) {
			if (!chatEnabled(chat->peer)) {
				off.push_back(peerId);
			}
		}
		for (const auto peerId : off) {
			dropChat(*state, peerId);
		}
	}
	for (const auto uniqueId : dropped) {
		dropSession(uniqueId);
	}

	// Every logged-in account: forget the data of one that just became
	// fully inactive (also when it had no state in this run - its files may
	// be from an earlier one), and pick up what is already on screen for
	// one that just became active.
	if (Core::IsAppLaunched() && Core::App().domain().started()) {
		for (const auto &[index, account] : Core::App().domain().accounts()) {
			const auto session = account->maybeSession();
			if (!session) {
				continue;
			}
			const auto uniqueId = session->uniqueId();
			const auto active = SessionActive(uniqueId);
			const auto i = _wasActive.find(uniqueId);
			const auto was = (i != _wasActive.end()) ? i->second : true;
			_wasActive[uniqueId] = active;
			if (!active && was) {
				forgetData(uniqueId);
			} else if (active) {
				if (const auto state = sessionState(session, true)) {
					scanLoaded(*state, nullptr);
				}
			}
		}
	}
	if (wasAnyActive && !_anyActive) {
		LOG(("Lumina AdShield: turned off everywhere."));
	}
	destroyWorkerIfIdle();
}

Manager::SessionState *Manager::sessionState(
		not_null<Main::Session*> session,
		bool create) {
	const auto uniqueId = session->uniqueId();
	const auto i = _sessions.find(uniqueId);
	if (i != _sessions.end()) {
		return i->second.get();
	} else if (!create || !_anyActive || _inactiveSessions.contains(uniqueId)) {
		return nullptr;
	} else if (!SessionActive(uniqueId)) {
		_inactiveSessions.emplace(uniqueId);
		return nullptr;
	} else if (!ensureWorker(true)) {
		return nullptr;
	}
	auto state = std::make_unique<SessionState>(session);
	state->uniqueId = uniqueId;
	const auto result = state.get();
	_sessions.emplace(uniqueId, std::move(state));
	_wasActive[uniqueId] = true;
	subscribe(*result);
	hookTeardown(session);
	return result;
}

Manager::ChatState *Manager::chatState(
		SessionState &state,
		not_null<PeerData*> peer,
		bool create) {
	const auto i = state.chats.find(peer->id);
	if (i != state.chats.end()) {
		return i->second.get();
	} else if (!create || state.disabledChats.contains(peer->id)) {
		return nullptr;
	} else if (!chatEnabled(peer)) {
		state.disabledChats.emplace(peer->id);
		return nullptr;
	}
	auto chat = std::make_unique<ChatState>(peer);
	chat->chatId = BotApiId(peer);
	const auto result = chat.get();
	state.chats.emplace(peer->id, std::move(chat));
	return result;
}

void Manager::subscribe(SessionState &state) {
	const auto session = state.session;
	const auto uniqueId = state.uniqueId;
	const auto find = [=]() -> SessionState* {
		const auto i = _sessions.find(uniqueId);
		return (i != _sessions.end()) ? i->second.get() : nullptr;
	};

	// Session::clear() destroys every message object: from here on nothing
	// that happens to them means anything.
	session->data().sessionDataAboutToBeCleared(
	) | rpl::on_next([=] {
		dropSession(uniqueId);
	}, state.lifetime);

	session->changes().messageUpdates(
		Data::MessageUpdate::Flag::Edited
	) | rpl::on_next([=](const Data::MessageUpdate &update) {
		if (const auto state = find()) {
			onEdited(*state, update.item);
		}
	}, state.lifetime);

	// Real deletions only (server, "delete for me", TTL, history cleared up
	// to a point). Data::Session::itemRemoved also fires when a message
	// object is merely freed, so it must never anonymise anything.
	session->data().itemsAboutToBeDestroyed(
	) | rpl::on_next([=](const std::vector<not_null<HistoryItem*>> &items) {
		if (const auto state = find()) {
			onDestroying(*state, items);
		}
	}, state.lifetime);

	session->data().itemRemoved(
	) | rpl::on_next([=](not_null<const HistoryItem*> item) {
		if (const auto state = find()) {
			onFreed(*state, item);
		}
	}, state.lifetime);

	using PeerFlag = Data::PeerUpdate::Flag;
	session->changes().peerUpdates(
		PeerFlag::Admins
		| PeerFlag::Members
		| PeerFlag::Username
		| PeerFlag::Usernames
		| PeerFlag::About
		| PeerFlag::DiscussionLink
		| PeerFlag::FullInfo
		| PeerFlag::Rights
	) | rpl::on_next([=](const Data::PeerUpdate &update) {
		if (const auto state = find()) {
			onPeerUpdated(*state, update.peer, update.flags);
		}
	}, state.lifetime);

	session->changes().entryUpdates(
		Data::EntryUpdate::Flag::HasPinnedMessages
	) | rpl::on_next([=](const Data::EntryUpdate &update) {
		if (const auto history = update.entry->asHistory()) {
			if (const auto state = find()) {
				onPinnedChanged(*state, history);
			}
		}
	}, state.lifetime);
}

void Manager::hookTeardown(not_null<Main::Session*> session) {
	if (!_teardownHooked.emplace(session).second) {
		return;
	}
	const auto uniqueId = session->uniqueId();
	session->lifetime().add([=] {
		// The session is half destroyed here: do not touch it. Normally
		// sessionDataAboutToBeCleared() has already dropped the state.
		_teardownHooked.remove(session);
		dropSession(uniqueId, false);
	});
}

void Manager::fireWholeChat(const SessionState &state, PeerId peerId) {
	if (const auto history = state.session->data().historyLoaded(peerId)) {
		_decisionsChanges.fire(DecisionsChange{ .history = history });
	}
}

void Manager::dropChat(SessionState &state, PeerId peerId) {
	const auto i = state.chats.find(peerId);
	if (i == state.chats.end()) {
		return;
	}
	state.chats.erase(i);
	for (auto j = state.decisions.begin(); j != state.decisions.end();) {
		if (j->first.peer == peerId) {
			j = state.decisions.erase(j);
		} else {
			++j;
		}
	}
	for (auto j = state.expanded.begin(); j != state.expanded.end();) {
		if (j->peer == peerId) {
			j = state.expanded.erase(j);
		} else {
			++j;
		}
	}
	fireWholeChat(state, peerId);
}

void Manager::dropSession(quint64 uniqueId, bool notify) {
	const auto i = _sessions.find(uniqueId);
	if (i == _sessions.end()) {
		return;
	}
	auto state = std::move(i->second);
	_sessions.erase(i);
	if (notify) {
		for (const auto &[peerId, chat] : state->chats) {
			fireWholeChat(*state, peerId);
		}
	}
	// Destroys the subscriptions with it.
	state = nullptr;
}

void Manager::scanLoaded(SessionState &state, PeerData *only) {
	const auto session = state.session;
	const auto take = [&](not_null<PeerData*> peer) {
		if (!ChatEligible(peer)) {
			return;
		}
		const auto history = session->data().historyLoaded(peer);
		if (!history || history->blocks.empty()) {
			return;
		}
		if (const auto chat = chatState(state, peer, true)) {
			noteLoaded(state, *chat, history);
		}
	};
	if (only) {
		take(only);
	} else {
		session->data().enumerateGroups(take);
	}
}

void Manager::noteLoaded(
		SessionState &state,
		ChatState &chat,
		not_null<History*> history) {
	for (const auto &block : history->blocks) {
		for (const auto &view : block->messages) {
			const auto item = view->data();
			if (item->isRegular()
				&& !item->isService()
				&& IsServerMsgId(item->id)
				&& !chat.observed.contains(item->id)) {
				chat.pending.emplace(item->id);
			}
		}
	}
	if (!chat.pending.empty()) {
		scheduleProcess();
	}
}

void Manager::reobserveAll(ChatState &chat) {
	for (const auto &[id, observed] : chat.observed) {
		chat.pending.emplace(id);
	}
	chat.observed.clear();
}

void Manager::note(not_null<HistoryItem*> item) {
	if (!item->isRegular()
		|| item->isService()
		|| item->isSponsored()
		|| item->isScheduled()
		|| !IsServerMsgId(item->id)) {
		return;
	}
	const auto history = item->history();
	const auto peer = history->peer;
	if (!ChatEligible(peer)) {
		return;
	}
	const auto state = sessionState(&history->session(), true);
	if (!state) {
		return;
	}
	const auto chat = chatState(*state, peer, true);
	if (!chat) {
		return;
	}
	const auto id = item->id;
	const auto i = chat->observed.find(id);
	if (i != chat->observed.end() && i->second.stamp == ComputeStamp(item)) {
		return;
	}
	chat->pending.emplace(id);
	scheduleProcess();
}

void Manager::scheduleProcess() {
	if (_processScheduled) {
		return;
	}
	_processScheduled = true;
	crl::on_main([] {
		Instance().processPending();
	});
}

void Manager::processPending() {
	_processScheduled = false;
	if (!_worker) {
		return;
	}
	const auto now = std::int64_t(base::unixtime::now());
	auto budget = kBatchLimit;
	auto posted = false;
	for (const auto &[uniqueId, state] : _sessions) {
		const auto session = state->session;
		for (const auto &[peerId, chat] : state->chats) {
			if (!chat->removed.empty()) {
				auto ids = base::take(chat->removed);
				const auto userId = userIdFor(*state);
				const auto chatId = chat->chatId;
				_worker->with([=, ids = std::move(ids)](Worker &worker) {
					worker.remove(userId, chatId, ids, now);
				});
				chat->decideDirty = true;
				posted = true;
			}
			refreshContext(*state, *chat);
			if (chat->pending.empty() || budget <= 0) {
				continue;
			}
			auto batch = ObserveBatch();
			batch.userId = userIdFor(*state);
			batch.chatId = chat->chatId;
			batch.now = now;
			while (!chat->pending.empty() && budget > 0) {
				const auto id = *chat->pending.begin();
				chat->pending.erase(chat->pending.begin());
				--budget;
				const auto item = session->data().message(peerId, id);
				if (!item) {
					continue;
				}
				const auto stamp = ComputeStamp(item);
				if (MessageTextFiltered(item->originalText().text)) {
					chat->observed[id] = { .stamp = stamp, .filtered = true };
					state->decisions.erase(FullMsgId(peerId, id));
					continue;
				}
				batch.messages.push_back(
					buildInput(*state, *chat, item, batch.photos));
				chat->observed[id] = { .stamp = stamp, .filtered = false };
			}
			// A newly found pinned message may have changed the trusted
			// links; refreshContext() then queues everything again.
			refreshContext(*state, *chat);
			batch.context = chat->context;
			if (!batch.messages.empty()) {
				_worker->with([batch = std::move(batch)](
						Worker &worker) mutable {
					worker.observe(std::move(batch));
				});
				posted = true;
			}
			chat->decideDirty = true;
		}
	}
	if (posted) {
		scheduleFlush();
	}
	scheduleDecide();
	for (const auto &[uniqueId, state] : _sessions) {
		for (const auto &[peerId, chat] : state->chats) {
			if (!chat->pending.empty()) {
				scheduleProcess();
				return;
			}
		}
	}
}

ChatContextData Manager::buildContext(
		const SessionState &state,
		const ChatState &chat) const {
	const auto peer = chat.peer;
	const auto owner = &peer->owner();
	auto result = ChatContextData();
	result.chatId = chat.chatId;
	result.type = ChatTypeFor(peer);
	result.username = U32(peer->username());
	result.description = U32(peer->about());
	for (const auto &text : chat.pinnedTexts) {
		result.pinnedTexts.push_back(U32(text));
	}
	result.selfUsername = U32(state.session->user()->username());

	auto handles = base::flat_set<QString>();
	const auto addUser = [&](UserData *user) {
		if (user && !user->username().isEmpty()) {
			handles.emplace(user->username());
		}
	};
	if (const auto channel = peer->asChannel()) {
		if (const auto linked = channel->discussionLink()) {
			result.linkedChannelId = BotApiId(linked);
			result.linkedChannelUsername = U32(linked->username());
		}
		if (const auto info = channel->mgInfo.get()) {
			result.adminsKnown = info->adminsLoaded;
			for (const auto &userId : info->admins) {
				addUser(owner->userLoaded(userId));
			}
			for (const auto &[user, admin] : info->lastAdmins) {
				addUser(user);
			}
			addUser(info->creator);
			for (const auto &bot : info->bots) {
				addUser(bot);
			}
		}
	} else if (const auto group = peer->asChat()) {
		result.adminsKnown = !group->noParticipantInfo();
		for (const auto &user : group->admins) {
			addUser(user);
		}
		if (group->creator) {
			addUser(owner->userLoaded(group->creator));
		}
		for (const auto &user : group->participants) {
			if (user->isBot()) {
				addUser(user);
			}
		}
	}
	for (const auto &handle : handles) {
		result.memberHandles.push_back(U32(handle));
	}
	return result;
}

[[nodiscard]] bool SameContext(
		const ChatContextData &a,
		const ChatContextData &b) {
	return (a.chatId == b.chatId)
		&& (a.type == b.type)
		&& (a.username == b.username)
		&& (a.linkedChannelId == b.linkedChannelId)
		&& (a.linkedChannelUsername == b.linkedChannelUsername)
		&& (a.description == b.description)
		&& (a.pinnedTexts == b.pinnedTexts)
		&& (a.adminsKnown == b.adminsKnown)
		&& (a.memberHandles == b.memberHandles)
		&& (a.selfUsername == b.selfUsername);
}

void Manager::refreshContext(SessionState &state, ChatState &chat) {
	if (!chat.contextDirty) {
		return;
	}
	chat.contextDirty = false;
	auto data = buildContext(state, chat);
	if (!data.adminsKnown) {
		if (!chat.adminsRequested) {
			// SPEC 6.9: nothing folds before the admin list is known; ask
			// for it once.
			chat.adminsRequested = true;
			if (const auto channel = chat.peer->asChannel()) {
				state.session->api().chatParticipants().requestAdmins(
					channel);
			} else if (const auto group = chat.peer->asChat()) {
				group->updateFull();
			}
		}
		// The answer does not always come with a PeerUpdate (a failed
		// request only flips the loaded flag): look again in a while.
		if (chat.adminsPolls < kAdminsPollLimit && !_adminsTimer.isActive()) {
			_adminsTimer.callOnce(kAdminsPollDelay);
		}
	}
	const auto had = (chat.context.data != nullptr);
	if (had && SameContext(*chat.context.data, data)) {
		return;
	}
	chat.context.data = std::make_shared<const ChatContextData>(
		std::move(data));
	++chat.context.version;
	if (had) {
		// Roles, trusted links and known members are part of every stored
		// row: observe the on-screen messages again (endorsements upsert,
		// SPEC 6.3 / 6.9).
		reobserveAll(chat);
		scheduleProcess();
	}
}

Sender Manager::buildSender(
		const SessionState &state,
		const ChatState &chat,
		not_null<HistoryItem*> item) const {
	auto result = Sender();
	const auto peer = chat.peer;
	const auto from = item->from();
	if (item->out() || from->isSelf()) {
		result.role = Role::Self;
		result.id = BotApiId(from);
		result.username = U32(from->username());
		return result;
	} else if (from == peer) {
		result.role = Role::AnonAdmin;
		result.id = chat.chatId;
		return result;
	}
	result.id = BotApiId(from);
	result.username = U32(from->username());
	if (const auto channel = from->asChannel()) {
		const auto group = peer->asChannel();
		const auto linked = group ? group->discussionLink() : nullptr;
		result.role = (linked && channel == linked)
			? Role::Channel
			: Role::Member;
		return result;
	}
	const auto user = from->asUser();
	if (!user) {
		return result;
	}
	const auto userId = peerToUser(user->id);
	if (const auto channel = peer->asChannel()) {
		const auto info = channel->mgInfo.get();
		if (info && info->creator == user) {
			result.role = Role::Creator;
		} else if (info && info->admins.contains(userId)) {
			result.role = Role::Admin;
		}
	} else if (const auto group = peer->asChat()) {
		if (group->creator == userId) {
			result.role = Role::Creator;
		} else if (group->admins.contains(user)) {
			result.role = Role::Admin;
		}
	}
	if (result.role == Role::Member) {
		if (user->isBot()) {
			result.role = Role::Bot;
		} else if (user->isContact()) {
			result.role = Role::Contact;
		}
	}
	return result;
}

void Manager::rememberPinned(ChatState &chat, const QString &text) {
	if (text.trimmed().isEmpty() || ranges::contains(chat.pinnedTexts, text)) {
		return;
	}
	chat.pinnedTexts.push_back(text);
	if (chat.pinnedTexts.size() > kMaxPinnedTexts) {
		chat.pinnedTexts.erase(chat.pinnedTexts.begin());
	}
	chat.contextDirty = true;
}

MessageInput Manager::buildInput(
		const SessionState &state,
		ChatState &chat,
		not_null<HistoryItem*> item,
		std::vector<PhotoProbe> &photos) {
	auto result = MessageInput();
	result.id = std::int64_t(item->id.bare);
	result.date = std::int64_t(item->date());
	result.sender = buildSender(state, chat, item);

	const auto &text = item->originalText();
	result.text = U32(text.text);
	for (const auto &entity : text.entities) {
		if (entity.type() == ::EntityType::CustomUrl) {
			result.entities.push_back({
				.type = AdShield::EntityType::TextUrl,
				.url = U32(entity.data()),
			});
		} else if (entity.type() == ::EntityType::MentionName) {
			const auto fields = TextUtilities::MentionNameDataToFields(
				entity.data());
			if (fields.userId) {
				result.entities.push_back({
					.type = AdShield::EntityType::MentionName,
					.userId = std::int64_t(fields.userId),
					.hasUserId = true,
				});
			}
		}
	}

	// SPEC 3 media whitelist: only media attached to the message itself,
	// never a web page preview's picture.
	if (const auto media = item->media()) {
		if (const auto photo = dynamic_cast<Data::MediaPhoto*>(media)) {
			if (const auto data = photo->photo()) {
				result.media.kind = MediaKind::Photo;
				result.media.id = std::to_string(
					static_cast<std::int64_t>(data->id));
				photos.push_back({
					.msgId = result.id,
					.inlineBytes = data->inlineThumbnailBytes(),
					.width = data->width(),
					.height = data->height(),
				});
			}
		} else if (const auto file = dynamic_cast<Data::MediaFile*>(media)) {
			if (const auto document = file->document()) {
				const auto kind = document->sticker()
					? MediaKind::Sticker
					: document->isVideoMessage()
					? MediaKind::Round
					: (document->isAnimation() || document->isGifv())
					? MediaKind::Gif
					: document->isVoiceMessage()
					? MediaKind::Voice
					: document->isVideoFile()
					? MediaKind::Video
					: (document->isAudioFile() || document->isSong())
					? MediaKind::Audio
					: MediaKind::Document;
				result.media.kind = kind;
				result.media.id = std::to_string(
					static_cast<std::int64_t>(document->id));
			}
		} else if (const auto contact
				= dynamic_cast<Data::MediaContact*>(media)) {
			if (const auto shared = contact->sharedContact()) {
				result.media.kind = MediaKind::Contact;
				result.media.userId = std::int64_t(shared->userId.bare);
				result.media.phone = U32(shared->phoneNumber);
			}
		} else if (media->invoice() && media->invoice()->isPaidMedia) {
			result.media.kind = MediaKind::Paid;
		}
	}

	result.album = std::int64_t(item->groupId().value);

	if (const auto forwarded = item->Get<HistoryMessageForwarded>()) {
		auto forward = Forward();
		forward.date = std::int64_t(forwarded->originalDate);
		if (const auto sender = forwarded->originalSender) {
			forward.fromId = BotApiId(sender);
			forward.username = U32(sender->username());
			if (sender->isChannel()) {
				forward.fromKind = ForwardKind::Channel;
				forward.postId = std::int64_t(forwarded->originalId.bare);
			} else if (sender->isUser()) {
				forward.fromKind = ForwardKind::User;
			} else {
				forward.fromKind = ForwardKind::Hidden;
				forward.fromId = 0;
				forward.fromName = U32(sender->name());
			}
		} else {
			forward.fromKind = ForwardKind::Hidden;
			if (const auto &hidden = forwarded->originalHiddenSenderInfo) {
				forward.fromName = U32(hidden->name);
			}
		}
		result.forward = std::move(forward);
	}

	if (const auto markup = item->inlineReplyMarkup()) {
		using ButtonType = HistoryMessageMarkupButton::Type;
		for (const auto &row : markup->data.rows) {
			for (const auto &button : row) {
				if (button.type == ButtonType::Url) {
					result.buttons.push_back(
						U32(QString::fromUtf8(button.data)));
				}
			}
		}
	}

	if (const auto reply = item->Get<HistoryMessageReply>()) {
		const auto replyId = reply->messageId();
		// A plain message in a forum topic carries the topic root as its
		// reply id; that is not a reply.
		const auto topicRoot = reply->topicPost()
			&& (replyId == item->topicRootId());
		if (!reply->external() && replyId && !topicRoot) {
			result.replyTo = std::int64_t(replyId.bare);
			const auto to = reply->resolvedMessage.get();
			result.replyToMe = to && to->out();
		}
	}
	result.mentionsMe = item->mentionsMe();
	result.viaBot = (item->viaBot() != nullptr);
	result.pinned = item->isPinned();
	result.autoForward = item->isDiscussionPost();
	result.service = item->isService();
	result.sponsored = item->isSponsored();
	result.scheduled = item->isScheduled();

	if (result.pinned) {
		rememberPinned(chat, text.text);
	}
	return result;
}

void Manager::pollAdmins() {
	auto any = false;
	for (const auto &[uniqueId, state] : _sessions) {
		for (const auto &[peerId, chat] : state->chats) {
			const auto &data = chat->context.data;
			if (data
				&& !data->adminsKnown
				&& chat->adminsPolls < kAdminsPollLimit) {
				++chat->adminsPolls;
				chat->contextDirty = true;
				chat->decideDirty = true;
				any = true;
			}
		}
	}
	if (any) {
		scheduleProcess();
		scheduleDecide();
	}
}

void Manager::scheduleDecide() {
	if (!_decideTimer.isActive()) {
		_decideTimer.callOnce(kDecideDelay);
	}
}

void Manager::runDecides() {
	if (!_worker) {
		return;
	}
	const auto now = std::int64_t(base::unixtime::now());
	const auto settings = CurrentSettings();
	const auto generation = _generation;
	for (const auto &[uniqueId, state] : _sessions) {
		for (const auto &[peerId, chat] : state->chats) {
			if (!chat->decideDirty || chat->decideInFlight) {
				continue;
			}
			chat->decideDirty = false;
			refreshContext(*state, *chat);
			auto request = DecideRequest();
			request.userId = userIdFor(*state);
			request.chatId = chat->chatId;
			request.context = chat->context;
			request.settings = settings;
			request.now = now;
			request.ids.reserve(chat->observed.size());
			for (const auto &[id, observed] : chat->observed) {
				if (!observed.filtered) {
					request.ids.push_back(std::int64_t(id.bare));
				}
			}
			if (request.ids.empty()) {
				continue;
			}
			chat->decideInFlight = true;
			const auto unique = uniqueId;
			const auto peer = peerId;
			const auto allowDefer = !base::take(chat->noDeferOnce);
			_worker->with([=, request = std::move(request)](
					Worker &worker) {
				auto list = worker.decide(request);
				crl::on_main([=, list = std::move(list)]() mutable {
					Instance().applyDecisions(
						generation,
						unique,
						peer,
						allowDefer,
						std::move(list));
				});
			});
		}
	}
}

void Manager::applyDecisions(
		std::uint64_t generation,
		quint64 uniqueId,
		PeerId peerId,
		bool allowDefer,
		DecisionList &&list) {
	if (generation != _generation) {
		return;
	}
	const auto i = _sessions.find(uniqueId);
	if (i == _sessions.end()) {
		return;
	}
	const auto state = i->second.get();
	const auto j = state->chats.find(peerId);
	if (j == state->chats.end()) {
		return;
	}
	const auto chat = j->second.get();
	chat->decideInFlight = false;

	auto changed = std::vector<MsgId>();
	for (const auto &[id, decision] : list) {
		const auto msgId = MsgId(id);
		if (!chat->observed.contains(msgId)) {
			// Freed or deleted while the queue was deciding.
			continue;
		}
		const auto key = FullMsgId(peerId, msgId);
		const auto k = state->decisions.find(key);
		if (k == state->decisions.end()) {
			state->decisions.emplace(key, Cached{ .decision = decision });
			if (decision.fold) {
				changed.push_back(msgId);
			}
			continue;
		}
		auto &entry = k->second;
		if (entry.decision == decision) {
			continue;
		}
		const auto wasFold = entry.decision.fold;
		const auto settled = Settled(entry.decision.state);
		entry.decision = decision;
		if (!decision.fold) {
			entry.deferred = false;
		} else if (!wasFold && settled && allowDefer) {
			// D: a message already judged "keep" does not vanish under the
			// reader when its cluster triggers; it folds the next time the
			// chat is shown (ChatShown()).
			entry.deferred = true;
		}
		// A folded message whose decision changed (its fold row shows the
		// occurrence and the reply flag), or one that (un)folds.
		if (wasFold || decision.fold) {
			changed.push_back(msgId);
		}
	}
	if (!changed.empty()) {
		if (const auto history = state->session->data().historyLoaded(
				peerId)) {
			_decisionsChanges.fire(DecisionsChange{
				.history = history,
				.ids = std::move(changed),
			});
		}
	}
	if (chat->decideDirty) {
		scheduleDecide();
	}
}

void Manager::onEdited(SessionState &state, not_null<HistoryItem*> item) {
	const auto i = state.chats.find(item->history()->peer->id);
	if (i == state.chats.end()) {
		return;
	}
	const auto chat = i->second.get();
	const auto j = chat->observed.find(item->id);
	if (j != chat->observed.end() && !(j->second.stamp == ComputeStamp(item))) {
		chat->pending.emplace(item->id);
		scheduleProcess();
	}
}

void Manager::onDestroying(
		SessionState &state,
		const std::vector<not_null<HistoryItem*>> &items) {
	auto touched = false;
	for (const auto &item : items) {
		const auto peerId = item->history()->peer->id;
		const auto i = state.chats.find(peerId);
		if (i == state.chats.end()
			|| !item->isRegular()
			|| !IsServerMsgId(item->id)) {
			continue;
		}
		const auto chat = i->second.get();
		chat->removed.push_back(std::int64_t(item->id.bare));
		chat->observed.remove(item->id);
		chat->pending.remove(item->id);
		state.decisions.erase(FullMsgId(peerId, item->id));
		state.expanded.remove(FullMsgId(peerId, item->id));
		touched = true;
	}
	if (touched) {
		scheduleProcess();
	}
}

void Manager::onFreed(
		SessionState &state,
		not_null<const HistoryItem*> item) {
	const auto peerId = item->history()->peer->id;
	const auto i = state.chats.find(peerId);
	if (i == state.chats.end()) {
		return;
	}
	const auto chat = i->second.get();
	chat->observed.remove(item->id);
	chat->pending.remove(item->id);
	state.decisions.erase(FullMsgId(peerId, item->id));
	state.expanded.remove(FullMsgId(peerId, item->id));
}

void Manager::onPeerUpdated(
		SessionState &state,
		not_null<PeerData*> peer,
		Data::PeerUpdate::Flags flags) {
	using Flag = Data::PeerUpdate::Flag;
	const auto i = state.chats.find(peer->id);
	if (i != state.chats.end()) {
		i->second->contextDirty = true;
		scheduleProcess();
	} else if (peer->isUser()
		&& (flags & (Flag::Username | Flag::Usernames))) {
		// An admin's or a bot's username is a known member's handle (and
		// mine is selfUsername); the contexts are compared before anything
		// is observed again, so a stranger's rename costs nothing.
		for (const auto &[peerId, chat] : state.chats) {
			chat->contextDirty = true;
		}
		if (!state.chats.empty()) {
			scheduleProcess();
		}
	}
}

void Manager::onPinnedChanged(
		SessionState &state,
		not_null<History*> history) {
	const auto i = state.chats.find(history->peer->id);
	if (i == state.chats.end()) {
		return;
	}
	const auto chat = i->second.get();
	const auto owner = &history->owner();
	for (const auto &[id, observed] : chat->observed) {
		const auto item = owner->message(history->peer->id, id);
		if (item && item->isPinned() != observed.stamp.pinned) {
			chat->pending.emplace(id);
		}
	}
	if (!chat->pending.empty()) {
		scheduleProcess();
	}
}

std::optional<Key> Manager::loadKey(bool create) {
	auto file = QFile(_keyPath);
	if (file.open(QIODevice::ReadOnly)) {
		const auto stored = QString::fromUtf8(file.read(4096)).trimmed();
		file.close();
		if (const auto key = KeyFromHex(OpenApiKey(stored))) {
			return key;
		}
	}
	if (!create) {
		return std::nullopt;
	}
	const auto key = RandomKey();
	const auto hex = KeyToHex(key);
	const auto stored = SealApiKey(hex).value_or(hex);
	QDir().mkpath(cWorkingDir() + u"tdata"_q);
	auto save = QSaveFile(_keyPath);
	if (!save.open(QIODevice::WriteOnly)) {
		LOG(("Lumina AdShield Error: could not write the key file."));
		return std::nullopt;
	}
	const auto bytes = stored.toUtf8();
	if (save.write(bytes) != bytes.size() || !save.commit()) {
		LOG(("Lumina AdShield Error: could not write the key file."));
		return std::nullopt;
	}
	return key;
}

bool Manager::ensureWorker(bool createKey) {
	if (_worker && (_persistentKey || !createKey)) {
		return true;
	}
	auto key = loadKey(createKey);
	if (!key && createKey) {
		return false;
	}
	_persistentKey = key.has_value();
	if (!key) {
		// Only cleaning up: without the key every file there is an orphan,
		// and a throw-away key makes PurgeOrphans() treat them as such.
		key = RandomKey();
	}
	// Every queue object runs on a queue of its own: a clean-up queue that
	// is replaced by the real one is drained first, so that its removals
	// can never race the new queue's writes.
	retireWorker();
	// crl constructs the object on its queue from rvalues only.
	_worker = std::make_unique<crl::object_on_queue<Worker>>(
		QString(_root),
		Key(*key));
	++_generation;
	resetInFlight();
	return true;
}

void Manager::destroyWorkerIfIdle() {
	if (!_worker || _anyActive || !_sessions.empty()) {
		return;
	}
	retireWorker();
}

void Manager::retireWorker() {
	if (!_worker) {
		return;
	}
	// Waits for what is already posted (forgets, clears: a few file
	// removals). A queue created later runs on a different crl::queue, and
	// must not start while this one may still be removing files.
	_worker->with_sync([](Worker &) {});
	_worker = nullptr;
	++_generation;
	resetInFlight();
	_flushTimer.cancel();
}

void Manager::resetInFlight() {
	for (const auto &[uniqueId, state] : _sessions) {
		for (const auto &[peerId, chat] : state->chats) {
			if (chat->decideInFlight) {
				chat->decideInFlight = false;
				chat->decideDirty = true;
			}
		}
	}
}

void Manager::forgetData(quint64 uniqueId) {
	if (!_worker && !QDir(_root).exists()) {
		return;
	}
	if (!ensureWorker(false)) {
		return;
	}
	const auto userId = std::int64_t(uniqueId);
	_worker->with([=](Worker &worker) {
		worker.forget(userId);
	});
}

void Manager::scheduleFlush() {
	if (!_flushTimer.isActive()) {
		_flushTimer.callOnce(Worker::kFlushDelay + crl::time(100));
	}
}

void Manager::flushTimerDone() {
	if (!_worker) {
		return;
	}
	const auto generation = _generation;
	_worker->with([=](Worker &worker) {
		if (worker.flushDue(crl::now())) {
			crl::on_main([=] {
				auto &manager = Instance();
				if (manager._generation == generation) {
					manager.scheduleFlush();
				}
			});
		}
	});
}

void Manager::sessionLoggingOut(not_null<Main::Session*> session) {
	const auto uniqueId = session->uniqueId();
	dropSession(uniqueId);
	_wasActive.remove(uniqueId);
	_inactiveSessions.remove(uniqueId);
	forgetData(uniqueId);
	destroyWorkerIfIdle();
}

void Manager::flushNow() {
	if (_worker) {
		_worker->with_sync([](Worker &worker) {
			worker.flushAll();
		});
	}
}

void Manager::panicWipe() {
	auto sessions = base::take(_sessions);
	for (const auto &[uniqueId, state] : sessions) {
		for (const auto &[peerId, chat] : state->chats) {
			fireWholeChat(*state, peerId);
		}
	}
	sessions.clear();
	_wasActive.clear();
	_inactiveSessions.clear();
	_flushTimer.cancel();
	_decideTimer.cancel();
	if (_worker) {
		_worker->with_sync([](Worker &worker) {
			worker.wipe();
		});
		_worker = nullptr;
		++_generation;
	}
	if (!_root.isEmpty()) {
		QDir(_root).removeRecursively();
	}
	if (!_keyPath.isEmpty()) {
		QFile::remove(_keyPath);
	}
	refreshFlags();
	LOG(("Lumina AdShield: panic wipe done."));
}

void Manager::clearLocalData() {
	// Every message is undecided again: no folds until it is observed anew.
	for (const auto &[uniqueId, state] : _sessions) {
		for (const auto &[peerId, chat] : state->chats) {
			chat->observed.clear();
			chat->pending.clear();
			chat->removed.clear();
			chat->decideDirty = false;
			fireWholeChat(*state, peerId);
		}
		state->decisions.clear();
		state->expanded.clear();
	}
	if (_worker || QDir(_root).exists()) {
		if (ensureWorker(false)) {
			_worker->with([](Worker &worker) {
				worker.clearAll();
			});
		}
	}
	// Fires ActivationChanges(): accounts that only had per-group "on"
	// become inactive and the queue may go away.
	ClearChatModes();
	destroyWorkerIfIdle();
}

ChatMode Manager::chatMode(not_null<PeerData*> peer) const {
	return AdShield::ChatModeFor(
		peer->session().uniqueId(),
		quint64(peer->id.value));
}

void Manager::setChatMode(not_null<PeerData*> peer, ChatMode mode) {
	if (!ChatEligible(peer)) {
		return;
	}
	AdShield::SetChatMode(
		peer->session().uniqueId(),
		quint64(peer->id.value),
		mode);
	// ActivationChanges() did the rest; pick up what is already on screen
	// in this group.
	if (const auto state = sessionState(&peer->session(), true)) {
		if (chatEnabled(peer)) {
			scanLoaded(*state, peer);
		}
	}
}

bool Manager::chatEnabled(not_null<PeerData*> peer) const {
	if (!ChatEligible(peer)) {
		return false;
	}
	switch (chatMode(peer)) {
	case ChatMode::On: return true;
	case ChatMode::Off: return false;
	case ChatMode::Default: return GlobalEnabled();
	}
	return false;
}

const Manager::SessionState *Manager::findState(
		not_null<const HistoryItem*> item) const {
	const auto i = _sessions.find(item->history()->session().uniqueId());
	return (i != _sessions.end()) ? i->second.get() : nullptr;
}

const Decision *Manager::cached(not_null<const HistoryItem*> item) const {
	const auto state = findState(item);
	if (!state) {
		return nullptr;
	}
	const auto i = state->decisions.find(item->fullId());
	return (i != state->decisions.end()) ? &i->second.decision : nullptr;
}

bool Manager::shouldFold(not_null<const HistoryItem*> item) const {
	if (_trial) {
		return false;
	}
	const auto state = findState(item);
	if (!state) {
		return false;
	}
	const auto key = item->fullId();
	const auto i = state->decisions.find(key);
	return (i != state->decisions.end())
		&& i->second.decision.fold
		&& !i->second.deferred
		&& !state->expanded.contains(key);
}

bool Manager::shouldMarkTrial(not_null<const HistoryItem*> item) const {
	if (!_trial) {
		return false;
	}
	const auto decision = cached(item);
	return decision && decision->fold;
}

bool Manager::expanded(not_null<const HistoryItem*> item) const {
	const auto state = findState(item);
	return state && state->expanded.contains(item->fullId());
}

void Manager::setExpanded(
		not_null<History*> history,
		const std::vector<MsgId> &ids,
		bool expanded) {
	const auto i = _sessions.find(history->session().uniqueId());
	if (i == _sessions.end() || ids.empty()) {
		return;
	}
	const auto state = i->second.get();
	auto changed = std::vector<MsgId>();
	for (const auto &id : ids) {
		const auto key = FullMsgId(history->peer->id, id);
		const auto done = expanded
			? state->expanded.emplace(key).second
			: state->expanded.remove(key);
		if (done) {
			changed.push_back(id);
		}
	}
	if (!changed.empty()) {
		_decisionsChanges.fire(DecisionsChange{
			.history = history,
			.ids = std::move(changed),
		});
	}
}

void Manager::chatShown(not_null<History*> history) {
	const auto i = _sessions.find(history->session().uniqueId());
	if (i == _sessions.end()) {
		return;
	}
	const auto state = i->second.get();
	const auto peerId = history->peer->id;
	auto changed = std::vector<MsgId>();
	for (auto &[key, entry] : state->decisions) {
		if (key.peer == peerId && entry.deferred) {
			entry.deferred = false;
			changed.push_back(key.msg);
		}
	}
	if (!changed.empty()) {
		_decisionsChanges.fire(DecisionsChange{
			.history = history,
			.ids = std::move(changed),
		});
	}
}

void Manager::markNotAd(not_null<HistoryItem*> item) {
	const auto history = item->history();
	const auto state = sessionState(&history->session(), false);
	if (!state || !_worker || !IsServerMsgId(item->id)) {
		return;
	}
	const auto chat = chatState(*state, history->peer, false);
	if (!chat) {
		return;
	}
	refreshContext(*state, *chat);
	const auto userId = userIdFor(*state);
	const auto chatId = chat->chatId;
	const auto context = chat->context;
	const auto msgId = std::int64_t(item->id.bare);
	const auto now = std::int64_t(base::unixtime::now());
	_worker->with([=](Worker &worker) {
		const auto result = worker.markNotAd(
			userId,
			chatId,
			context,
			msgId,
			now);
		DEBUG_LOG(("Lumina AdShield: not-an-ad mark %1."
			).arg(result == MarkResult::Recorded ? "recorded" : "skipped"));
	});
	chat->decideDirty = true;
	scheduleDecide();
	scheduleFlush();
}

void Manager::requestFoldedCount(
		not_null<PeerData*> peer,
		Fn<void(int)> done) {
	const auto state = sessionState(&peer->session(), true);
	const auto chat = state ? chatState(*state, peer, true) : nullptr;
	if (!chat || !_worker) {
		done(0);
		return;
	}
	refreshContext(*state, *chat);
	const auto userId = userIdFor(*state);
	const auto chatId = chat->chatId;
	const auto context = chat->context;
	const auto settings = CurrentSettings();
	const auto now = std::int64_t(base::unixtime::now());
	const auto generation = _generation;
	_worker->with([=](Worker &worker) {
		const auto count = worker.countFolded(
			userId,
			chatId,
			context,
			settings,
			now);
		crl::on_main([=] {
			if (Instance()._generation == generation) {
				done(count);
			}
		});
	});
}

} // namespace

void Start() {
	Manager::Instance().start();
}

void NoteViewCreated(
		not_null<HistoryItem*> item,
		not_null<HistoryView::ElementDelegate*> delegate) {
	if (!Active) {
		return;
	}
	using Context = HistoryView::Context;
	const auto context = delegate->elementContext();
	if (context != Context::History && context != Context::Replies) {
		return;
	}
	Manager::Instance().note(item);
}

void SessionLoggingOut(not_null<Main::Session*> session) {
	auto &manager = Manager::Instance();
	manager.start();
	manager.sessionLoggingOut(session);
}

void FlushNow() {
	if (Constructed) {
		Manager::Instance().flushNow();
	}
}

void PanicWipe() {
	auto &manager = Manager::Instance();
	manager.start();
	manager.panicWipe();
}

void ClearLocalData() {
	auto &manager = Manager::Instance();
	manager.start();
	manager.clearLocalData();
}

bool Observing() {
	return Active;
}

bool ChatEligible(not_null<PeerData*> peer) {
	return ChatTypeFor(peer) != ChatType::Other;
}

ChatMode ChatModeFor(not_null<PeerData*> peer) {
	return Manager::Instance().chatMode(peer);
}

void SetChatMode(not_null<PeerData*> peer, ChatMode mode) {
	auto &manager = Manager::Instance();
	manager.start();
	manager.setChatMode(peer, mode);
}

bool ChatEnabled(not_null<PeerData*> peer) {
	return Manager::Instance().chatEnabled(peer);
}

const Decision *CachedDecision(not_null<const HistoryItem*> item) {
	return Active ? Manager::Instance().cached(item) : nullptr;
}

bool ShouldFold(not_null<const HistoryItem*> item) {
	return Active && Manager::Instance().shouldFold(item);
}

bool ShouldMarkTrial(not_null<const HistoryItem*> item) {
	return Active && Manager::Instance().shouldMarkTrial(item);
}

bool Expanded(not_null<const HistoryItem*> item) {
	return Active && Manager::Instance().expanded(item);
}

void SetExpanded(
		not_null<History*> history,
		const std::vector<MsgId> &ids,
		bool expanded) {
	Manager::Instance().setExpanded(history, ids, expanded);
}

void ChatShown(not_null<History*> history) {
	if (Active) {
		Manager::Instance().chatShown(history);
	}
}

void MarkNotAd(not_null<HistoryItem*> item) {
	Manager::Instance().markNotAd(item);
}

void RequestFoldedCount(not_null<PeerData*> peer, Fn<void(int)> done) {
	auto &manager = Manager::Instance();
	manager.start();
	manager.requestFoldedCount(peer, std::move(done));
}

rpl::producer<DecisionsChange> DecisionsChanges() {
	return Manager::Instance().decisionsChanges();
}

rpl::producer<> DisplayChanges() {
	return Manager::Instance().displayChanges();
}

} // namespace Lumina::AdShield
