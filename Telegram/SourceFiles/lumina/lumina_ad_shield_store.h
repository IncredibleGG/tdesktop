/*
This file is part of LuminaGram,
a fork of Telegram Desktop.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "lumina/lumina_ad_shield.h"

#include <map>
#include <memory>
#include <unordered_map>

namespace Lumina::AdShield {

enum class Sensitivity : std::uint8_t {
	Loose,
	Standard,
	Aggressive,
};

struct Settings {
	Sensitivity sensitivity = Sensitivity::Standard;
	bool exemptContacts = true;
	bool exemptBots = true;
	bool exemptReplies = true;
	bool dryRun = false;
};

enum class State : std::uint8_t {
	Ineligible,
	Untracked,
	Pending,
	Endorsed,
	NotAd,
	NotTriggered,
	Original,
	Exempt,
	Fold,
};

enum class DecisionClass : std::uint8_t {
	Unset,
	Strong,
	Weak,
	Plain,
	Impersonation,
};

enum class DecisionRule : std::uint8_t {
	Unset,
	Strong,
	Weak,
	MediaSameSender,
	FwdSameSender,
	Plain,
};

struct Decision {
	State state = State::NotTriggered;
	bool fold = false;
	DecisionClass cls = DecisionClass::Unset;
	DecisionRule rule = DecisionRule::Unset;
	int occurrence = 0;
	int groupSize = 0;
	bool replyToMe = false;

	friend inline bool operator==(const Decision &a, const Decision &b) {
		return (a.state == b.state)
			&& (a.fold == b.fold)
			&& (a.cls == b.cls)
			&& (a.rule == b.rule)
			&& (a.occurrence == b.occurrence)
			&& (a.groupSize == b.groupSize)
			&& (a.replyToMe == b.replyToMe);
	}
	friend inline bool operator!=(const Decision &a, const Decision &b) {
		return !(a == b);
	}
};

enum class ObserveResult : std::uint8_t {
	Observed,
	Ignored,
	Refused,
};

enum class MarkResult : std::uint8_t {
	Recorded,
	NotFound,
	Refused,
};

inline constexpr std::uint8_t kRecordEndorse = 1;
inline constexpr std::uint8_t kRecordNotAd = 2;

struct StoredRow {
	std::uint32_t rid = 0;
	std::uint32_t msgId = 0;
	std::int64_t album = 0;
	std::int64_t date = 0;
	std::int64_t seen = 0;
	std::uint64_t sender = 0;
	std::uint32_t flags = 0;
	std::vector<KeyRef> keys;
	std::vector<std::uint64_t> links;
	std::vector<std::uint64_t> pending;
};

struct StoredRecord {
	std::uint8_t kind = kRecordEndorse;
	std::int64_t stamp = 0;
	std::uint64_t msgKey = 0;
	std::vector<KeyRef> keys;
	std::vector<std::uint64_t> links;
	std::vector<KeyRef> keySet;
};

struct HandleInfo {
	std::int64_t first = 0;
	std::int64_t last = 0;
	std::int64_t seen = 0;
};

[[nodiscard]] std::string_view StateName(State state);
[[nodiscard]] std::string_view DecisionClassName(DecisionClass cls);
[[nodiscard]] std::string_view DecisionRuleName(DecisionRule rule);

// One chat's rows, endorsement / "not an ad" records and known-member
// handles (SPEC 6, 8). Single-thread confined like everything in AdShield.
// After the owning Account::forgetAll() every write is refused, even through
// a Store pointer that is still held somewhere.
class Store {
public:
	Store(
		std::int64_t chatId,
		const Key &key,
		std::shared_ptr<const bool> forgotten = nullptr);

	[[nodiscard]] ObserveResult observe(
		const MessageInput &msg,
		const ChatContext &context,
		std::int64_t now);
	[[nodiscard]] bool remove(
		const std::vector<std::int64_t> &msgIds,
		std::int64_t now);
	[[nodiscard]] MarkResult markNotAd(
		std::int64_t msgId,
		const ChatContext &context,
		std::int64_t now);
	[[nodiscard]] Decision decide(
		std::int64_t msgId,
		const ChatContext &context,
		const Settings &settings,
		std::int64_t now);
	void compact(std::int64_t now);

	[[nodiscard]] std::vector<unsigned char> serialize() const;
	[[nodiscard]] static std::unique_ptr<Store> Deserialize(
		const unsigned char *data,
		std::size_t size,
		std::int64_t chatId,
		const Key &key,
		std::shared_ptr<const bool> forgotten = nullptr);

	[[nodiscard]] std::int64_t chatId() const {
		return _chatId;
	}
	[[nodiscard]] std::uint64_t version() const {
		return _version;
	}
	[[nodiscard]] bool writable() const;
	[[nodiscard]] bool tracks(std::int64_t msgId) const;
	[[nodiscard]] const std::map<std::uint32_t, StoredRow> &rows() const {
		return _rows;
	}
	[[nodiscard]] const std::vector<StoredRecord> &records() const {
		return _records;
	}
	[[nodiscard]] const std::map<std::uint64_t, HandleInfo> &handles() const {
		return _handles;
	}

private:
	struct Order {
		std::int64_t date = 0;
		int live = 0;
		std::uint32_t minMsgId = 0;
		std::uint64_t minKey = 0;
	};
	struct Unit {
		bool album = false;
		std::int64_t id = 0;
		std::int64_t date = 0;
		std::uint64_t sender = 0;
		std::uint32_t flags = 0;
		std::vector<KeyRef> keys;
		std::vector<std::uint64_t> links;
		bool live = false;
		Order order;
	};
	struct KeyRefHash {
		std::size_t operator()(const KeyRef &key) const {
			return std::size_t(key.hash ^ (std::uint64_t(key.kind) << 56));
		}
	};
	struct Units {
		bool valid = false;
		std::uint64_t generation = 0;
		std::vector<std::uint64_t> known;
		std::int64_t validFrom = 0;
		std::int64_t validTill = 0;
		std::vector<Unit> list;
		std::map<std::pair<bool, std::int64_t>, std::size_t> byId;
		std::unordered_map<KeyRef, std::vector<std::size_t>, KeyRefHash> index;
	};

	[[nodiscard]] static bool OrderLess(const Order &a, const Order &b);
	[[nodiscard]] static bool UnitLess(const Unit &a, const Unit &b);

	void touch();
	[[nodiscard]] std::uint32_t allocateRid();
	[[nodiscard]] const Units &units(
		const ChatContext &context,
		std::int64_t now);
	[[nodiscard]] std::optional<std::size_t> unitOf(
		const Units &all,
		std::int64_t msgId) const;
	[[nodiscard]] std::vector<std::size_t> group(
		const Units &all,
		std::size_t m) const;
	void upsertEndorse(
		std::int64_t stamp,
		std::uint64_t msgKey,
		const std::vector<KeyRef> &keys,
		const std::vector<std::uint64_t> &links);
	void addNotAd(
		std::int64_t stamp,
		std::vector<KeyRef> keys,
		std::vector<std::uint64_t> links);
	void evictRecords(std::uint8_t kind);
	void enforceCaps();

	std::int64_t _chatId = 0;
	Key _key = {};
	std::shared_ptr<const bool> _forgotten;
	std::map<std::uint32_t, StoredRow> _rows;
	std::unordered_map<std::uint32_t, std::uint32_t> _byMsg;
	std::uint32_t _nextRid = 1;
	std::vector<StoredRecord> _records;
	std::map<std::uint64_t, HandleInfo> _handles;
	std::uint64_t _version = 0;
	std::uint64_t _generation = 0;
	Units _units;

};

} // namespace Lumina::AdShield
