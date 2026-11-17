/*
This file is part of LuminaGram,
a fork of Telegram Desktop.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "lumina/lumina_ad_shield_store.h"

#include "lumina/ad_shield_tables.h"

#include <algorithm>
#include <iterator>
#include <limits>
#include <unordered_set>

namespace Lumina::AdShield {
namespace {

constexpr auto kHeaderSize = std::size_t(56);
constexpr auto kInt64Max = std::numeric_limits<std::int64_t>::max();
constexpr auto kInt64Min = std::numeric_limits<std::int64_t>::min();

struct Rule {
	std::int64_t window = 0;
	std::size_t hits = 0;
	std::size_t senders = 0;
};

struct RuleSet {
	std::vector<Rule> strong;
	std::vector<Rule> weak;
	bool hasSame = false;
	Rule same;
	bool hasPlain = false;
	Rule plain;
};

[[nodiscard]] const RuleSet &RulesFor(Sensitivity sensitivity) {
	static const auto kLoose = RuleSet{
		{ { 72 * kHour, 4, 1 }, { 72 * kHour, 2, 3 } },
		{},
		false,
		{},
		false,
		{},
	};
	static const auto kStandard = RuleSet{
		{ { 7 * kDay, 3, 1 }, { 7 * kDay, 2, 2 } },
		{ { 24 * kHour, 4, 3 } },
		true,
		{ 7 * kDay, 3, 1 },
		false,
		{},
	};
	static const auto kAggressive = RuleSet{
		{ { 7 * kDay, 2, 1 } },
		{ { 7 * kDay, 3, 2 } },
		true,
		{ 7 * kDay, 3, 1 },
		true,
		{ 7 * kDay, 3, 3 },
	};
	switch (sensitivity) {
	case Sensitivity::Loose: return kLoose;
	case Sensitivity::Standard: return kStandard;
	case Sensitivity::Aggressive: return kAggressive;
	}
	return kStandard;
}

// now - t > limit, exact for any int64 values (no signed overflow).
[[nodiscard]] bool Older(std::int64_t now, std::int64_t t, std::int64_t limit) {
	return (now > t)
		&& (std::uint64_t(now) - std::uint64_t(t) > std::uint64_t(limit));
}

[[nodiscard]] std::int64_t SaturatedAdd(std::int64_t a, std::int64_t b) {
	return (a > kInt64Max - b) ? kInt64Max : (a + b);
}

[[nodiscard]] bool InDateRange(std::int64_t value) {
	return (value >= 0) && (value <= kMaxDate);
}

[[nodiscard]] bool IsExact(KeyKind kind) {
	return kind != KeyKind::Para;
}

[[nodiscard]] bool Overlap(
		const std::vector<KeyRef> &a,
		const std::vector<KeyRef> &b) {
	auto paragraphs = 0;
	auto i = a.begin();
	auto j = b.begin();
	while (i != a.end() && j != b.end()) {
		if (*i < *j) {
			++i;
		} else if (*j < *i) {
			++j;
		} else {
			if (IsExact(i->kind)) {
				return true;
			} else if (++paragraphs >= 2) {
				return true;
			}
			++i;
			++j;
		}
	}
	return false;
}

[[nodiscard]] bool Subset(
		const std::vector<std::uint64_t> &what,
		const std::vector<std::uint64_t> &of) {
	return std::includes(of.begin(), of.end(), what.begin(), what.end());
}

[[nodiscard]] std::vector<KeyRef> SortedUnique(std::vector<KeyRef> keys) {
	std::sort(keys.begin(), keys.end());
	keys.erase(std::unique(keys.begin(), keys.end()), keys.end());
	return keys;
}

[[nodiscard]] std::vector<KeyRef> OrderedRecordKeys(
		const std::vector<KeyRef> &sequence) {
	auto unique = std::vector<KeyRef>();
	auto seen = std::vector<KeyRef>();
	for (const auto &key : sequence) {
		const auto i = std::lower_bound(seen.begin(), seen.end(), key);
		if (i != seen.end() && *i == key) {
			continue;
		}
		seen.insert(i, key);
		unique.push_back(key);
	}
	auto result = std::vector<KeyRef>();
	for (const auto &key : unique) {
		if (IsExact(key.kind) && result.size() < kMaxRecordKeys) {
			result.push_back(key);
		}
	}
	for (const auto &key : unique) {
		if (!IsExact(key.kind) && result.size() < kMaxRecordKeys) {
			result.push_back(key);
		}
	}
	return result;
}

[[nodiscard]] std::vector<std::uint64_t> FirstUnique(
		const std::vector<std::uint64_t> &sequence,
		std::size_t cap) {
	auto result = std::vector<std::uint64_t>();
	auto seen = std::unordered_set<std::uint64_t>();
	for (const auto value : sequence) {
		if (result.size() == cap) {
			break;
		}
		if (seen.insert(value).second) {
			result.push_back(value);
		}
	}
	return result;
}

class Writer final {
public:
	explicit Writer(std::vector<unsigned char> &out) : _out(out) {
	}

	void u8(std::uint8_t value) {
		_out.push_back(value);
	}
	void u16(std::uint16_t value) {
		put(value, 2);
	}
	void u32(std::uint32_t value) {
		put(value, 4);
	}
	void u64(std::uint64_t value) {
		put(value, 8);
	}
	void i64(std::int64_t value) {
		put(std::uint64_t(value), 8);
	}
	void bytes(const char *data, std::size_t size) {
		for (std::size_t i = 0; i != size; ++i) {
			_out.push_back(static_cast<unsigned char>(data[i]));
		}
	}

private:
	void put(std::uint64_t value, int size) {
		for (auto i = 0; i != size; ++i) {
			_out.push_back(static_cast<unsigned char>(value >> (8 * i)));
		}
	}

	std::vector<unsigned char> &_out;

};

class Reader final {
public:
	Reader(const unsigned char *data, std::size_t size)
	: _data(data)
	, _size(size) {
	}

	[[nodiscard]] bool u8(std::uint8_t &value) {
		auto raw = std::uint64_t();
		if (!get(raw, 1)) {
			return false;
		}
		value = std::uint8_t(raw);
		return true;
	}
	[[nodiscard]] bool u16(std::uint16_t &value) {
		auto raw = std::uint64_t();
		if (!get(raw, 2)) {
			return false;
		}
		value = std::uint16_t(raw);
		return true;
	}
	[[nodiscard]] bool u32(std::uint32_t &value) {
		auto raw = std::uint64_t();
		if (!get(raw, 4)) {
			return false;
		}
		value = std::uint32_t(raw);
		return true;
	}
	[[nodiscard]] bool u64(std::uint64_t &value) {
		return get(value, 8);
	}
	[[nodiscard]] bool i64(std::int64_t &value) {
		auto raw = std::uint64_t();
		if (!get(raw, 8)) {
			return false;
		}
		value = std::int64_t(raw);
		return true;
	}
	[[nodiscard]] std::size_t offset() const {
		return _offset;
	}

private:
	[[nodiscard]] bool get(std::uint64_t &value, std::size_t size) {
		if (_size - _offset < size) {
			return false;
		}
		value = 0;
		for (std::size_t i = 0; i != size; ++i) {
			value |= std::uint64_t(_data[_offset + i]) << (8 * i);
		}
		_offset += size;
		return true;
	}

	const unsigned char *_data = nullptr;
	std::size_t _size = 0;
	std::size_t _offset = 0;

};

[[nodiscard]] bool ReadKeys(
		Reader &reader,
		std::size_t cap,
		std::vector<KeyRef> &keys) {
	auto count = std::uint8_t();
	if (!reader.u8(count) || count > cap) {
		return false;
	}
	keys.reserve(count);
	for (auto i = 0; i != count; ++i) {
		auto kind = std::uint8_t();
		auto hash = std::uint64_t();
		if (!reader.u8(kind) || !reader.u64(hash) || kind < 1 || kind > 7) {
			return false;
		}
		keys.push_back({ KeyKind(kind), hash });
	}
	return true;
}

[[nodiscard]] bool ReadHashes(
		Reader &reader,
		std::size_t cap,
		std::vector<std::uint64_t> &hashes) {
	auto count = std::uint8_t();
	if (!reader.u8(count) || count > cap) {
		return false;
	}
	hashes.resize(count);
	for (auto &hash : hashes) {
		if (!reader.u64(hash)) {
			return false;
		}
	}
	return true;
}

void WriteKeys(Writer &writer, const std::vector<KeyRef> &keys) {
	writer.u8(std::uint8_t(keys.size()));
	for (const auto &key : keys) {
		writer.u8(std::uint8_t(key.kind));
		writer.u64(key.hash);
	}
}

void WriteHashes(Writer &writer, const std::vector<std::uint64_t> &hashes) {
	writer.u8(std::uint8_t(hashes.size()));
	for (const auto hash : hashes) {
		writer.u64(hash);
	}
}

[[nodiscard]] std::uint64_t KeyCheck(const Key &key) {
	return Hash(key, "check", std::string_view("LGAS"));
}

[[nodiscard]] std::uint64_t ChatTag(const Key &key, std::int64_t chatId) {
	return Hash(key, "chat", std::string_view(std::to_string(chatId)));
}

} // namespace

std::string_view StateName(State state) {
	switch (state) {
	case State::Ineligible: return "ineligible";
	case State::Untracked: return "untracked";
	case State::Pending: return "pending";
	case State::Endorsed: return "endorsed";
	case State::NotAd: return "not_ad";
	case State::NotTriggered: return "none";
	case State::Original: return "original";
	case State::Exempt: return "exempt";
	case State::Fold: return "fold";
	}
	return "";
}

std::string_view DecisionClassName(DecisionClass cls) {
	switch (cls) {
	case DecisionClass::Unset: return "";
	case DecisionClass::Strong: return "strong";
	case DecisionClass::Weak: return "weak";
	case DecisionClass::Plain: return "plain";
	case DecisionClass::Impersonation: return "impersonation";
	}
	return "";
}

std::string_view DecisionRuleName(DecisionRule rule) {
	switch (rule) {
	case DecisionRule::Unset: return "";
	case DecisionRule::Strong: return "strong";
	case DecisionRule::Weak: return "weak";
	case DecisionRule::MediaSameSender: return "media_same_sender";
	case DecisionRule::FwdSameSender: return "fwd_same_sender";
	case DecisionRule::Plain: return "plain";
	}
	return "";
}

Store::Store(
	std::int64_t chatId,
	const Key &key,
	std::shared_ptr<const bool> forgotten)
: _chatId(chatId)
, _key(key)
, _forgotten(std::move(forgotten)) {
}

bool Store::writable() const {
	return !_forgotten || !*_forgotten;
}

bool Store::tracks(std::int64_t msgId) const {
	return (msgId > 0)
		&& (msgId <= kMaxMsgId)
		&& _byMsg.count(std::uint32_t(msgId));
}

void Store::touch() {
	++_generation;
}

std::uint32_t Store::allocateRid() {
	if (_nextRid == std::numeric_limits<std::uint32_t>::max()) {
		auto renumbered = std::map<std::uint32_t, StoredRow>();
		auto rid = std::uint32_t(0);
		for (auto &[old, row] : _rows) {
			row.rid = ++rid;
			if (row.msgId != 0) {
				_byMsg[row.msgId] = rid;
			}
			renumbered.emplace(rid, std::move(row));
		}
		_rows = std::move(renumbered);
		_nextRid = rid + 1;
	}
	return _nextRid++;
}

bool Store::OrderLess(const Order &a, const Order &b) {
	if (a.date != b.date) {
		return a.date < b.date;
	} else if (a.live != b.live) {
		return a.live < b.live;
	} else if (a.minMsgId != b.minMsgId) {
		return a.minMsgId < b.minMsgId;
	}
	return a.minKey < b.minKey;
}

bool Store::UnitLess(const Unit &a, const Unit &b) {
	if (OrderLess(a.order, b.order)) {
		return true;
	} else if (OrderLess(b.order, a.order)) {
		return false;
	} else if (a.keys != b.keys) {
		return a.keys < b.keys;
	} else if (a.album != b.album) {
		return a.album < b.album;
	}
	return a.id < b.id;
}

const Store::Units &Store::units(
		const ChatContext &context,
		std::int64_t now) {
	auto known = std::vector<std::uint64_t>();
	known.reserve(context.memberHandles().size());
	for (const auto &name : context.memberHandles()) {
		if (!name.empty()) {
			known.push_back(HandleHash(_key, name));
		}
	}
	std::sort(known.begin(), known.end());
	known.erase(std::unique(known.begin(), known.end()), known.end());
	auto &u = _units;
	if (u.valid
		&& u.generation == _generation
		&& u.known == known
		&& now >= u.validFrom
		&& now <= u.validTill) {
		return u;
	}
	u.valid = true;
	u.generation = _generation;
	u.known = std::move(known);
	u.validFrom = kInt64Min;
	u.validTill = kInt64Max;
	u.list.clear();
	u.byId.clear();
	u.index.clear();

	auto currentDate = std::int64_t(0);
	const auto isKnown = IsKnownHandle([&](std::uint64_t h) {
		if (std::binary_search(u.known.begin(), u.known.end(), h)) {
			return true;
		}
		const auto i = _handles.find(h);
		return (i != _handles.end())
			&& (i->second.first <= currentDate - kKnownMemberMinAge);
	});
	for (const auto &[rid, row] : _rows) {
		const auto bound = SaturatedAdd(
			std::max(row.date, row.seen),
			kEvictAfter);
		if (now > bound) {
			u.validFrom = std::max(u.validFrom, bound + 1);
			continue;
		}
		u.validTill = std::min(u.validTill, bound);
		currentDate = row.date;
		auto resolved = Resolve(
			row.flags,
			row.keys,
			row.links,
			row.pending,
			isKnown);
		const auto id = (row.album != 0)
			? std::make_pair(true, row.album)
			: std::make_pair(false, std::int64_t(rid));
		const auto [i, inserted] = u.byId.emplace(id, u.list.size());
		if (inserted) {
			auto unit = Unit();
			unit.album = id.first;
			unit.id = id.second;
			unit.date = row.date;
			u.list.push_back(std::move(unit));
		}
		auto &unit = u.list[i->second];
		unit.date = std::min(unit.date, row.date);
		unit.flags |= resolved.flags;
		unit.keys.insert(
			unit.keys.end(),
			resolved.keys.begin(),
			resolved.keys.end());
		unit.links.insert(
			unit.links.end(),
			resolved.links.begin(),
			resolved.links.end());
		if (row.msgId != 0
			&& (!unit.live || row.msgId < unit.order.minMsgId)) {
			unit.live = true;
			unit.order.minMsgId = row.msgId;
			unit.sender = row.sender;
		}
	}
	for (auto &unit : u.list) {
		unit.keys = SortedUnique(std::move(unit.keys));
		std::sort(unit.links.begin(), unit.links.end());
		unit.links.erase(
			std::unique(unit.links.begin(), unit.links.end()),
			unit.links.end());
		if (unit.live) {
			unit.flags &= ~Flag::kAnon;
		}
		if (unit.flags & Flag::kStrongMask) {
			unit.flags &= ~Flag::kMediaOnly;
		}
		unit.order.date = unit.date;
		unit.order.live = unit.live ? 1 : 0;
		if (!unit.live) {
			unit.order.minMsgId = 0;
		}
		unit.order.minKey = 0;
		for (std::size_t k = 0; k != unit.keys.size(); ++k) {
			const auto hash = unit.keys[k].hash;
			if (k == 0 || hash < unit.order.minKey) {
				unit.order.minKey = hash;
			}
		}
	}
	for (std::size_t index = 0; index != u.list.size(); ++index) {
		for (const auto &key : u.list[index].keys) {
			u.index[key].push_back(index);
		}
	}
	return u;
}

std::optional<std::size_t> Store::unitOf(
		const Units &all,
		std::int64_t msgId) const {
	if (msgId <= 0 || msgId > kMaxMsgId) {
		return std::nullopt;
	}
	const auto i = _byMsg.find(std::uint32_t(msgId));
	if (i == _byMsg.end()) {
		return std::nullopt;
	}
	const auto r = _rows.find(i->second);
	if (r == _rows.end()) {
		return std::nullopt;
	}
	const auto &row = r->second;
	const auto id = (row.album != 0)
		? std::make_pair(true, row.album)
		: std::make_pair(false, std::int64_t(row.rid));
	const auto u = all.byId.find(id);
	if (u == all.byId.end()) {
		return std::nullopt;
	}
	return u->second;
}

std::vector<std::size_t> Store::group(
		const Units &all,
		std::size_t m) const {
	auto marked = std::vector<std::uint8_t>(all.list.size(), 0);
	auto paragraphs = std::unordered_map<std::size_t, int>();
	auto result = std::vector<std::size_t>{ m };
	marked[m] = 1;
	for (const auto &key : all.list[m].keys) {
		const auto i = all.index.find(key);
		if (i == all.index.end()) {
			continue;
		}
		for (const auto other : i->second) {
			if (IsExact(key.kind)) {
				if (!marked[other]) {
					marked[other] = 1;
					result.push_back(other);
				}
			} else if (++paragraphs[other] >= 2 && !marked[other]) {
				marked[other] = 1;
				result.push_back(other);
			}
		}
	}
	std::sort(result.begin(), result.end(), [&](
			std::size_t a,
			std::size_t b) {
		return UnitLess(all.list[a], all.list[b]);
	});
	return result;
}

namespace {

struct Candidate {
	DecisionRule rule = DecisionRule::Unset;
	Rule limits;
	std::vector<std::size_t> pool;
};

} // namespace

Decision Store::decide(
		std::int64_t msgId,
		const ChatContext &context,
		const Settings &settings,
		std::int64_t now) {
	auto result = Decision();
	if (!context.eligible()) {
		result.state = State::Ineligible;
		return result;
	}
	const auto &all = units(context, now);
	const auto found = unitOf(all, msgId);
	if (!found) {
		result.state = State::Untracked;
		return result;
	}
	if (!context.adminsKnown()) {
		result.state = State::Pending;
		return result;
	}
	const auto mi = *found;
	const auto &list = all.list;
	const auto &m = list[mi];
	result.replyToMe = (m.flags & Flag::kReplyMe) != 0;
	if (m.flags & Flag::kEndorsing) {
		result.state = State::Endorsed;
		return result;
	}
	const auto members = group(all, mi);

	auto endorseLinks = std::vector<std::uint64_t>();
	auto endorsedHit = false;
	for (const auto &record : _records) {
		if (record.kind != kRecordEndorse
			|| Older(now, record.stamp, kRecordTtl)) {
			continue;
		}
		if (Overlap(m.keys, record.keySet)) {
			endorsedHit = true;
			endorseLinks.insert(
				endorseLinks.end(),
				record.links.begin(),
				record.links.end());
		}
	}
	for (const auto u : members) {
		if (u != mi && (list[u].flags & Flag::kEndorsing)) {
			endorsedHit = true;
			endorseLinks.insert(
				endorseLinks.end(),
				list[u].links.begin(),
				list[u].links.end());
		}
	}
	std::sort(endorseLinks.begin(), endorseLinks.end());
	endorseLinks.erase(
		std::unique(endorseLinks.begin(), endorseLinks.end()),
		endorseLinks.end());
	auto impersonating = false;
	if (endorsedHit) {
		if (Subset(m.links, endorseLinks)) {
			result.state = State::Endorsed;
			return result;
		}
		impersonating = true;
	}

	for (const auto &record : _records) {
		if (record.kind != kRecordNotAd
			|| Older(now, record.stamp, kRecordTtl)) {
			continue;
		}
		if (Overlap(m.keys, record.keySet)) {
			result.state = State::NotAd;
			return result;
		}
	}

	auto gc = std::vector<std::size_t>();
	auto cls = DecisionClass::Plain;
	if (impersonating) {
		for (const auto u : members) {
			if (!(list[u].flags & Flag::kEndorsing)
				&& !Subset(list[u].links, endorseLinks)) {
				gc.push_back(u);
			}
		}
		cls = DecisionClass::Strong;
	} else {
		gc = members;
		if (m.flags & Flag::kStrongMask) {
			cls = DecisionClass::Strong;
		} else {
			auto sharesMediaOrFwd = false;
			for (const auto u : members) {
				if (u == mi) {
					continue;
				}
				for (const auto &key : list[u].keys) {
					if ((key.kind == KeyKind::Media || key.kind == KeyKind::Fwd)
						&& std::binary_search(
							m.keys.begin(),
							m.keys.end(),
							key)) {
						sharesMediaOrFwd = true;
						break;
					}
				}
				if (sharesMediaOrFwd) {
					break;
				}
			}
			cls = ((m.flags & Flag::kWeakMask) || sharesMediaOrFwd)
				? DecisionClass::Weak
				: DecisionClass::Plain;
		}
	}
	result.cls = impersonating ? DecisionClass::Impersonation : cls;
	for (const auto u : gc) {
		const auto delta = list[u].date - m.date;
		if (delta <= 7 * kDay && delta >= -7 * kDay) {
			++result.groupSize;
		}
		if (m.date - 7 * kDay <= list[u].date
			&& !OrderLess(m.order, list[u].order)) {
			++result.occurrence;
		}
	}

	const auto &rules = RulesFor(settings.sensitivity);
	auto candidates = std::vector<Candidate>();
	if (cls == DecisionClass::Strong) {
		for (const auto &rule : rules.strong) {
			candidates.push_back({ DecisionRule::Strong, rule, gc });
		}
	} else if (cls == DecisionClass::Weak) {
		for (const auto &rule : rules.weak) {
			candidates.push_back({ DecisionRule::Weak, rule, gc });
		}
		const auto sameSender = [&](KeyKind kind) {
			auto pool = std::vector<std::size_t>();
			for (const auto u : gc) {
				const auto &unit = list[u];
				if (!unit.live || unit.sender != m.sender) {
					continue;
				}
				for (const auto &key : m.keys) {
					if (key.kind == kind
						&& std::binary_search(
							unit.keys.begin(),
							unit.keys.end(),
							key)) {
						pool.push_back(u);
						break;
					}
				}
			}
			return pool;
		};
		const auto hasKind = [&](KeyKind kind) {
			return std::any_of(m.keys.begin(), m.keys.end(), [&](
					const KeyRef &key) {
				return key.kind == kind;
			});
		};
		if (rules.hasSame && (m.flags & Flag::kMediaOnly)) {
			candidates.push_back({
				DecisionRule::MediaSameSender,
				rules.same,
				sameSender(KeyKind::Media),
			});
		}
		if (rules.hasSame && hasKind(KeyKind::Fwd)) {
			candidates.push_back({
				DecisionRule::FwdSameSender,
				rules.same,
				sameSender(KeyKind::Fwd),
			});
		}
	} else if (rules.hasPlain
		&& (m.flags & Flag::kPureText)
		&& !(m.flags & Flag::kAck)) {
		auto counted = std::vector<std::size_t>();
		for (const auto u : gc) {
			if (!(list[u].flags & Flag::kAck)) {
				counted.push_back(u);
			}
		}
		candidates.push_back({ DecisionRule::Plain, rules.plain, counted });
	}

	const auto windowSatisfied = [&](const Candidate &candidate) {
		struct Item {
			std::int64_t date = 0;
			std::uint64_t sender = 0;
			bool counted = false;
		};
		auto items = std::vector<Item>();
		items.reserve(candidate.pool.size() + 1);
		auto hasM = false;
		const auto push = [&](std::size_t u) {
			const auto &unit = list[u];
			items.push_back({
				unit.date,
				unit.sender,
				unit.live && unit.sender != 0,
			});
		};
		for (const auto u : candidate.pool) {
			hasM = hasM || (u == mi);
			push(u);
		}
		if (!hasM) {
			push(mi);
		}
		std::sort(items.begin(), items.end(), [](
				const Item &a,
				const Item &b) {
			return a.date < b.date;
		});
		const auto window = candidate.limits.window;
		auto counts = std::unordered_map<std::uint64_t, int>();
		auto distinct = std::size_t(0);
		auto j = std::size_t(0);
		const auto n = items.size();
		for (std::size_t i = 0; i != n; ++i) {
			const auto t0 = items[i].date;
			while (j < n && items[j].date <= t0 + window) {
				if (items[j].counted && ++counts[items[j].sender] == 1) {
					++distinct;
				}
				++j;
			}
			const auto first = (i == 0) || (items[i - 1].date < t0);
			if (first && m.date - window <= t0 && t0 <= m.date) {
				if (j - i >= candidate.limits.hits
					&& distinct >= candidate.limits.senders) {
					return true;
				}
			}
			if (items[i].counted && --counts[items[i].sender] == 0) {
				--distinct;
			}
		}
		return false;
	};
	const auto isFirst = [&](const Candidate &candidate) {
		const auto window = candidate.limits.window;
		return std::none_of(
			candidate.pool.begin(),
			candidate.pool.end(),
			[&](std::size_t u) {
				return (u != mi)
					&& (m.date - window <= list[u].date)
					&& OrderLess(list[u].order, m.order);
			});
	};
	const auto repostedBySelf = [&](const Candidate &candidate) {
		const auto window = candidate.limits.window;
		return std::any_of(
			candidate.pool.begin(),
			candidate.pool.end(),
			[&](std::size_t u) {
				const auto &unit = list[u];
				return (u != mi)
					&& unit.live
					&& (unit.sender == m.sender)
					&& OrderLess(m.order, unit.order)
					&& (unit.date <= m.date + window);
			});
	};

	auto protectedOriginal = false;
	auto triggered = DecisionRule::Unset;
	for (const auto &candidate : candidates) {
		if (!windowSatisfied(candidate)) {
			continue;
		}
		if (!impersonating
			&& isFirst(candidate)
			&& !repostedBySelf(candidate)) {
			protectedOriginal = true;
			continue;
		}
		triggered = candidate.rule;
		break;
	}
	if (triggered == DecisionRule::Unset) {
		result.state = protectedOriginal
			? State::Original
			: State::NotTriggered;
		return result;
	}
	result.rule = triggered;

	const auto f = m.flags;
	if (f & Flag::kSelf) {
		result.state = State::Endorsed;
		return result;
	}
	if ((f & Flag::kContact) && settings.exemptContacts) {
		result.state = State::Exempt;
		return result;
	}
	if ((f & Flag::kBot) && settings.exemptBots) {
		result.state = State::Exempt;
		return result;
	}
	if ((f & (Flag::kReplyMe | Flag::kMentionMe))
		&& settings.exemptReplies
		&& !(f & Flag::kStrongMask)
		&& !impersonating
		&& !(f & Flag::kMassMention)) {
		result.state = State::Exempt;
		return result;
	}
	result.state = State::Fold;
	result.fold = true;
	return result;
}

ObserveResult Store::observe(
		const MessageInput &msg,
		const ChatContext &context,
		std::int64_t now) {
	if (!writable()) {
		return ObserveResult::Refused;
	}
	if (!context.eligible() || !EligibleMessage(msg) || !ValidMessage(msg)) {
		return ObserveResult::Ignored;
	}
	if (!InDateRange(now)) {
		return ObserveResult::Ignored;
	}
	compact(now);
	auto fp = Fingerprint(msg, context, _key);
	const auto msgId = std::uint32_t(msg.id);
	const auto date = msg.date;
	const auto existing = _byMsg.find(msgId);
	if (existing != _byMsg.end()) {
		_rows.erase(existing->second);
		_byMsg.erase(existing);
	}
	const auto username = FoldUsername(msg.sender.username);
	if (ValidUsername(username)) {
		const auto h = HandleHash(_key, username);
		const auto i = _handles.find(h);
		if (i == _handles.end()) {
			_handles.emplace(h, HandleInfo{ date, date, now });
		} else {
			auto &info = i->second;
			info.first = std::min(info.first, date);
			info.last = std::max(info.last, date);
			info.seen = std::max(info.seen, now);
		}
	}
	auto keys = std::vector<KeyRef>();
	keys.reserve(fp.keys.size());
	for (const auto &key : fp.keys) {
		keys.push_back({ key.kind, key.hash });
	}
	if (fp.endorsing && !keys.empty()) {
		auto links = fp.linkHashes;
		for (const auto h : fp.pending) {
			if (std::find(
					fp.linkHashes.begin(),
					fp.linkHashes.end(),
					h) == fp.linkHashes.end()) {
				links.push_back(h);
			}
		}
		upsertEndorse(
			now,
			Hash(_key, "msg", std::string_view(std::to_string(msgId))),
			keys,
			links);
	}
	if (fp.store) {
		const auto rid = allocateRid();
		auto row = StoredRow();
		row.rid = rid;
		row.msgId = msgId;
		row.album = msg.album;
		row.date = date;
		row.seen = now;
		row.sender = fp.senderKey;
		row.flags = fp.flags;
		row.keys = std::move(keys);
		row.links = std::move(fp.linkHashes);
		row.pending = std::move(fp.pending);
		_rows.emplace(rid, std::move(row));
		_byMsg[msgId] = rid;
	}
	enforceCaps();
	++_version;
	touch();
	return ObserveResult::Observed;
}

bool Store::remove(
		const std::vector<std::int64_t> &msgIds,
		std::int64_t now) {
	if (!writable()) {
		return false;
	}
	for (const auto msgId : msgIds) {
		if (msgId <= 0 || msgId > kMaxMsgId) {
			continue;
		}
		const auto i = _byMsg.find(std::uint32_t(msgId));
		if (i == _byMsg.end()) {
			continue;
		}
		const auto r = _rows.find(i->second);
		_byMsg.erase(i);
		if (r == _rows.end()) {
			continue;
		}
		auto &row = r->second;
		row.msgId = 0;
		row.sender = 0;
		row.flags = (row.flags & Flag::kAnonKeep) | Flag::kAnon;
		row.seen = now;
	}
	++_version;
	touch();
	return true;
}

MarkResult Store::markNotAd(
		std::int64_t msgId,
		const ChatContext &context,
		std::int64_t now) {
	if (!writable()) {
		return MarkResult::Refused;
	}
	const auto &all = units(context, now);
	const auto found = unitOf(all, msgId);
	if (!found) {
		return MarkResult::NotFound;
	}
	const auto mi = *found;
	auto keySequence = std::vector<KeyRef>();
	auto linkSequence = std::vector<std::uint64_t>();
	const auto append = [&](std::size_t u) {
		const auto &unit = all.list[u];
		keySequence.insert(
			keySequence.end(),
			unit.keys.begin(),
			unit.keys.end());
		linkSequence.insert(
			linkSequence.end(),
			unit.links.begin(),
			unit.links.end());
	};
	append(mi);
	for (const auto u : group(all, mi)) {
		if (u != mi) {
			append(u);
		}
	}
	addNotAd(
		now,
		OrderedRecordKeys(keySequence),
		FirstUnique(linkSequence, kMaxRecordLinks));
	++_version;
	touch();
	return MarkResult::Recorded;
}

void Store::upsertEndorse(
		std::int64_t stamp,
		std::uint64_t msgKey,
		const std::vector<KeyRef> &keys,
		const std::vector<std::uint64_t> &links) {
	auto ordered = OrderedRecordKeys(keys);
	auto capped = std::vector<std::uint64_t>(
		links.begin(),
		links.begin() + std::min(links.size(), kMaxRecordLinks));
	for (auto &record : _records) {
		if (record.kind == kRecordEndorse && record.msgKey == msgKey) {
			record.keySet = SortedUnique(ordered);
			record.keys = std::move(ordered);
			record.links = std::move(capped);
			record.stamp = std::max(record.stamp, stamp);
			return;
		}
	}
	auto record = StoredRecord();
	record.kind = kRecordEndorse;
	record.stamp = stamp;
	record.msgKey = msgKey;
	record.keySet = SortedUnique(ordered);
	record.keys = std::move(ordered);
	record.links = std::move(capped);
	_records.push_back(std::move(record));
	evictRecords(kRecordEndorse);
}

void Store::addNotAd(
		std::int64_t stamp,
		std::vector<KeyRef> keys,
		std::vector<std::uint64_t> links) {
	keys = OrderedRecordKeys(keys);
	if (links.size() > kMaxRecordLinks) {
		links.resize(kMaxRecordLinks);
	}
	for (auto &record : _records) {
		if (record.kind == kRecordNotAd
			&& record.keys == keys
			&& record.links == links) {
			record.stamp = std::max(record.stamp, stamp);
			return;
		}
	}
	auto record = StoredRecord();
	record.kind = kRecordNotAd;
	record.stamp = stamp;
	record.msgKey = 0;
	record.keySet = SortedUnique(keys);
	record.keys = std::move(keys);
	record.links = std::move(links);
	_records.push_back(std::move(record));
	evictRecords(kRecordNotAd);
}

void Store::evictRecords(std::uint8_t kind) {
	const auto cap = (kind == kRecordEndorse)
		? kMaxEndorseRecords
		: kMaxNotAdRecords;
	while (true) {
		auto count = std::size_t(0);
		auto oldest = _records.end();
		for (auto i = _records.begin(); i != _records.end(); ++i) {
			if (i->kind != kind) {
				continue;
			}
			++count;
			if (oldest == _records.end() || i->stamp < oldest->stamp) {
				oldest = i;
			}
		}
		if (count <= cap) {
			return;
		}
		_records.erase(oldest);
	}
}

void Store::compact(std::int64_t now) {
	auto expired = false;
	for (auto i = _rows.begin(); i != _rows.end();) {
		const auto &row = i->second;
		if (Older(now, std::max(row.date, row.seen), kEvictAfter)) {
			if (row.msgId != 0) {
				_byMsg.erase(row.msgId);
			}
			i = _rows.erase(i);
			expired = true;
		} else {
			++i;
		}
	}
	const auto records = _records.size();
	_records.erase(std::remove_if(
		_records.begin(),
		_records.end(),
		[&](const StoredRecord &record) {
			return Older(now, record.stamp, kRecordTtl);
		}), _records.end());
	auto handlesChanged = false;
	for (auto i = _handles.begin(); i != _handles.end();) {
		const auto &info = i->second;
		if (Older(now, std::max(info.last, info.seen), kHandleTtl)) {
			i = _handles.erase(i);
			handlesChanged = true;
		} else {
			++i;
		}
	}
	if (expired) {
		++_version;
	}
	if (expired || handlesChanged || records != _records.size()) {
		touch();
	}
}

void Store::enforceCaps() {
	while (_rows.size() > kMaxRows) {
		auto victim = _rows.begin();
		auto victimAge = std::max(victim->second.date, victim->second.seen);
		for (auto i = std::next(_rows.begin()); i != _rows.end(); ++i) {
			const auto age = std::max(i->second.date, i->second.seen);
			if (age < victimAge) {
				victim = i;
				victimAge = age;
			}
		}
		if (victim->second.msgId != 0) {
			_byMsg.erase(victim->second.msgId);
		}
		_rows.erase(victim);
	}
	while (_handles.size() > kMaxHandles) {
		auto victim = _handles.begin();
		auto victimAge = std::max(victim->second.last, victim->second.seen);
		for (auto i = std::next(_handles.begin()); i != _handles.end(); ++i) {
			const auto age = std::max(i->second.last, i->second.seen);
			if (age < victimAge) {
				victim = i;
				victimAge = age;
			}
		}
		_handles.erase(victim);
	}
}

std::vector<unsigned char> Store::serialize() const {
	auto body = std::vector<unsigned char>();
	auto writer = Writer(body);
	for (const auto &[rid, row] : _rows) {
		writer.u32(row.msgId);
		writer.i64(row.album);
		writer.i64(row.date);
		writer.i64(row.seen);
		writer.u64(row.sender);
		writer.u32(row.flags);
		WriteKeys(writer, row.keys);
		WriteHashes(writer, row.links);
		WriteHashes(writer, row.pending);
	}
	for (const auto &record : _records) {
		writer.u8(record.kind);
		writer.i64(record.stamp);
		writer.u64(record.msgKey);
		WriteKeys(writer, record.keys);
		WriteHashes(writer, record.links);
	}
	for (const auto &[hash, info] : _handles) {
		writer.u64(hash);
		writer.i64(info.first);
		writer.i64(info.last);
		writer.i64(info.seen);
	}
	auto result = std::vector<unsigned char>();
	result.reserve(kHeaderSize + body.size());
	auto header = Writer(result);
	header.bytes("LGAS", 4);
	header.u16(std::uint16_t(kFormatVersion));
	header.u16(std::uint16_t(kAlgoVersion));
	header.u32(AdShieldTables::kTablesVersionU32);
	header.u32(0);
	header.u64(KeyCheck(_key));
	header.u64(ChatTag(_key, _chatId));
	header.u32(std::uint32_t(_rows.size()));
	header.u32(std::uint32_t(_records.size()));
	header.u32(std::uint32_t(_handles.size()));
	header.u32(_nextRid);
	header.u64(HashBytes(_key, "body", body.data(), body.size()));
	result.insert(result.end(), body.begin(), body.end());
	return result;
}

std::unique_ptr<Store> Store::Deserialize(
		const unsigned char *data,
		std::size_t size,
		std::int64_t chatId,
		const Key &key,
		std::shared_ptr<const bool> forgotten) {
	if (!data || size < kHeaderSize) {
		return nullptr;
	}
	auto reader = Reader(data, size);
	auto formatVersion = std::uint16_t();
	auto algoVersion = std::uint16_t();
	auto tablesVersion = std::uint32_t();
	auto reserved = std::uint32_t();
	auto keyCheck = std::uint64_t();
	auto chatTag = std::uint64_t();
	auto rowCount = std::uint32_t();
	auto recordCount = std::uint32_t();
	auto handleCount = std::uint32_t();
	auto nextRid = std::uint32_t();
	auto bodyHash = std::uint64_t();
	auto magic = std::uint32_t();
	const auto headerRead = reader.u32(magic)
		&& reader.u16(formatVersion)
		&& reader.u16(algoVersion)
		&& reader.u32(tablesVersion)
		&& reader.u32(reserved)
		&& reader.u64(keyCheck)
		&& reader.u64(chatTag)
		&& reader.u32(rowCount)
		&& reader.u32(recordCount)
		&& reader.u32(handleCount)
		&& reader.u32(nextRid)
		&& reader.u64(bodyHash);
	if (!headerRead
		|| data[0] != 'L'
		|| data[1] != 'G'
		|| data[2] != 'A'
		|| data[3] != 'S'
		|| formatVersion != kFormatVersion
		|| algoVersion != kAlgoVersion
		|| tablesVersion != AdShieldTables::kTablesVersionU32) {
		return nullptr;
	}
	if (keyCheck != KeyCheck(key) || chatTag != ChatTag(key, chatId)) {
		return nullptr;
	}
	if (bodyHash != HashBytes(
			key,
			"body",
			data + kHeaderSize,
			size - kHeaderSize)) {
		return nullptr;
	}
	if (rowCount > kMaxRows
		|| recordCount > kMaxEndorseRecords + kMaxNotAdRecords
		|| handleCount > kMaxHandles) {
		return nullptr;
	}
	auto result = std::make_unique<Store>(chatId, key, std::move(forgotten));
	auto rid = std::uint32_t(0);
	for (auto i = std::uint32_t(0); i != rowCount; ++i) {
		auto row = StoredRow();
		const auto read = reader.u32(row.msgId)
			&& reader.i64(row.album)
			&& reader.i64(row.date)
			&& reader.i64(row.seen)
			&& reader.u64(row.sender)
			&& reader.u32(row.flags);
		if (!read
			|| row.msgId > kMaxMsgId
			|| !InDateRange(row.date)
			|| !InDateRange(row.seen)
			|| (row.flags & ~Flag::kStoredMask)) {
			return nullptr;
		}
		const auto anonymous = (row.msgId == 0);
		if (anonymous != ((row.flags & Flag::kAnon) != 0)
			|| (anonymous && row.sender != 0)) {
			return nullptr;
		}
		if (!ReadKeys(reader, kMaxRowKeys, row.keys)
			|| !ReadHashes(reader, kMaxLinksPerRow, row.links)
			|| !ReadHashes(reader, kMaxPendingPerRow, row.pending)) {
			return nullptr;
		}
		row.rid = ++rid;
		if (!anonymous) {
			if (!result->_byMsg.emplace(row.msgId, rid).second) {
				return nullptr;
			}
		}
		result->_rows.emplace(rid, std::move(row));
	}
	result->_nextRid = std::max(nextRid, rid + 1);
	auto endorse = std::size_t(0);
	auto notAd = std::size_t(0);
	for (auto i = std::uint32_t(0); i != recordCount; ++i) {
		auto record = StoredRecord();
		const auto read = reader.u8(record.kind)
			&& reader.i64(record.stamp)
			&& reader.u64(record.msgKey);
		if (!read
			|| (record.kind != kRecordEndorse && record.kind != kRecordNotAd)
			|| !InDateRange(record.stamp)) {
			return nullptr;
		}
		++((record.kind == kRecordEndorse) ? endorse : notAd);
		if (!ReadKeys(reader, kMaxRecordKeys, record.keys)
			|| !ReadHashes(reader, kMaxRecordLinks, record.links)) {
			return nullptr;
		}
		record.keySet = SortedUnique(record.keys);
		result->_records.push_back(std::move(record));
	}
	if (endorse > kMaxEndorseRecords || notAd > kMaxNotAdRecords) {
		return nullptr;
	}
	for (auto i = std::uint32_t(0); i != handleCount; ++i) {
		auto hash = std::uint64_t();
		auto info = HandleInfo();
		const auto read = reader.u64(hash)
			&& reader.i64(info.first)
			&& reader.i64(info.last)
			&& reader.i64(info.seen);
		if (!read
			|| !InDateRange(info.first)
			|| !InDateRange(info.last)
			|| !InDateRange(info.seen)
			|| info.first > info.last) {
			return nullptr;
		}
		if (!result->_handles.emplace(hash, info).second) {
			return nullptr;
		}
	}
	if (reader.offset() != size) {
		return nullptr;
	}
	return result;
}

} // namespace Lumina::AdShield
