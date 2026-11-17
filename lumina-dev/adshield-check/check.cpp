// AdShield conformance checker for the desktop C++ port (dev tool, not part
// of the app build). It links the engine sources from
// Telegram/SourceFiles/lumina/ and replays the shared vectors and scenarios.
//
//   sh lumina-dev/adshield-check/build.sh            (see that script)
//   ./adshield-check <dir containing vectors/ corpus/ golden/>
//
// vectors/ and corpus/ are unmodified copies of the shared adshield/ package
// (tablesVersion 25528dcc, algoVersion 3, formatVersion 3), as is
// Telegram/SourceFiles/lumina/ad_shield_tables.h (adshield/out/). golden/ is
// produced from the reference engine by gen_golden.py; rerun it whenever the
// shared package changes. Exit code 0 only when every check passes.
// All data is synthetic.
#include "check_json.h"
#include "check_memory_fs.h"

#include "lumina/ad_shield_tables.h"
#include "lumina/lumina_ad_shield.h"
#include "lumina/lumina_ad_shield_account.h"
#include "lumina/lumina_ad_shield_store.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <limits>
#include <map>
#include <random>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace {

namespace A = Lumina::AdShield;
namespace T = Lumina::AdShieldTables;
using AdShieldCheck::Json;
using AdShieldCheck::JsonParser;
using AdShieldCheck::MemoryFileSystem;

struct Tally {
	std::string name;
	int passed = 0;
	int failed = 0;
};

std::vector<Tally> Tallies;
int PrintedFailures = 0;

Tally &Section(const std::string &name) {
	for (auto &tally : Tallies) {
		if (tally.name == name) {
			return tally;
		}
	}
	Tallies.push_back({ name, 0, 0 });
	return Tallies.back();
}

bool Expect(Tally &tally, bool ok, const std::string &what) {
	if (ok) {
		++tally.passed;
	} else {
		++tally.failed;
		if (PrintedFailures++ < 200) {
			std::cerr << "FAIL [" << tally.name << "] " << what << "\n";
		}
	}
	return ok;
}

int Unexpected = 0;

void Observe(
		A::Store &chat,
		const A::MessageInput &m,
		const A::ChatContext &context,
		std::int64_t now) {
	if (chat.observe(m, context, now) == A::ObserveResult::Refused) {
		++Unexpected;
	}
}

std::string ReadText(const std::string &path) {
	auto in = std::ifstream(path, std::ios::binary);
	auto ss = std::stringstream();
	ss << in.rdbuf();
	return ss.str();
}

Json LoadJson(const std::string &path) {
	const auto text = ReadText(path);
	auto result = Json();
	auto parser = JsonParser(text);
	if (text.empty() || !parser.parse(result)) {
		std::cerr << "cannot parse " << path << "\n";
		std::exit(2);
	}
	return result;
}

std::u32string U32(std::string_view utf8) {
	auto result = std::u32string();
	for (std::size_t i = 0; i < utf8.size();) {
		const auto c = static_cast<unsigned char>(utf8[i]);
		auto cp = char32_t(c);
		auto length = std::size_t(1);
		if (c >= 0xF0) {
			cp = c & 0x07;
			length = 4;
		} else if (c >= 0xE0) {
			cp = c & 0x0F;
			length = 3;
		} else if (c >= 0xC0) {
			cp = c & 0x1F;
			length = 2;
		}
		for (std::size_t j = 1; j < length && i + j < utf8.size(); ++j) {
			cp = (cp << 6) | (static_cast<unsigned char>(utf8[i + j]) & 0x3F);
		}
		result.push_back(cp);
		i += length;
	}
	return result;
}

std::string U8(std::u32string_view s) {
	return A::ToUtf8(s);
}

std::vector<unsigned char> FromHex(std::string_view hex) {
	auto result = std::vector<unsigned char>();
	const auto nibble = [](char c) {
		return (c >= '0' && c <= '9') ? (c - '0') : (c - 'a' + 10);
	};
	for (std::size_t i = 0; i + 1 < hex.size(); i += 2) {
		result.push_back(static_cast<unsigned char>(
			(nibble(hex[i]) << 4) | nibble(hex[i + 1])));
	}
	return result;
}

A::Key KeyFromHex(std::string_view hex) {
	auto result = A::Key();
	const auto bytes = FromHex(hex);
	for (std::size_t i = 0; i != result.size() && i != bytes.size(); ++i) {
		result[i] = bytes[i];
	}
	return result;
}

A::Key TestKey() {
	auto result = A::Key();
	for (std::size_t i = 0; i != result.size(); ++i) {
		result[i] = static_cast<unsigned char>(i);
	}
	return result;
}

A::Key FilledKey(unsigned char value) {
	auto result = A::Key();
	result.fill(value);
	return result;
}

std::string ScalarHex(char32_t c) {
	char buffer[16] = { 0 };
	std::snprintf(buffer, sizeof(buffer), "%04x", unsigned(c));
	return buffer;
}

A::Role RoleFrom(const std::string &role) {
	if (role == "admin") return A::Role::Admin;
	if (role == "creator") return A::Role::Creator;
	if (role == "anon_admin") return A::Role::AnonAdmin;
	if (role == "bot") return A::Role::Bot;
	if (role == "contact") return A::Role::Contact;
	if (role == "self") return A::Role::Self;
	if (role == "channel") return A::Role::Channel;
	return A::Role::Member;
}

A::MediaKind MediaKindFrom(const std::string &kind) {
	static const auto kMap = std::map<std::string, A::MediaKind>{
		{ "photo", A::MediaKind::Photo },
		{ "video", A::MediaKind::Video },
		{ "document", A::MediaKind::Document },
		{ "audio", A::MediaKind::Audio },
		{ "contact", A::MediaKind::Contact },
		{ "sticker", A::MediaKind::Sticker },
		{ "gif", A::MediaKind::Gif },
		{ "voice", A::MediaKind::Voice },
		{ "round", A::MediaKind::Round },
		{ "dice", A::MediaKind::Dice },
		{ "location", A::MediaKind::Location },
		{ "venue", A::MediaKind::Venue },
		{ "poll", A::MediaKind::Poll },
		{ "webpage", A::MediaKind::Webpage },
		{ "game", A::MediaKind::Game },
		{ "story", A::MediaKind::Story },
		{ "invoice", A::MediaKind::Invoice },
		{ "giveaway", A::MediaKind::Giveaway },
		{ "paid", A::MediaKind::Paid },
	};
	const auto i = kMap.find(kind);
	return (i != kMap.end()) ? i->second : A::MediaKind::Absent;
}

A::ChatType ChatTypeFrom(const std::string &type) {
	if (type == "group") return A::ChatType::Group;
	if (type == "supergroup") return A::ChatType::Supergroup;
	if (type == "forum") return A::ChatType::Forum;
	if (type == "gigagroup") return A::ChatType::Gigagroup;
	return A::ChatType::Other;
}

std::string Narrow(const Json &value) {
	return U8(value.asString());
}

A::MessageInput MessageFrom(const Json &j) {
	auto m = A::MessageInput();
	m.id = j["id"].asInt();
	m.date = j["date"].asInt();
	const auto &sender = j["sender"];
	m.sender.id = sender["id"].asInt();
	m.sender.role = RoleFrom(sender.has("role")
		? Narrow(sender["role"])
		: std::string("member"));
	m.sender.username = sender["username"].asString();
	m.text = j["text"].asString();
	for (const auto &e : j["entities"].array) {
		auto entity = A::Entity();
		const auto type = Narrow(e["type"]);
		entity.type = (type == "text_url")
			? A::EntityType::TextUrl
			: (type == "mention_name")
			? A::EntityType::MentionName
			: A::EntityType::Other;
		entity.url = e["url"].asString();
		if (e.has("userId")) {
			entity.hasUserId = true;
			entity.userId = e["userId"].asInt();
		}
		m.entities.push_back(std::move(entity));
	}
	if (j.has("media")) {
		const auto &media = j["media"];
		m.media.kind = MediaKindFrom(Narrow(media["kind"]));
		if (media["id"].type == Json::Type::String) {
			m.media.id = Narrow(media["id"]);
		} else if (media["id"].type == Json::Type::Int) {
			m.media.id = std::to_string(media["id"].integer);
		}
		m.media.userId = media["userId"].asInt();
		m.media.phone = media["phone"].asString();
	}
	m.album = j["album"].asInt();
	if (j.has("forward")) {
		const auto &f = j["forward"];
		auto forward = A::Forward();
		const auto kind = Narrow(f["fromKind"]);
		forward.fromKind = (kind == "channel")
			? A::ForwardKind::Channel
			: (kind == "user")
			? A::ForwardKind::User
			: A::ForwardKind::Hidden;
		forward.fromId = f["fromId"].asInt();
		forward.postId = f["postId"].asInt();
		forward.fromName = f["fromName"].asString();
		forward.username = f["username"].asString();
		forward.date = f["date"].asInt();
		m.forward = forward;
	}
	for (const auto &b : j["buttons"].array) {
		m.buttons.push_back(b.asString());
	}
	m.replyTo = j["replyTo"].asInt();
	m.viaBot = j["viaBot"].truthy();
	m.replyToMe = j["replyToMe"].truthy();
	m.mentionsMe = j["mentionsMe"].truthy();
	m.pinned = j["pinned"].truthy();
	m.autoForward = j["autoForward"].truthy();
	m.service = j["service"].truthy();
	m.sponsored = j["sponsored"].truthy();
	m.scheduled = j["scheduled"].truthy();
	return m;
}

A::ChatContext ContextFrom(const Json &d) {
	auto data = A::ChatContextData();
	data.chatId = d["chatId"].asInt();
	data.type = ChatTypeFrom(d.has("type")
		? Narrow(d["type"])
		: std::string("supergroup"));
	data.username = d["username"].asString();
	data.linkedChannelId = d["linkedChannel"]["id"].asInt();
	data.linkedChannelUsername = d["linkedChannel"]["username"].asString();
	data.description = d["description"].asString();
	for (const auto &p : d["pinnedTexts"].array) {
		data.pinnedTexts.push_back(p.asString());
	}
	data.adminsKnown = d.has("adminsKnown") ? d["adminsKnown"].truthy() : true;
	for (const auto &h : d["memberHandles"].array) {
		data.memberHandles.push_back(h.asString());
	}
	data.selfUsername = d["selfUsername"].asString();
	return A::ChatContext(std::move(data));
}

A::ChatContext PlainContext(std::int64_t chatId) {
	auto data = A::ChatContextData();
	data.chatId = chatId;
	return A::ChatContext(std::move(data));
}

const char *kSensitivityNames[] = { "loose", "standard", "aggressive" };
const A::Sensitivity kSensitivities[] = {
	A::Sensitivity::Loose,
	A::Sensitivity::Standard,
	A::Sensitivity::Aggressive,
};

A::Settings SettingsFrom(const Json &s, A::Sensitivity sensitivity) {
	auto result = A::Settings();
	result.sensitivity = sensitivity;
	if (s.has("exemptContacts")) {
		result.exemptContacts = s["exemptContacts"].truthy();
	}
	if (s.has("exemptBots")) {
		result.exemptBots = s["exemptBots"].truthy();
	}
	if (s.has("exemptReplies")) {
		result.exemptReplies = s["exemptReplies"].truthy();
	}
	if (s.has("dryRun")) {
		result.dryRun = s["dryRun"].truthy();
	}
	if (s.has("foldPrivilegedRepeats")) {
		result.foldPrivilegedRepeats = s["foldPrivilegedRepeats"].truthy();
	}
	return result;
}

std::u32string FromUtf16Units(const Json &units) {
	auto result = std::u32string();
	const auto &list = units.array;
	for (std::size_t i = 0; i != list.size(); ++i) {
		const auto unit = char32_t(list[i].asInt());
		if (unit >= 0xD800 && unit <= 0xDBFF && i + 1 < list.size()) {
			const auto low = char32_t(list[i + 1].asInt());
			if (low >= 0xDC00 && low <= 0xDFFF) {
				result.push_back(0x10000 + ((unit - 0xD800) << 10) + (low - 0xDC00));
				++i;
				continue;
			}
		}
		result.push_back(unit);
	}
	return result;
}

std::vector<std::u32string> StringList(const Json &list) {
	auto result = std::vector<std::u32string>();
	for (const auto &item : list.array) {
		result.push_back(item.asString());
	}
	return result;
}

std::vector<std::string> NarrowList(const Json &list) {
	auto result = std::vector<std::string>();
	for (const auto &item : list.array) {
		result.push_back(Narrow(item));
	}
	return result;
}

std::vector<std::string> HexList(const std::vector<std::uint64_t> &list) {
	auto result = std::vector<std::string>();
	for (const auto value : list) {
		result.push_back(A::Hex64(value));
	}
	return result;
}

// ---- normalize ----

void CheckNormalize(const std::string &dir) {
	auto &tally = Section("normalize 55 (json)");
	const auto v = LoadJson(dir + "/vectors/normalize_vectors.json");
	Expect(
		tally,
		Narrow(v["tablesVersion"]) == std::string(T::kTablesVersion),
		"tablesVersion");
	for (const auto &c : v["cases"].array) {
		const auto id = Narrow(c["id"]);
		const auto input = c["input"].isNull()
			? FromUtf16Units(c["inputUtf16"])
			: c["input"].asString();
		const auto folded = T::Fold(input);
		auto scalars = std::vector<std::string>();
		for (const auto ch : folded) {
			scalars.push_back(ScalarHex(ch));
		}
		const auto text = T::TextForm(folded);
		const auto counts = A::LetterCounts(text);
		const auto paragraphs = A::Paragraphs(A::ExtractLinks(folded).foldNoLinks);
		auto ok = true;
		ok = ok && (scalars == NarrowList(c["foldScalars"]));
		ok = ok && (folded == c["fold"].asString());
		ok = ok && (text == c["text"].asString());
		ok = ok && (T::CjkForm(text) == c["cjk"].asString());
		ok = ok && (T::Wlen(text) == c["wlen"].asInt());
		ok = ok && (counts.first == c["cjkCount"].asInt());
		ok = ok && (counts.second == c["otherLetterCount"].asInt());
		ok = ok && (paragraphs == StringList(c["paragraphs"]));
		Expect(tally, ok, "normalize " + id + " fold=" + U8(folded));
	}
}

std::vector<std::string> SplitTabs(std::string line) {
	while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) {
		line.pop_back();
	}
	auto result = std::vector<std::string>();
	auto ss = std::stringstream(line);
	auto part = std::string();
	while (std::getline(ss, part, '\t')) {
		result.push_back(part);
	}
	if (!line.empty() && line.back() == '\t') {
		result.push_back(std::string());
	}
	return result;
}

std::string HexOf(std::string_view bytes) {
	static const char *digits = "0123456789abcdef";
	auto result = std::string();
	for (const auto c : bytes) {
		const auto b = static_cast<unsigned char>(c);
		result.push_back(digits[b >> 4]);
		result.push_back(digits[b & 15]);
	}
	return result;
}

std::string BytesOf(std::string_view hex) {
	if (hex == "-") {
		return std::string();
	}
	const auto bytes = FromHex(hex);
	return std::string(bytes.begin(), bytes.end());
}

void CheckTsv(const std::string &dir, bool crlf) {
	auto &tally = Section(crlf ? "tsv vectors (CRLF)" : "tsv vectors (LF)");
	{
		auto text = ReadText(dir + "/vectors/normalize_vectors.tsv");
		auto in = std::stringstream(text);
		auto line = std::string();
		while (std::getline(in, line)) {
			if (crlf) {
				line += '\r';
			}
			if (line.empty() || line[0] == '#' || line == "\r") {
				continue;
			}
			const auto f = SplitTabs(line);
			const auto folded = T::Fold(U32(BytesOf(f[1])));
			const auto text = T::TextForm(folded);
			const auto ok = (HexOf(U8(folded)) == f[2])
				&& (HexOf(U8(text)) == f[3])
				&& (HexOf(U8(T::CjkForm(text))) == (f.size() > 4 ? f[4] : ""))
				&& (std::to_string(T::Wlen(text)) == (f.size() > 5 ? f[5] : ""));
			Expect(tally, ok, "tsv normalize " + f[0]);
		}
	}
	{
		auto text = ReadText(dir + "/vectors/siphash_vectors.tsv");
		auto in = std::stringstream(text);
		auto line = std::string();
		const auto key = TestKey();
		while (std::getline(in, line)) {
			if (crlf) {
				line += '\r';
			}
			if (line.empty() || line[0] == '#' || line == "\r") {
				continue;
			}
			const auto f = SplitTabs(line);
			auto got = std::string();
			if (f[0] == "raw") {
				const auto m = BytesOf(f[2]);
				got = A::Hex64(T::SipHash24(
					0x0706050403020100ULL,
					0x0f0e0d0c0b0a0908ULL,
					reinterpret_cast<const unsigned char*>(m.data()),
					m.size()));
			} else {
				got = A::Hex64(A::Hash(key, BytesOf(f[1]), BytesOf(f[2])));
			}
			Expect(tally, got == f[3], "tsv siphash " + line);
		}
	}
}

// ---- siphash ----

void CheckSipHash(const std::string &dir) {
	auto &tally = Section("siphash 77");
	const auto v = LoadJson(dir + "/vectors/siphash_vectors.json");
	const auto key = KeyFromHex(Narrow(v["key"]));
	auto k0 = std::uint64_t(0);
	auto k1 = std::uint64_t(0);
	for (auto j = 7; j >= 0; --j) {
		k0 = (k0 << 8) | key[j];
	}
	for (auto j = 15; j >= 8; --j) {
		k1 = (k1 << 8) | key[j];
	}
	for (const auto &r : v["raw"].array) {
		const auto m = FromHex(Narrow(r["messageHex"]));
		const auto got = A::Hex64(T::SipHash24(k0, k1, m.data(), m.size()));
		Expect(tally, got == Narrow(r["hash"]), "raw " + Narrow(r["messageHex"]));
	}
	for (const auto &r : v["tagged"].array) {
		const auto got = A::Hex64(A::Hash(
			key,
			Narrow(r["tag"]),
			std::u32string_view(r["payload"].asString())));
		Expect(tally, got == Narrow(r["hash"]), "tagged " + Narrow(r["tag"]));
	}
	Expect(
		tally,
		A::Hex64(T::SipHash24(k0, k1, nullptr, 0)) == "726fdb47dd0e0e31",
		"paper vector empty");
}

// ---- fingerprint ----

void CheckFingerprint(const std::string &dir) {
	const auto v = LoadJson(dir + "/vectors/fingerprint_vectors.json");
	auto &tally = Section("fingerprint " + std::to_string(v["cases"].array.size()));
	const auto key = KeyFromHex(Narrow(v["key"]));
	const auto context = ContextFrom(v["context"]);
	for (const auto &c : v["cases"].array) {
		const auto id = Narrow(c["id"]);
		const auto fp = A::Fingerprint(MessageFrom(c["message"]), context, key);
		const auto &e = c["expect"];
		auto bad = std::vector<std::string>();
		const auto field = [&](bool ok, const char *name) {
			if (!ok) {
				bad.push_back(name);
			}
		};
		field(fp.fold == e["fold"].asString(), "fold");
		field(fp.foldNoLinks == e["foldNoLinks"].asString(), "foldNoLinks");
		field(fp.text == e["text"].asString(), "text");
		field(fp.cjk == e["cjk"].asString(), "cjk");
		field(fp.wlen == e["wlen"].asInt(), "wlen");
		field(fp.cjkCount == e["cjkCount"].asInt(), "cjkCount");
		field(fp.otherLetterCount == e["otherLetterCount"].asInt(), "otherLetterCount");
		field(fp.paragraphs == StringList(e["paragraphs"]), "paragraphs");
		field(fp.contentLines == StringList(e["contentLines"]), "contentLines");
		field(fp.blocks == StringList(e["blocks"]), "blocks");
		const auto &links = e["links"].array;
		auto linksOk = (links.size() == fp.links.size());
		for (std::size_t i = 0; linksOk && i != links.size(); ++i) {
			const auto &l = fp.links[i];
			const auto &x = links[i];
			linksOk = (l.token == x["token"].asString())
				&& (l.type == x["type"].asString())
				&& (A::LinkKindName(l.kind) == Narrow(x["kind"]))
				&& (A::LinkSourceName(l.source) == Narrow(x["source"]))
				&& (A::LinkFeatureName(fp.linkFeatures[i]) == Narrow(x["feature"]))
				&& (A::Hex64(A::Hash(key, "link", std::u32string_view(l.token)))
					== Narrow(x["hash"]));
		}
		field(linksOk, "links");
		field(A::StrongNames(fp.flags) == NarrowList(e["strong"]), "strong");
		field(A::WeakNames(fp.flags) == NarrowList(e["weak"]), "weak");
		field(fp.mentionCount == e["mentionCount"].asInt(), "mentionCount");
		const auto &keys = e["keys"].array;
		auto keysOk = (keys.size() == fp.keys.size());
		for (std::size_t i = 0; keysOk && i != keys.size(); ++i) {
			const auto &k = fp.keys[i];
			keysOk = (A::KeyKindName(k.kind) == Narrow(keys[i]["kind"]))
				&& (A::KeyKindTag(k.kind) == Narrow(keys[i]["tag"]))
				&& (k.payload == keys[i]["payload"].asString())
				&& (A::Hex64(k.hash) == Narrow(keys[i]["hash"]));
		}
		field(keysOk, "keys");
		field(std::int64_t(fp.flags) == e["flags"].asInt(), "flags");
		field(HexList(fp.linkHashes) == NarrowList(e["subsetLinks"]), "subsetLinks");
		field(HexList(fp.pending) == NarrowList(e["pending"]), "pending");
		field(A::StrongNames(fp.resolvedFlags) == NarrowList(e["resolvedStrong"]), "resolvedStrong");
		field(A::WeakNames(fp.resolvedFlags) == NarrowList(e["resolvedWeak"]), "resolvedWeak");
		field(std::int64_t(fp.resolvedFlags) == e["resolvedFlags"].asInt(), "resolvedFlags");
		field(A::Hex64(fp.senderKey) == Narrow(e["senderKey"]), "senderKey");
		field(fp.store == e["store"].truthy(), "store");
		field(fp.endorsing == e["endorsing"].truthy(), "endorsing");
		field(fp.privileged == e["privileged"].truthy(), "privileged");
		auto what = "fingerprint " + id;
		for (const auto &name : bad) {
			what += " " + name;
		}
		Expect(tally, bad.empty(), what);
	}
}

// ---- scenarios ----

struct Event {
	const Json *json = nullptr;
	bool overrideNow = false;
	std::int64_t now = 0;
	bool dropStates = false;
};

struct ExpectOutcome {
	std::size_t index = 0;
	std::string sensitivity;
	std::vector<std::int64_t> want;
	std::vector<std::int64_t> got;
	std::vector<std::string> badStates;
};

struct ScenarioRun {
	std::vector<ExpectOutcome> outcomes;
	std::vector<unsigned char> blob;
};

ScenarioRun RunScenario(const Json &sc, const std::vector<Event> &events) {
	auto result = ScenarioRun();
	auto account = A::Account(424242, TestKey());
	auto context = std::make_unique<A::ChatContext>(ContextFrom(sc["context"]));
	const auto chat = account.chat(context->chatId());
	auto seen = std::vector<std::int64_t>();
	auto now = std::int64_t(0);
	const auto nowOf = [&](const Event &ev, std::int64_t fallback) {
		if (ev.overrideNow) {
			return ev.now;
		}
		return ev.json->has("now") ? (*ev.json)["now"].asInt() : fallback;
	};
	for (std::size_t index = 0; index != events.size(); ++index) {
		const auto &ev = events[index];
		const auto &j = *ev.json;
		const auto op = Narrow(j["op"]);
		if (op == "observe") {
			const auto &m = j["message"];
			now = nowOf(ev, std::max(now, m["date"].asInt()));
			Observe(*chat, MessageFrom(m), *context, now);
			const auto id = m["id"].asInt();
			if (std::find(seen.begin(), seen.end(), id) == seen.end()) {
				seen.push_back(id);
			}
		} else if (op == "delete") {
			now = nowOf(ev, now);
			auto ids = std::vector<std::int64_t>();
			for (const auto &id : j["ids"].array) {
				ids.push_back(id.asInt());
			}
			if (!chat->remove(ids, now)) {
				++Unexpected;
			}
		} else if (op == "notAd") {
			now = nowOf(ev, now);
			if (chat->markNotAd(j["id"].asInt(), *context, now)
				== A::MarkResult::Refused) {
				++Unexpected;
			}
		} else if (op == "context") {
			context = std::make_unique<A::ChatContext>(ContextFrom(j["context"]));
		} else if (op == "expect") {
			now = nowOf(ev, now);
			for (auto s = 0; s != 3; ++s) {
				const auto settings = SettingsFrom(sc["settings"], kSensitivities[s]);
				auto outcome = ExpectOutcome();
				outcome.index = index;
				outcome.sensitivity = kSensitivityNames[s];
				auto decisions = std::map<std::int64_t, A::Decision>();
				for (const auto id : seen) {
					decisions[id] = chat->decide(id, *context, settings, now);
					if (decisions[id].fold) {
						outcome.got.push_back(id);
					}
				}
				std::sort(outcome.got.begin(), outcome.got.end());
				for (const auto &id : j["fold"][kSensitivityNames[s]].array) {
					outcome.want.push_back(id.asInt());
				}
				std::sort(outcome.want.begin(), outcome.want.end());
				if (!ev.dropStates) {
					for (const auto &[sid, state] : j["states"][kSensitivityNames[s]].object) {
						const auto id = std::stoll(sid);
						const auto got = std::string(A::StateName(decisions[id].state));
						if (got != Narrow(state)) {
							outcome.badStates.push_back(
								sid + ": want " + Narrow(state) + " got " + got);
						}
					}
				}
				result.outcomes.push_back(std::move(outcome));
			}
		}
	}
	result.blob = chat->serialize();
	return result;
}

std::vector<Event> AllEvents(const Json &sc) {
	auto result = std::vector<Event>();
	for (const auto &ev : sc["events"].array) {
		result.push_back({ &ev, false, 0, false });
	}
	return result;
}

std::string Ids(const std::vector<std::int64_t> &ids) {
	auto result = std::string("[");
	for (std::size_t i = 0; i != ids.size(); ++i) {
		result += (i ? "," : "") + std::to_string(ids[i]);
	}
	return result + "]";
}

std::map<std::string, std::pair<std::size_t, std::string>> LoadGolden(
		const std::string &dir) {
	auto result = std::map<std::string, std::pair<std::size_t, std::string>>();
	auto in = std::stringstream(ReadText(dir + "/golden/scenario_states.tsv"));
	auto line = std::string();
	while (std::getline(in, line)) {
		if (line.empty() || line[0] == '#') {
			continue;
		}
		const auto f = SplitTabs(line);
		if (f.size() == 3) {
			result[f[0]] = { std::size_t(std::stoull(f[1])), f[2] };
		}
	}
	return result;
}

void CheckScenarios(const std::string &dir, const Json &data) {
	auto &tally = Section(
		"scenarios " + std::to_string(data["scenarios"].array.size()));
	auto &expects = Section("scenario expect checks (x3 sensitivities)");
	auto &golden = Section("scenario end state == reference blob");
	const auto states = LoadGolden(dir);
	for (const auto &sc : data["scenarios"].array) {
		const auto id = Narrow(sc["id"]);
		const auto run = RunScenario(sc, AllEvents(sc));
		auto ok = true;
		for (const auto &o : run.outcomes) {
			const auto good = (o.got == o.want) && o.badStates.empty();
			auto what = id + " event#" + std::to_string(o.index) + " " + o.sensitivity
				+ " want " + Ids(o.want) + " got " + Ids(o.got);
			for (const auto &s : o.badStates) {
				what += " | " + s;
			}
			Expect(expects, good, what);
			ok = ok && good;
		}
		Expect(tally, ok, "scenario " + id);
		const auto g = states.find(id);
		const auto blobHash = A::Hex64(T::SipHash24(
			0x0706050403020100ULL,
			0x0f0e0d0c0b0a0908ULL,
			run.blob.data(),
			run.blob.size()));
		Expect(
			golden,
			g != states.end()
				&& g->second.first == run.blob.size()
				&& g->second.second == blobHash,
			"golden " + id + " size " + std::to_string(run.blob.size()));
	}
}

std::vector<std::pair<std::string, std::vector<std::int64_t>>> FoldSets(
		const ScenarioRun &run) {
	auto result = std::vector<std::pair<std::string, std::vector<std::int64_t>>>();
	for (const auto &o : run.outcomes) {
		result.emplace_back(o.sensitivity, o.got);
	}
	return result;
}

void CheckLoadOrder(const Json &data) {
	auto &tally = Section("shuffled load order invariance");
	auto rng = std::mt19937_64(20261117);
	auto covered = std::set<std::string>();
	for (const auto &sc : data["scenarios"].array) {
		const auto &events = sc["events"].array;
		auto skip = false;
		auto observes = std::vector<const Json*>();
		auto ids = std::set<std::int64_t>();
		const Json *lastExpect = nullptr;
		for (const auto &ev : events) {
			const auto op = Narrow(ev["op"]);
			if (op == "delete" || op == "notAd" || op == "context") {
				skip = true;
			} else if (op == "observe") {
				observes.push_back(&ev);
				if (!ids.insert(ev["message"]["id"].asInt()).second) {
					skip = true;
				}
			} else if (op == "expect") {
				lastExpect = &ev;
			}
		}
		if (skip || !lastExpect || observes.empty()) {
			continue;
		}
		auto maxNow = std::numeric_limits<std::int64_t>::min();
		for (const auto ev : observes) {
			maxNow = std::max(maxNow, ev->has("now")
				? (*ev)["now"].asInt()
				: (*ev)["message"]["date"].asInt());
		}
		const auto build = [&](const std::vector<const Json*> &order) {
			auto list = std::vector<Event>();
			for (const auto ev : order) {
				list.push_back({ ev, true, maxNow, false });
			}
			list.push_back({ lastExpect, true, maxNow, true });
			return list;
		};
		const auto id = Narrow(sc["id"]);
		const auto baseline = FoldSets(RunScenario(sc, build(observes)));
		covered.insert(id);
		for (auto round = 0; round != 6; ++round) {
			auto order = observes;
			if (round == 5) {
				std::reverse(order.begin(), order.end());
			} else {
				for (auto i = order.size(); i > 1; --i) {
					std::swap(order[i - 1], order[std::size_t(rng() % i)]);
				}
			}
			const auto got = FoldSets(RunScenario(sc, build(order)));
			Expect(
				tally,
				got == baseline,
				id + (round == 5 ? " reversed" : " shuffle#" + std::to_string(round)));
		}
	}
	for (const auto need : { "FP-20a", "FP-20b", "RG-01", "RG-02", "EV-25" }) {
		Expect(tally, covered.count(need) > 0, std::string("covered ") + need);
	}
	auto byId = std::map<std::string, const Json*>();
	for (const auto &sc : data["scenarios"].array) {
		byId[Narrow(sc["id"])] = &sc;
	}
	const auto a = FoldSets(RunScenario(*byId["FP-20a"], AllEvents(*byId["FP-20a"])));
	const auto b = FoldSets(RunScenario(*byId["FP-20b"], AllEvents(*byId["FP-20b"])));
	Expect(tally, a == b, "FP-20a equals FP-20b (reverse-order learning)");
	std::printf("  load-order: %zu scenarios shuffled (5 random + reversed each)\n", covered.size());
}

// ---- differential golden data from the reference engine ----

std::string TestDigest(const std::string &text) {
	return A::Hex64(T::SipHash24(
		0x0706050403020100ULL,
		0x0f0e0d0c0b0a0908ULL,
		reinterpret_cast<const unsigned char*>(text.data()),
		text.size()));
}

std::string LinkFields(const A::Link &l) {
	return U8(l.token) + "\t" + U8(l.type) + "\t"
		+ std::string(A::LinkKindName(l.kind)) + "\t"
		+ std::string(A::LinkSourceName(l.source)) + "\t"
		+ U8(l.domain) + "\t" + (l.shortener ? "1" : "0");
}

std::string ExtractionCanon(const std::u32string &raw) {
	const auto f = T::Fold(raw);
	const auto ex = A::ExtractLinks(f);
	auto lines = std::vector<std::string>();
	lines.push_back("F" + U8(f));
	lines.push_back("X" + U8(ex.foldNoLinks));
	for (const auto &l : ex.links) {
		lines.push_back("L" + LinkFields(l));
	}
	for (const auto &m : ex.mentions) {
		lines.push_back("M" + U8(m));
	}
	lines.push_back(ex.pointer ? "P1" : "P0");
	for (const auto &n : ex.longNumbers) {
		lines.push_back("N" + U8(n));
	}
	for (const auto &h : A::LeadingHandles(f)) {
		lines.push_back("H" + U8(h));
	}
	for (const auto &p : A::Paragraphs(ex.foldNoLinks)) {
		lines.push_back("G" + U8(p));
	}
	for (const auto source : { A::LinkSource::Text, A::LinkSource::Hidden }) {
		const auto u = A::CanonicalizeUrl(f, source);
		lines.push_back(u ? ("U" + LinkFields(*u)) : std::string("U-"));
	}
	auto result = std::string();
	for (std::size_t i = 0; i != lines.size(); ++i) {
		result += (i ? "\n" : "") + lines[i];
	}
	return result;
}

void CheckExtraction(const std::string &dir) {
	auto &tally = Section("extraction differential (ref golden)");
	const auto v = LoadJson(dir + "/golden/extraction.json");
	Expect(
		tally,
		Narrow(v["tablesVersion"]) == std::string(T::kTablesVersion),
		"golden tablesVersion");
	for (const auto &c : v["cases"].array) {
		const auto input = c["input"].asString();
		const auto canon = ExtractionCanon(input);
		if (c.has("expect")) {
			Expect(
				tally,
				canon == Narrow(c["expect"]),
				"extract [" + U8(input) + "]\n  got:\n" + canon
					+ "\n  want:\n" + Narrow(c["expect"]));
		} else {
			Expect(
				tally,
				TestDigest(canon) == Narrow(c["digest"]),
				"extract [" + U8(input) + "]\n  got:\n" + canon);
		}
	}
}

std::string FingerprintCanon(const A::FingerprintResult &fp) {
	const auto join = [](const std::vector<std::string> &parts, const char *sep) {
		auto result = std::string();
		for (std::size_t i = 0; i != parts.size(); ++i) {
			result += (i ? sep : "") + parts[i];
		}
		return result;
	};
	const auto narrow = [](const std::vector<std::u32string> &list) {
		auto result = std::vector<std::string>();
		for (const auto &item : list) {
			result.push_back(U8(item));
		}
		return result;
	};
	const auto paragraphs = narrow(fp.paragraphs);
	const auto contentLines = narrow(fp.contentLines);
	const auto blocks = narrow(fp.blocks);
	auto links = std::vector<std::string>();
	for (std::size_t i = 0; i != fp.links.size(); ++i) {
		const auto &l = fp.links[i];
		links.push_back(join({
			U8(l.token),
			U8(l.type),
			std::string(A::LinkKindName(l.kind)),
			std::string(A::LinkSourceName(l.source)),
			std::string(A::LinkFeatureName(fp.linkFeatures[i])) }, "\x1d"));
	}
	auto keys = std::vector<std::string>();
	for (const auto &k : fp.keys) {
		keys.push_back(join({
			std::string(A::KeyKindName(k.kind)),
			U8(k.payload),
			A::Hex64(k.hash) }, "\x1d"));
	}
	return join({
		U8(fp.fold),
		U8(fp.foldNoLinks),
		U8(fp.text),
		U8(fp.cjk),
		std::to_string(fp.wlen),
		std::to_string(fp.cjkCount),
		std::to_string(fp.otherLetterCount),
		join(paragraphs, "\x1e"),
		join(links, "\x1e"),
		join(keys, "\x1e"),
		std::to_string(fp.flags),
		join(HexList(fp.linkHashes), ","),
		join(HexList(fp.pending), ","),
		std::to_string(fp.resolvedFlags),
		A::Hex64(fp.senderKey),
		fp.store ? "1" : "0",
		fp.endorsing ? "1" : "0",
		std::to_string(fp.mentionCount),
		join(contentLines, "\x1e"),
		join(blocks, "\x1e"),
		fp.privileged ? "1" : "0" }, "\x1f");
}

void CheckFuzzFingerprint(const std::string &dir) {
	auto &tally = Section("fingerprint differential (ref golden)");
	const auto v = LoadJson(dir + "/golden/fuzz_fingerprint.json");
	const auto context = ContextFrom(v["context"]);
	const auto key = TestKey();
	for (const auto &c : v["cases"].array) {
		const auto fp = A::Fingerprint(MessageFrom(c["message"]), context, key);
		const auto canon = FingerprintCanon(fp);
		Expect(
			tally,
			TestDigest(canon) == Narrow(c["digest"]),
			"fingerprint fuzz id " + std::to_string(c["message"]["id"].asInt())
				+ " text [" + U8(c["message"]["text"].asString()) + "]");
	}
}

void CheckFuzzStore(const std::string &dir) {
	auto &tally = Section("store differential (ref golden)");
	const auto v = LoadJson(dir + "/golden/fuzz_store.json");
	const auto &templates = v["templates"].array;
	const auto &senders = v["senders"].array;
	const auto &contexts = v["contexts"].array;
	auto expects = 0;
	for (const auto &c : v["cases"].array) {
		auto account = A::Account(424242, TestKey());
		auto context = std::make_unique<A::ChatContext>(ContextFrom(contexts[0]));
		const auto chat = account.chat(context->chatId());
		auto seen = std::vector<std::int64_t>();
		auto now = std::int64_t(0);
		auto lines = std::vector<std::string>();
		for (const auto &ev : c["events"].array) {
			const auto &e = ev.array;
			const auto op = Narrow(e[0]);
			if (op == "o") {
				auto m = MessageFrom(templates[std::size_t(e[4].asInt())]);
				const auto &sender = senders[std::size_t(e[3].asInt())];
				m.id = e[1].asInt();
				m.date = e[2].asInt();
				m.sender.id = sender["id"].asInt();
				m.sender.role = RoleFrom(Narrow(sender["role"]));
				m.sender.username = sender["username"].asString();
				m.album = e[5].asInt();
				now = e[6].isNull() ? std::max(now, m.date) : e[6].asInt();
				Observe(*chat, m, *context, now);
				if (std::find(seen.begin(), seen.end(), m.id) == seen.end()) {
					seen.push_back(m.id);
				}
			} else if (op == "d") {
				now = e[2].isNull() ? now : e[2].asInt();
				auto ids = std::vector<std::int64_t>();
				for (const auto &id : e[1].array) {
					ids.push_back(id.asInt());
				}
				if (!chat->remove(ids, now)) {
					++Unexpected;
				}
			} else if (op == "n") {
				now = e[2].isNull() ? now : e[2].asInt();
				if (chat->markNotAd(e[1].asInt(), *context, now) == A::MarkResult::Refused) {
					++Unexpected;
				}
			} else if (op == "c") {
				context = std::make_unique<A::ChatContext>(
					ContextFrom(contexts[std::size_t(e[1].asInt())]));
			} else if (op == "e") {
				now = e[1].isNull() ? now : e[1].asInt();
				++expects;
				for (const auto privileged : { false, true }) {
					for (auto s = 0; s != 3; ++s) {
						auto settings = A::Settings();
						settings.sensitivity = kSensitivities[s];
						settings.foldPrivilegedRepeats = privileged;
						for (const auto id : seen) {
							const auto d = chat->decide(id, *context, settings, now);
							char buffer[256] = { 0 };
							std::snprintf(
								buffer,
								sizeof(buffer),
								"%s%s %lld:%s:%d:%s:%s:%d:%d:%d",
								kSensitivityNames[s],
								privileged ? "+priv" : "",
								static_cast<long long>(id),
								std::string(A::StateName(d.state)).c_str(),
								d.fold ? 1 : 0,
								std::string(A::DecisionClassName(d.cls)).c_str(),
								std::string(A::DecisionRuleName(d.rule)).c_str(),
								d.occurrence,
								d.groupSize,
								d.replyToMe ? 1 : 0);
							lines.push_back(buffer);
						}
					}
				}
			}
		}
		const auto blob = chat->serialize();
		lines.push_back("blob " + A::Hex64(T::SipHash24(
			0x0706050403020100ULL,
			0x0f0e0d0c0b0a0908ULL,
			blob.data(),
			blob.size())));
		auto canon = std::string();
		for (std::size_t i = 0; i != lines.size(); ++i) {
			canon += (i ? "\n" : "") + lines[i];
		}
		auto what = std::string("store sequence:");
		for (const auto &ev : c["events"].array) {
			what += " " + Narrow(ev.array[0]);
		}
		Expect(tally, TestDigest(canon) == Narrow(c["digest"]), what + "\n" + canon);
	}
	std::printf("  store differential: %zu sequences, %d expect points (all Decision fields, 3 sensitivities x foldPrivilegedRepeats off/on)\n",
		v["cases"].array.size(),
		expects);
}

// ---- storage ----

struct Filled {
	std::unique_ptr<A::ChatContext> context;
	std::shared_ptr<A::Store> chat;
	std::int64_t now = 0;
	const Json *scenario = nullptr;
};

const Json *FindScenario(const Json &data, const std::string &id) {
	for (const auto &sc : data["scenarios"].array) {
		if (Narrow(sc["id"]) == id) {
			return &sc;
		}
	}
	return nullptr;
}

Filled FilledChat(const Json &data, A::Account &account) {
	auto result = Filled();
	result.scenario = FindScenario(data, "TR-03");
	result.context = std::make_unique<A::ChatContext>(
		ContextFrom((*result.scenario)["context"]));
	result.chat = account.chat(result.context->chatId());
	for (const auto &ev : (*result.scenario)["events"].array) {
		if (Narrow(ev["op"]) == "observe") {
			result.now = std::max(result.now, ev["message"]["date"].asInt());
			Observe(
				*result.chat,
				MessageFrom(ev["message"]),
				*result.context,
				result.now);
		}
	}
	return result;
}

A::MessageInput SimpleMessage(
		std::int64_t id,
		std::int64_t date,
		std::int64_t sender,
		const std::string &text) {
	auto m = A::MessageInput();
	m.id = id;
	m.date = date;
	m.sender.id = sender;
	m.text = U32(text);
	return m;
}

bool Contains(const std::vector<unsigned char> &blob, const std::string &needle) {
	if (needle.empty()) {
		return true;
	}
	return std::search(
		blob.begin(),
		blob.end(),
		needle.begin(),
		needle.end(),
		[](unsigned char a, char b) { return a == static_cast<unsigned char>(b); })
		!= blob.end();
}

void CheckStorage(const Json &data) {
	auto &tally = Section("storage");
	const auto key = TestKey();
	{
		auto account = A::Account(1, key);
		auto f = FilledChat(data, account);
		const auto blob = f.chat->serialize();
		const auto back = A::Store::Deserialize(
			blob.data(),
			blob.size(),
			f.context->chatId(),
			key);
		Expect(tally, back != nullptr, "roundtrip deserialize");
		if (back) {
			Expect(tally, back->serialize() == blob, "roundtrip bytes identical");
			auto same = true;
			for (auto id = 1; id <= 4; ++id) {
				for (const auto sensitivity : kSensitivities) {
					auto settings = A::Settings();
					settings.sensitivity = sensitivity;
					same = same && (f.chat->decide(id, *f.context, settings, f.now)
						== back->decide(id, *f.context, settings, f.now));
				}
			}
			Expect(tally, same, "roundtrip same decisions");
		}
		Expect(tally, !A::Store::Deserialize(blob.data(), blob.size(), f.context->chatId(), A::Key()), "wrong key discarded");
		Expect(tally, !A::Store::Deserialize(blob.data(), blob.size(), f.context->chatId() + 1, key), "wrong chat discarded");
		Expect(tally, !A::Store::Deserialize(blob.data(), blob.size() - 1, f.context->chatId(), key), "truncated discarded");
		for (const auto position : { std::size_t(60), blob.size() / 2, blob.size() - 3 }) {
			auto bad = blob;
			bad[position] ^= 0x40;
			Expect(
				tally,
				!A::Store::Deserialize(bad.data(), bad.size(), f.context->chatId(), key),
				"corrupt body byte " + std::to_string(position));
		}
		auto bad = blob;
		bad[32] = 0xB9;
		bad[33] = 0x0B;
		bad[34] = 0;
		bad[35] = 0;
		Expect(tally, !A::Store::Deserialize(bad.data(), bad.size(), f.context->chatId(), key), "header rowCount 3001");
	}
	{
		auto &privacy = Section("no plaintext in blob");
		auto account = A::Account(1, key);
		auto f = FilledChat(data, account);
		const auto blob = f.chat->serialize();
		for (const auto &ev : (*f.scenario)["events"].array) {
			if (Narrow(ev["op"]) != "observe") {
				continue;
			}
			const auto &m = ev["message"];
			const auto folded = T::Fold(m["text"].asString());
			const auto senderId = m["sender"]["id"].asInt();
			for (const auto &fragment : {
					U8(std::u32string_view(folded).substr(0, 6)),
					std::string("viproom9"),
					std::string("VipRoom9"),
					std::to_string(senderId),
					U8(U32("\xe6\x9c\xac\xe7\xbe\xa4\xe6\x8f\x90\xe4\xbe\x9b")) }) {
				Expect(privacy, !Contains(blob, fragment), "fragment " + fragment);
			}
			auto le = std::string();
			for (auto i = 0; i != 8; ++i) {
				le.push_back(char(std::uint64_t(senderId) >> (8 * i)));
			}
			Expect(privacy, !Contains(blob, le), "sender id LE bytes");
		}
	}
	{
		auto account = A::Account(1, key);
		auto f = FilledChat(data, account);
		auto links = std::vector<std::uint64_t>();
		for (const auto &[rid, row] : f.chat->rows()) {
			if (row.msgId == 1) {
				links = row.links;
			}
		}
		Expect(tally, f.chat->remove({ 1 }, f.now + 1), "delete accepted");
		auto anon = std::vector<A::StoredRow>();
		for (const auto &[rid, row] : f.chat->rows()) {
			if (row.msgId == 0) {
				anon.push_back(row);
			}
		}
		Expect(tally, anon.size() == 1, "delete: one anonymous row");
		if (anon.size() == 1) {
			const auto &r = anon[0];
			Expect(tally, r.sender == 0, "delete: sender cleared");
			Expect(tally, (r.flags & A::Flag::kAnon) != 0, "delete: ANON flag");
			Expect(tally, (r.flags & ~(A::Flag::kAnonKeep | A::Flag::kAnon)) == 0, "delete: flags reduced");
			Expect(tally, r.links == links, "delete: link hashes kept");
			Expect(tally, r.seen == f.now + 1, "delete: seen updated");
		}
		auto settings = A::Settings();
		Expect(tally, f.chat->decide(1, *f.context, settings, f.now + 1).state == A::State::Untracked, "delete: untracked");
		const auto blob = f.chat->serialize();
		Expect(tally, A::Store::Deserialize(blob.data(), blob.size(), f.context->chatId(), key) != nullptr, "delete: blob loads");
	}
	{
		const auto context = PlainContext(-1);
		auto chat = A::Store(-1, key);
		auto ignored = 0;
		for (const auto &[id, date] : std::vector<std::pair<std::int64_t, std::int64_t>>{
				{ 0, 1000 },
				{ -5, 1000 },
				{ 0x80000000LL, 1000 },
				{ 1, -1 },
				{ 1, (std::int64_t(1) << 40) + 1 } }) {
			auto m = SimpleMessage(id, date, 1, "\xe7\xa7\x81\xe8\x81\x8a\xe6\x88\x91 t.me/+abc");
			ignored += (chat.observe(m, context, 2000) == A::ObserveResult::Ignored) ? 1 : 0;
		}
		auto m = SimpleMessage(1, 1000, 1, "\xe7\xa7\x81\xe8\x81\x8a\xe6\x88\x91 t.me/+abc");
		ignored += (chat.observe(m, context, -1) == A::ObserveResult::Ignored) ? 1 : 0;
		ignored += (chat.observe(m, context, (std::int64_t(1) << 40) + 1) == A::ObserveResult::Ignored) ? 1 : 0;
		Expect(tally, ignored == 7, "observe rejects out of range id/date/now");
		Expect(tally, chat.rows().empty() && chat.handles().empty(), "observe out of range stores nothing");
	}
	{
		const auto context = PlainContext(-1);
		auto chat = A::Store(-1, key);
		auto m = SimpleMessage(5, 1000, 9, "\xe4\xbb\x8a\xe6\x99\x9a\xe5\x85\xab\xe9\xbb\x9e\xe8\xaa\x9e\xe9\x9f\xb3\xe8\x81\x8a\xe5\xa4\xa9\xe5\xae\xa4\xe9\x96\x8b\xe6\x94\xbe\xef\xbc\x8c\xe6\xad\xa1\xe8\xbf\x8e\xe5\xa4\xa7\xe5\xae\xb6\xe4\xbe\x86\xe7\x8e\xa9");
		m.sender.role = A::Role::Admin;
		for (auto k = 0; k != 300; ++k) {
			Observe(chat, m, context, 2000 + k);
		}
		auto endorse = 0;
		for (const auto &record : chat.records()) {
			endorse += (record.kind == A::kRecordEndorse) ? 1 : 0;
		}
		Expect(tally, endorse == 1, "endorse record upsert (300 observes -> 1 record)");
		Expect(tally, !chat.records().empty() && chat.records()[0].stamp == 2299, "endorse stamp = max");
		m.text = U32("\xe6\x94\xb9\xe9\x81\x8e\xe7\x9a\x84\xe5\x85\xac\xe5\x91\x8a\xef\xbc\x9a\xe4\xbb\x8a\xe6\x99\x9a\xe4\xb9\x9d\xe9\xbb\x9e\xe8\xaa\x9e\xe9\x9f\xb3\xe8\x81\x8a\xe5\xa4\xa9\xe5\xae\xa4\xe9\x96\x8b\xe6\x94\xbe");
		Observe(chat, m, context, 3000);
		Expect(tally, chat.records().size() == 1, "edit upserts the endorse record");
		const auto first = chat.markNotAd(5, context, 3001);
		const auto second = chat.markNotAd(5, context, 3002);
		auto notAd = 0;
		for (const auto &record : chat.records()) {
			notAd += (record.kind == A::kRecordNotAd) ? 1 : 0;
		}
		Expect(tally, first == A::MarkResult::Recorded && second == A::MarkResult::Recorded && notAd == 1, "not-ad record deduplicated");
	}
	{
		auto results = std::set<std::pair<std::vector<std::pair<int, std::uint64_t>>, std::vector<std::uint64_t>>>();
		const auto common = std::vector<std::string>{
			"\xe5\x85\xb1\xe7\x94\xa8\xe6\xae\xb5\xe8\x90\xbd\xe7\xac\xac\xe4\xb8\x80\xe6\xae\xb5\xe7\x9a\x84\xe5\x85\xa7\xe5\xae\xb9\xe8\xa6\x81\xe8\xb6\x85\xe9\x81\x8e\xe5\x8d\x81\xe5\x85\xad\xe5\x80\x8b\xe4\xb8\xad\xe6\x96\x87\xe5\xad\x97\xe6\x89\x8d\xe6\x9c\x83\xe8\xa2\xab\xe7\x95\xb6\xe6\x88\x90\xe6\xae\xb5\xe8\x90\xbd\xe9\x8d\xb5\xe5\x96\x94",
			"\xe5\x85\xb1\xe7\x94\xa8\xe6\xae\xb5\xe8\x90\xbd\xe7\xac\xac\xe4\xba\x8c\xe6\xae\xb5\xe7\x9a\x84\xe5\x85\xa7\xe5\xae\xb9\xe8\xa6\x81\xe8\xb6\x85\xe9\x81\x8e\xe5\x8d\x81\xe5\x85\xad\xe5\x80\x8b\xe4\xb8\xad\xe6\x96\x87\xe5\xad\x97\xe6\x89\x8d\xe6\x9c\x83\xe8\xa2\xab\xe7\x95\xb6\xe6\x88\x90\xe6\xae\xb5\xe8\x90\xbd\xe9\x8d\xb5\xe5\x96\x94",
		};
		const auto ordinals = U32("\xe4\xb8\x80\xe4\xba\x8c\xe4\xb8\x89\xe5\x9b\x9b\xe4\xba\x94");
		const auto stems = U32("\xe7\x94\xb2\xe4\xb9\x99\xe4\xb8\x99\xe4\xb8\x81\xe6\x88\x8a\xe5\xb7\xb1\xe5\xba\x9a\xe8\xbe\x9b\xe5\xa3\xac\xe7\x99\xb8\xe5\xad\x90\xe4\xb8\x91\xe5\xaf\x85\xe5\x8d\xaf");
		auto messages = std::vector<A::MessageInput>();
		for (auto i = 0; i != 5; ++i) {
			auto text = std::u32string();
			for (const auto &line : common) {
				text += U32(line) + U"\n";
			}
			for (auto j = 0; j != 14; ++j) {
				text += U32("\xe7\xac\xac") + ordinals.substr(std::size_t(i), 1)
					+ U32("\xe5\x89\x87\xe8\xa8\x8a\xe6\x81\xaf\xe8\x87\xaa\xe5\xb7\xb1\xe7\x9a\x84\xe6\xae\xb5\xe8\x90\xbd\xe4\xb9\x8b")
					+ stems.substr(std::size_t(j), 1)
					+ U32("\xef\xbc\x8c\xe5\x85\xa7\xe5\xae\xb9\xe5\x90\x8c\xe6\xa8\xa3\xe8\xa6\x81\xe8\xb6\x85\xe9\x81\x8e\xe5\x8d\x81\xe5\x85\xad\xe5\x80\x8b\xe4\xb8\xad\xe6\x96\x87\xe5\xad\x97\xe6\x89\x8d\xe7\xae\x97");
				if (j != 13) {
					text += U"\n";
				}
			}
			auto m = A::MessageInput();
			m.id = i + 1;
			m.date = 1000 + i;
			m.sender.id = 100 + i;
			m.text = text;
			messages.push_back(std::move(m));
		}
		const auto context = PlainContext(-1);
		auto rng = std::mt19937_64(7);
		auto lastKeys = std::vector<A::KeyRef>();
		for (auto round = 0; round != 6; ++round) {
			auto order = messages;
			for (auto i = order.size(); i > 1; --i) {
				std::swap(order[i - 1], order[std::size_t(rng() % i)]);
			}
			auto chat = A::Store(-1, key);
			for (const auto &m : order) {
				Observe(chat, m, context, 5000);
			}
			Expect(tally, chat.markNotAd(3, context, 5001) == A::MarkResult::Recorded, "notAd recorded");
			for (const auto &record : chat.records()) {
				if (record.kind != A::kRecordNotAd) {
					continue;
				}
				auto keys = std::vector<std::pair<int, std::uint64_t>>();
				for (const auto &k : record.keys) {
					keys.emplace_back(int(k.kind), k.hash);
				}
				results.emplace(keys, record.links);
				lastKeys = record.keys;
				break;
			}
		}
		Expect(tally, results.size() == 1, "not-ad record independent of load order");
		Expect(tally, lastKeys.size() == A::kMaxRecordKeys, "not-ad record capped at 128 keys");
		// v2 exact keys, then paragraph keys (= the v2 record, 64), then v3.
		const auto rank = [](A::KeyKind kind) {
			return (kind == A::KeyKind::Para)
				? 1
				: (int(kind) <= 7)
				? 0
				: 2;
		};
		auto ordered = true;
		auto v2Keys = std::size_t(0);
		for (std::size_t i = 0; i != lastKeys.size(); ++i) {
			v2Keys += (rank(lastKeys[i].kind) < 2) ? 1 : 0;
			if (i > 0 && rank(lastKeys[i - 1].kind) > rank(lastKeys[i].kind)) {
				ordered = false;
			}
		}
		Expect(tally, ordered, "not-ad record: exact keys, paragraph keys, then v3 keys");
		Expect(tally, v2Keys == A::kRecordV2Keys, "not-ad record: 64 v2 keys first");
		const auto fp3 = A::Fingerprint(messages[2], context, key);
		auto ownKept = true;
		for (const auto &k : fp3.keys) {
			if (k.kind == A::KeyKind::Para) {
				ownKept = ownKept && std::find(lastKeys.begin(), lastKeys.end(), A::KeyRef{ k.kind, k.hash }) != lastKeys.end();
			}
		}
		Expect(tally, ownKept, "not-ad record keeps m's own paragraph keys");
	}
	{
		const auto context = PlainContext(-1);
		auto chat = A::Store(-1, key);
		for (auto i = 0; i != 40; ++i) {
			char name[16] = { 0 };
			std::snprintf(name, sizeof(name), "user_%03d", i);
			auto m = SimpleMessage(i + 1, 1000 + i, i, "\xe7\xa7\x81\xe8\x81\x8a\xe6\x88\x91 t.me/+x" + std::to_string(i));
			m.sender.username = U32(name);
			Observe(chat, m, context, 2000);
		}
		const auto blob = chat.serialize();
		const auto handles = std::size_t(blob[40]) | (std::size_t(blob[41]) << 8);
		auto hashes = std::vector<std::uint64_t>();
		for (std::size_t i = 0; i != handles; ++i) {
			const auto offset = blob.size() - 32 * (handles - i);
			auto h = std::uint64_t(0);
			for (auto b = 0; b != 8; ++b) {
				h |= std::uint64_t(blob[offset + b]) << (8 * b);
			}
			hashes.push_back(h);
		}
		const auto high = std::count_if(hashes.begin(), hashes.end(), [](std::uint64_t h) {
			return h >= (std::uint64_t(1) << 63);
		});
		Expect(tally, handles == 40 && std::is_sorted(hashes.begin(), hashes.end()), "handles serialized sorted unsigned");
		Expect(tally, high > 0 && high < 40, "handles cover both halves of u64");
	}
	{
		const auto context = PlainContext(-1);
		auto chat = A::Store(-1, key);
		Observe(chat, SimpleMessage(1, 0, 1, "\xe7\xa7\x81\xe8\x81\x8a\xe6\x88\x91\xe9\xa0\x98\xe5\x8f\x96 t.me/+abc"), context, 100 * A::kDay);
		chat.compact(100 * A::kDay + 8 * A::kDay);
		Expect(tally, chat.rows().size() == 1, "eviction keeps row at exactly 8 days");
		chat.compact(100 * A::kDay + 8 * A::kDay + 1);
		Expect(tally, chat.rows().empty(), "eviction by max(date, seen) after 8 days");
	}
}

// ---- crafted blobs: every SPEC 8.3 rejection rule ----

struct BlobRow {
	std::uint32_t msgId = 0;
	std::int64_t album = 0;
	std::int64_t date = 1000;
	std::int64_t seen = 1000;
	std::uint64_t sender = 0;
	std::uint32_t flags = 0;
	std::vector<std::pair<std::uint8_t, std::uint64_t>> keys;
	std::vector<std::uint64_t> links;
	std::vector<std::uint64_t> pending;
};

struct BlobRecord {
	std::uint8_t kind = 1;
	std::int64_t stamp = 1000;
	std::uint64_t msgKey = 0;
	std::uint64_t author = 0;
	std::vector<std::pair<std::uint8_t, std::uint64_t>> keys;
	std::vector<std::uint64_t> links;
};

struct BlobHandle {
	std::uint64_t hash = 0;
	std::int64_t first = 1000;
	std::int64_t last = 1000;
	std::int64_t seen = 1000;
};

struct Blob {
	char magic[4] = { 'L', 'G', 'A', 'S' };
	std::uint16_t formatVersion = 3;
	std::uint16_t algoVersion = 3;
	std::uint32_t tablesVersion = T::kTablesVersionU32;
	std::uint32_t reserved = 0;
	A::Key checkKey = TestKey();
	std::int64_t tagChat = -42;
	std::uint32_t nextRid = 1;
	std::vector<BlobRow> rows;
	std::vector<BlobRecord> records;
	std::vector<BlobHandle> handles;
	bool overrideCounts = false;
	std::uint32_t rowCount = 0;
	std::uint32_t recordCount = 0;
	std::uint32_t handleCount = 0;
	std::size_t truncate = 0;
	std::size_t trailing = 0;
	bool breakBodyHash = false;
};

void Put(std::vector<unsigned char> &out, std::uint64_t value, int size) {
	for (auto i = 0; i != size; ++i) {
		out.push_back(static_cast<unsigned char>(value >> (8 * i)));
	}
}

std::vector<unsigned char> BuildBlob(const Blob &b) {
	const auto key = TestKey();
	auto body = std::vector<unsigned char>();
	for (const auto &r : b.rows) {
		Put(body, r.msgId, 4);
		Put(body, std::uint64_t(r.album), 8);
		Put(body, std::uint64_t(r.date), 8);
		Put(body, std::uint64_t(r.seen), 8);
		Put(body, r.sender, 8);
		Put(body, r.flags, 4);
		Put(body, r.keys.size(), 1);
		for (const auto &[kind, hash] : r.keys) {
			Put(body, kind, 1);
			Put(body, hash, 8);
		}
		Put(body, r.links.size(), 1);
		for (const auto h : r.links) {
			Put(body, h, 8);
		}
		Put(body, r.pending.size(), 1);
		for (const auto h : r.pending) {
			Put(body, h, 8);
		}
	}
	for (const auto &r : b.records) {
		Put(body, r.kind, 1);
		Put(body, std::uint64_t(r.stamp), 8);
		Put(body, r.msgKey, 8);
		Put(body, r.author, 8);
		Put(body, r.keys.size(), 1);
		for (const auto &[kind, hash] : r.keys) {
			Put(body, kind, 1);
			Put(body, hash, 8);
		}
		Put(body, r.links.size(), 1);
		for (const auto h : r.links) {
			Put(body, h, 8);
		}
	}
	for (const auto &h : b.handles) {
		Put(body, h.hash, 8);
		Put(body, std::uint64_t(h.first), 8);
		Put(body, std::uint64_t(h.last), 8);
		Put(body, std::uint64_t(h.seen), 8);
	}
	if (b.truncate) {
		body.resize(body.size() - std::min(b.truncate, body.size()));
	}
	for (std::size_t i = 0; i != b.trailing; ++i) {
		body.push_back(0);
	}
	auto out = std::vector<unsigned char>();
	out.insert(out.end(), b.magic, b.magic + 4);
	Put(out, b.formatVersion, 2);
	Put(out, b.algoVersion, 2);
	Put(out, b.tablesVersion, 4);
	Put(out, b.reserved, 4);
	Put(out, A::Hash(b.checkKey, "check", std::string_view("LGAS")), 8);
	Put(out, A::Hash(key, "chat", std::string_view(std::to_string(b.tagChat))), 8);
	Put(out, b.overrideCounts ? b.rowCount : b.rows.size(), 4);
	Put(out, b.overrideCounts ? b.recordCount : b.records.size(), 4);
	Put(out, b.overrideCounts ? b.handleCount : b.handles.size(), 4);
	Put(out, b.nextRid, 4);
	auto bodyHash = A::HashBytes(key, "body", body.data(), body.size());
	if (b.breakBodyHash) {
		bodyHash ^= 1;
	}
	Put(out, bodyHash, 8);
	out.insert(out.end(), body.begin(), body.end());
	return out;
}

Blob ValidBlob() {
	auto b = Blob();
	auto live = BlobRow();
	live.msgId = 7;
	live.sender = 0x1234;
	live.flags = A::Flag::kInvite | A::Flag::kHasText;
	live.keys = { { 3, 0xAAAA }, { 7, 0xFFFFFFFFFFFFFFF0ULL } };
	live.links = { 0x11 };
	live.pending = { 0x22 };
	auto anon = BlobRow();
	anon.msgId = 0;
	anon.sender = 0;
	anon.flags = A::Flag::kAnon | A::Flag::kHasText;
	anon.keys = { { 3, 0xAAAA } };
	b.rows = { live, anon };
	b.nextRid = 3;
	auto endorse = BlobRecord();
	endorse.kind = 1;
	endorse.msgKey = 0x99;
	endorse.author = 0x1234;
	endorse.keys = { { 6, 0x1 }, { 6, 0x2 }, { 9, 0x3 }, { 11, 0x4 } };
	endorse.links = { 0x11 };
	auto notAd = BlobRecord();
	notAd.kind = 2;
	notAd.keys = { { 3, 0xBBBB } };
	b.records = { endorse, notAd };
	b.handles = { { 0x10, 900, 1000, 1000 }, { 0xF000000000000000ULL, 500, 600, 1000 } };
	return b;
}

void CheckRejections() {
	auto &tally = Section("SPEC 8.3 rejection rules (crafted blobs)");
	const auto key = TestKey();
	const auto loads = [&](const std::vector<unsigned char> &bytes) {
		return A::Store::Deserialize(bytes.data(), bytes.size(), -42, key) != nullptr;
	};
	const auto accept = [&](const Blob &b, const std::string &what) {
		const auto bytes = BuildBlob(b);
		Expect(tally, loads(bytes), "accepts " + what);
	};
	const auto reject = [&](const Blob &b, const std::string &what) {
		Expect(tally, !loads(BuildBlob(b)), "rejects " + what);
	};
	{
		const auto bytes = BuildBlob(ValidBlob());
		const auto store = A::Store::Deserialize(bytes.data(), bytes.size(), -42, key);
		Expect(tally, store != nullptr, "accepts baseline blob");
		Expect(tally, store && store->serialize() == bytes, "baseline re-serializes byte-identically");
		auto reservedSet = ValidBlob();
		reservedSet.reserved = 7;
		accept(reservedSet, "reserved header field (not a SPEC 8.3 rule; reference ignores it)");
	}
	for (std::size_t length : { std::size_t(0), std::size_t(1), std::size_t(55) }) {
		auto bytes = BuildBlob(ValidBlob());
		bytes.resize(length);
		Expect(tally, !loads(bytes), "rejects length " + std::to_string(length) + " < 56");
	}
	{
		auto b = ValidBlob();
		b.magic[3] = 'T';
		reject(b, "magic");
	}
	for (const auto version : { 1, 2, 4 }) {
		auto b = ValidBlob();
		b.formatVersion = std::uint16_t(version);
		reject(b, "formatVersion " + std::to_string(version));
		b = ValidBlob();
		b.algoVersion = std::uint16_t(version);
		reject(b, "algoVersion " + std::to_string(version));
	}
	{
		auto b = ValidBlob();
		b.tablesVersion ^= 1;
		reject(b, "tablesVersion");
		b = ValidBlob();
		b.checkKey = FilledKey(9);
		reject(b, "keyCheck (other key)");
		b = ValidBlob();
		b.tagChat = -43;
		reject(b, "chatTag (other chat)");
		b = ValidBlob();
		b.breakBodyHash = true;
		reject(b, "bodyHash");
		const auto valid = BuildBlob(ValidBlob());
		Expect(tally, !A::Store::Deserialize(valid.data(), valid.size(), -42, FilledKey(9)), "rejects whole blob under another key");
	}
	{
		auto b = ValidBlob();
		b.rows.clear();
		b.records.clear();
		b.handles.clear();
		auto row = BlobRow();
		row.flags = A::Flag::kAnon;
		b.rows.assign(A::kMaxRows, row);
		b.nextRid = std::uint32_t(A::kMaxRows + 1);
		accept(b, "3000 rows");
		b.rows.push_back(row);
		reject(b, "3001 rows");
	}
	{
		auto b = ValidBlob();
		b.records.clear();
		auto endorse = BlobRecord();
		endorse.kind = 1;
		auto notAd = BlobRecord();
		notAd.kind = 2;
		b.records.assign(A::kMaxEndorseRecords, endorse);
		b.records.insert(b.records.end(), A::kMaxNotAdRecords, notAd);
		accept(b, "200 endorse + 200 not-ad records");
		auto more = b;
		more.records.push_back(endorse);
		reject(more, "401 records (201 endorse)");
		more = b;
		more.records.push_back(notAd);
		reject(more, "401 records (201 not-ad)");
		auto onlyEndorse = ValidBlob();
		onlyEndorse.records.assign(A::kMaxEndorseRecords + 1, endorse);
		reject(onlyEndorse, "201 endorse records");
		auto onlyNotAd = ValidBlob();
		onlyNotAd.records.assign(A::kMaxNotAdRecords + 1, notAd);
		reject(onlyNotAd, "201 not-ad records");
		auto counted = ValidBlob();
		counted.overrideCounts = true;
		counted.rowCount = 2;
		counted.recordCount = 401;
		counted.handleCount = 2;
		reject(counted, "header recordCount 401");
	}
	{
		auto b = ValidBlob();
		b.handles.clear();
		for (std::size_t i = 0; i != A::kMaxHandles; ++i) {
			b.handles.push_back({ i + 1, 10, 20, 30 });
		}
		accept(b, "2000 handles");
		b.handles.push_back({ A::kMaxHandles + 1, 10, 20, 30 });
		reject(b, "2001 handles");
	}
	const auto maxDate = A::kMaxDate;
	for (const auto bad : { std::int64_t(-1), maxDate + 1, std::numeric_limits<std::int64_t>::min() }) {
		const auto suffix = " = " + std::to_string(bad);
		auto b = ValidBlob();
		b.rows[0].date = bad;
		reject(b, "row date" + suffix);
		b = ValidBlob();
		b.rows[0].seen = bad;
		reject(b, "row seen" + suffix);
		b = ValidBlob();
		b.records[0].stamp = bad;
		reject(b, "record stamp" + suffix);
		b = ValidBlob();
		b.handles[0].first = bad;
		reject(b, "handle first" + suffix);
		b = ValidBlob();
		b.handles[0].last = bad;
		reject(b, "handle last" + suffix);
		b = ValidBlob();
		b.handles[0].seen = bad;
		reject(b, "handle seen" + suffix);
	}
	{
		auto b = ValidBlob();
		b.rows[0].date = maxDate;
		b.rows[0].seen = maxDate;
		b.records[0].stamp = maxDate;
		b.handles[0] = { 0x10, 0, maxDate, maxDate };
		accept(b, "dates at 0 and 2^40");
		b = ValidBlob();
		b.handles[0].first = 1001;
		b.handles[0].last = 1000;
		reject(b, "handle first > last");
	}
	{
		auto b = ValidBlob();
		b.rows[0].msgId = 0x7FFFFFFF;
		accept(b, "msgId 2^31-1");
		b.rows[0].msgId = 0x80000000U;
		reject(b, "msgId 2^31");
		b.rows[0].msgId = 0xFFFFFFFFU;
		reject(b, "msgId 2^32-1");
		b = ValidBlob();
		b.rows[0].flags |= A::Flag::kAnon;
		reject(b, "live row with ANON flag");
		b = ValidBlob();
		b.rows[1].flags &= ~A::Flag::kAnon;
		reject(b, "anonymous row without ANON flag");
		b = ValidBlob();
		b.rows[1].sender = 5;
		reject(b, "anonymous row with sender");
		b = ValidBlob();
		b.rows[1] = b.rows[0];
		reject(b, "duplicate msgId");
		b = ValidBlob();
		b.handles[1].hash = b.handles[0].hash;
		reject(b, "duplicate handle");
	}
	for (const auto bit : { 17, 31 }) {
		auto b = ValidBlob();
		b.rows[0].flags |= (1U << bit);
		reject(b, "flag bit " + std::to_string(bit));
	}
	{
		// v3: PRIV needs ENDORSING, PRIV_STRONG needs PRIV.
		auto b = ValidBlob();
		b.rows[0].flags |= A::Flag::kPriv;
		reject(b, "PRIV without ENDORSING");
		b.rows[0].flags |= A::Flag::kEndorsing;
		accept(b, "PRIV with ENDORSING");
		b.rows[0].flags |= A::Flag::kPrivStrong;
		accept(b, "PRIV_STRONG with PRIV");
		b = ValidBlob();
		b.rows[0].flags |= A::Flag::kPrivStrong | A::Flag::kEndorsing;
		reject(b, "PRIV_STRONG without PRIV");
		b = ValidBlob();
		b.records[1].author = 5;
		reject(b, "not-ad record with author");
		b = ValidBlob();
		b.records[1].msgKey = 5;
		reject(b, "not-ad record with msgKey");
		b = ValidBlob();
		b.records[0].author = 0;
		accept(b, "endorse record without author");
	}
	{
		auto b = ValidBlob();
		b.rows[0].keys.assign(A::kMaxRowKeys, { 3, 1 });
		accept(b, "row with 72 keys");
		b.rows[0].keys.push_back({ 3, 2 });
		reject(b, "row with 73 keys");
		b = ValidBlob();
		b.rows[0].links.assign(A::kMaxLinksPerRow, 1);
		accept(b, "row with 16 links");
		b.rows[0].links.push_back(2);
		reject(b, "row with 17 links");
		b = ValidBlob();
		b.rows[0].pending.assign(A::kMaxPendingPerRow, 1);
		accept(b, "row with 16 pending");
		b.rows[0].pending.push_back(2);
		reject(b, "row with 17 pending");
		b = ValidBlob();
		b.records[0].keys.assign(A::kMaxRecordKeys, { 6, 1 });
		b.records[0].links.assign(A::kMaxRecordLinks, 1);
		accept(b, "record with 128 keys and 64 links");
		auto more = b;
		more.records[0].keys.push_back({ 6, 2 });
		reject(more, "record with 129 keys");
		more = b;
		more.records[0].links.push_back(2);
		reject(more, "record with 65 links");
	}
	for (const auto kind : { 8, 9, 10, 11 }) {
		auto b = ValidBlob();
		b.rows[0].keys[0].first = std::uint8_t(kind);
		b.records[1].keys[0].first = std::uint8_t(kind);
		accept(b, "key kind " + std::to_string(kind));
	}
	for (const auto kind : { 0, 12, 255 }) {
		auto b = ValidBlob();
		b.rows[0].keys[0].first = std::uint8_t(kind);
		reject(b, "row key kind " + std::to_string(kind));
		b = ValidBlob();
		b.records[1].keys[0].first = std::uint8_t(kind);
		reject(b, "record key kind " + std::to_string(kind));
	}
	for (const auto kind : { 0, 3, 255 }) {
		auto b = ValidBlob();
		b.records[0].kind = std::uint8_t(kind);
		reject(b, "record kind " + std::to_string(kind));
	}
	for (const auto cut : { std::size_t(1), std::size_t(8), std::size_t(33), std::size_t(100) }) {
		auto b = ValidBlob();
		b.truncate = cut;
		reject(b, "truncated body by " + std::to_string(cut) + " (hash recomputed)");
	}
	for (const auto extra : { std::size_t(1), std::size_t(32) }) {
		auto b = ValidBlob();
		b.trailing = extra;
		reject(b, "trailing " + std::to_string(extra) + " bytes (hash recomputed)");
	}
	{
		auto b = ValidBlob();
		b.overrideCounts = true;
		b.rowCount = 1;
		b.recordCount = 2;
		b.handleCount = 2;
		reject(b, "header rowCount smaller than body");
		b.rowCount = 3;
		reject(b, "header rowCount larger than body");
	}
}

// ---- v3: edit-tolerant keys, privileged authors (ref TestV3 / TestV3Fixes) ----

using VarList = std::vector<std::pair<int, std::u32string>>;

A::MessageInput V3Message(
		std::int64_t id,
		std::int64_t date,
		std::int64_t sender,
		A::Role role,
		std::u32string text) {
	auto m = A::MessageInput();
	m.id = id;
	m.date = date;
	m.sender.id = sender;
	m.sender.role = role;
	m.text = std::move(text);
	return m;
}

std::uint64_t SenderKey(std::int64_t id) {
	return A::Hash(TestKey(), "sender", std::string_view(std::to_string(id)));
}

std::uint64_t MsgKey(std::int64_t id) {
	return A::Hash(TestKey(), "msg", std::string_view(std::to_string(id)));
}

A::ChatContext V3Context(std::int64_t chatId, std::int64_t linked) {
	auto data = A::ChatContextData();
	data.chatId = chatId;
	data.linkedChannelId = linked;
	return A::ChatContext(std::move(data));
}

void CheckV3Keys(Tally &tally) {
	const auto var = int(A::KeyKind::Var);
	const auto varL = int(A::KeyKind::VarL);
	const auto varS = int(A::KeyKind::VarS);
	{
		const auto lines = std::vector<std::u32string>{
			U"丝滑提交",
			U"限时特价云端会员年卡全网最低价格",
			U"有需要的请私聊我下单先到先得",
		};
		Expect(tally, A::VarKeys(lines) == VarList{
			{ var, lines[0] + lines[1] + lines[2] },
			{ varS, lines[1] + lines[2] },
		}, "VarKeys: VAR and a strict VARS (D=8, K=60)");
		auto loose = lines;
		loose[0] = U"一二三四五六七八九十";
		Expect(tally, A::VarKeys(loose) == VarList{
			{ var, loose[0] + loose[1] + loose[2] },
			{ varL, loose[1] + loose[2] },
		}, "VarKeys: wide VARL (D=20, K=60)");
		Expect(tally, A::VarKeys({ U"短" }).empty(), "VarKeys: nothing below 24");
		Expect(tally, A::VarKeys({ U"一二三四五六七八九十一二" }) == VarList{
			{ var, U"一二三四五六七八九十一二" },
		}, "VarKeys: full text at exactly 24");
		const auto dup = A::VarKeys({
			U"价目表如下请参考",
			U"#件起",
			U"#件起",
			U"详情请私聊客服人员",
		});
		auto payloads = std::set<std::u32string>();
		for (const auto &[kind, payload] : dup) {
			payloads.insert(payload);
		}
		Expect(tally, payloads.size() == dup.size(), "VarKeys: deduplicated by payload");
		auto many = std::vector<std::u32string>();
		for (auto i = 0; i != 50; ++i) {
			char buffer[8] = { 0 };
			std::snprintf(buffer, sizeof(buffer), "%02d", i);
			many.push_back(U"第" + U32(buffer) + U"行商品名称");
		}
		const auto v = A::VarKeys(many);
		auto kinds = std::vector<int>();
		auto set = std::set<std::u32string>();
		for (const auto &[kind, payload] : v) {
			kinds.push_back(kind);
			set.insert(payload);
		}
		auto expected = std::vector<int>(A::kMaxVar, varS);
		expected[0] = var;
		Expect(tally, v.size() == A::kMaxVar && kinds == expected, "VarKeys: 34 at most (full + 16 + 16 + both ends)");
		const auto without = [&](std::size_t skip) {
			auto result = std::u32string();
			for (std::size_t i = 0; i != many.size(); ++i) {
				if (i != skip) {
					result += many[i];
				}
			}
			return result;
		};
		Expect(tally, !set.count(without(25)) && set.count(without(15)) && set.count(without(34)), "VarKeys: leave-one only for the first and last 16 lines");
	}
	{
		const auto f = A::ExtractLinks(T::Fold(
			U"標題\n\n第一區塊的內容長度足夠\n第二行\n🔥🔥🔥\n第二區塊的內容長度也足夠\n\n短\n\n第一區塊的內容長度足夠\n第二行")).foldNoLinks;
		const auto blocks = A::Blocks(f);
		Expect(tally, blocks == std::vector<std::u32string>{
			U"标题",
			U"第一区块的内容长度足够第二行",
			U"第二区块的内容长度也足够",
			U"短",
			U"第一区块的内容长度足够第二行",
		}, "Blocks: separated by non-content lines");
		Expect(tally, A::BlockPayloads(blocks) == std::vector<std::u32string>{
			U"第一区块的内容长度足够第二行",
			U"第二区块的内容长度也足够",
		}, "BlockPayloads: wlen >= 16, deduplicated");
		Expect(tally, A::BlockPayloads({ U"第一区块的内容长度足够第二行", U"短" }).empty(), "BlockPayloads: none with fewer than 2");
		Expect(tally, A::ContentLines(f) == std::vector<std::u32string>{
			U"标题",
			U"第一区块的内容长度足够",
			U"第二行",
			U"第二区块的内容长度也足够",
			U"短",
			U"第一区块的内容长度足够",
			U"第二行",
		}, "ContentLines");
	}
	{
		const auto context = PlainContext(-1);
		auto lf = std::u32string(U"雲端會員限時調價\n企業採購請私聊\n\n雲端硬碟會員年卡特價\n季卡特價月卡特價");
		auto crlf = std::u32string();
		for (const auto c : lf) {
			if (c == U'\n') {
				crlf.push_back(U'\r');
			}
			crlf.push_back(c);
		}
		const auto a = A::Fingerprint(V3Message(1, 1, 5, A::Role::Member, lf), context, TestKey());
		const auto b = A::Fingerprint(V3Message(1, 1, 5, A::Role::Member, crlf), context, TestKey());
		auto same = (a.fold == b.fold) && (a.blocks == b.blocks) && (a.flags == b.flags) && (a.keys.size() == b.keys.size());
		for (std::size_t i = 0; same && i != a.keys.size(); ++i) {
			same = (a.keys[i].kind == b.keys[i].kind) && (a.keys[i].hash == b.keys[i].hash);
		}
		Expect(tally, same, "CR LF collapsed before fold");
		Expect(tally, A::CollapseCrlf(U"a\r\r\nb\rc\n\r") == U"a\r\nb\rc\n\r", "CollapseCrlf: only a CR right before LF, one pass");
		Expect(tally, A::Blocks(T::Fold(U"一 二\n \t\n三\n🔥\n四")) == std::vector<std::u32string>{ U"一二", U"三", U"四" }, "Blocks: U+2028 is a line break, blank and emoji lines separate");
	}
	{
		// Row key cap: 6 + 16 PARA + 34 VAR family + 16 BLOCK = 72, in order.
		auto text = std::u32string();
		const auto digits = std::u32string(U"零一二三四五六七八九");
		const auto stems = std::u32string(U"甲乙丙丁戊");
		for (auto i = 0; i != 40; ++i) {
			text += U"第" + digits.substr(std::size_t(i % 10), 1)
				+ U"段是一行足夠長的內容，用來產生段落鍵與區塊鍵，順便測試上限"
				+ stems.substr(std::size_t(i / 10), 1) + U"\n\n";
		}
		text += U"客服電話 0912-345-678 有需要私聊";
		auto m = V3Message(1, 1000, 5, A::Role::Member, text);
		m.media.kind = A::MediaKind::Photo;
		m.media.id = "-42";
		auto forward = A::Forward();
		forward.fromKind = A::ForwardKind::User;
		forward.fromId = 77;
		forward.date = 900;
		m.forward = forward;
		const auto context = PlainContext(-1);
		const auto fp = A::Fingerprint(m, context, TestKey());
		auto para = 0;
		auto vars = 0;
		auto blocks = 0;
		auto ordered = true;
		const auto rank = [](A::KeyKind kind) {
			switch (kind) {
			case A::KeyKind::Fwd: return 0;
			case A::KeyKind::Media: return 1;
			case A::KeyKind::Text: return 2;
			case A::KeyKind::Cjk: return 3;
			case A::KeyKind::Tmpl: return 4;
			case A::KeyKind::Contact: return 5;
			case A::KeyKind::Para: return 6;
			case A::KeyKind::Var:
			case A::KeyKind::VarL:
			case A::KeyKind::VarS: return 7;
			case A::KeyKind::Block: return 8;
			}
			return 9;
		};
		for (std::size_t i = 0; i != fp.keys.size(); ++i) {
			const auto kind = fp.keys[i].kind;
			para += (kind == A::KeyKind::Para) ? 1 : 0;
			vars += (rank(kind) == 7) ? 1 : 0;
			blocks += (kind == A::KeyKind::Block) ? 1 : 0;
			if (i > 0 && rank(fp.keys[i - 1].kind) > rank(kind)) {
				ordered = false;
			}
		}
		Expect(tally, fp.keys.size() == A::kMaxRowKeys && para == 16 && vars == 34 && blocks == 16, "row reaches the 72 key cap");
		Expect(tally, ordered, "key order FWD MEDIA TEXT CJK TMPL CONTACT PARA VAR* BLOCK");
		auto chat = A::Store(-1, TestKey());
		Observe(chat, m, context, 2000);
		const auto blob = chat.serialize();
		const auto back = A::Store::Deserialize(blob.data(), blob.size(), -1, TestKey());
		Expect(tally, back && back->serialize() == blob, "72 key row serializes and loads back");
	}
}

void CheckV3Privileged(Tally &tally, const Json &data) {
	const auto key = TestKey();
	{
		struct Case {
			std::int64_t sender = 0;
			A::Role role = A::Role::Member;
			bool pinned = false;
			bool autoForward = false;
			int forward = 0; // 0 none, 1 user, 2 linked channel, 3 other channel
			bool endorsing = false;
			bool privileged = false;
			const char *what = "";
		};
		const auto context = V3Context(-100, -200);
		const auto cases = std::vector<Case>{
			{ 1, A::Role::Admin, false, false, 0, true, true, "admin" },
			{ 1, A::Role::Creator, false, false, 0, true, true, "creator" },
			{ -100, A::Role::AnonAdmin, false, false, 0, true, true, "anonymous admin" },
			{ -200, A::Role::Channel, false, false, 0, true, true, "the linked channel" },
			{ -300, A::Role::Channel, false, false, 0, false, false, "another channel" },
			{ -200, A::Role::Channel, false, true, 0, true, true, "automatic forward" },
			{ 1, A::Role::Self, false, false, 0, true, false, "self" },
			{ 1, A::Role::Member, true, false, 0, true, false, "pinned member message" },
			{ 1, A::Role::Member, false, false, 2, true, false, "member forwards the linked channel" },
			{ 1, A::Role::Admin, false, false, 1, false, false, "admin forwards a user" },
			{ 1, A::Role::Admin, false, false, 3, true, true, "admin forwards another channel" },
			{ -200, A::Role::Self, false, false, 0, true, false, "self as the linked channel" },
			{ -100, A::Role::Self, false, false, 0, true, false, "self as anonymous admin" },
		};
		for (const auto &c : cases) {
			auto m = V3Message(1, 1, c.sender, c.role, U"本群公告：雲端會員特價，私聊客服");
			m.pinned = c.pinned;
			m.autoForward = c.autoForward;
			if (c.forward) {
				auto forward = A::Forward();
				forward.fromKind = (c.forward == 1) ? A::ForwardKind::User : A::ForwardKind::Channel;
				forward.fromId = (c.forward == 1) ? 5 : (c.forward == 2) ? -200 : -900;
				forward.postId = (c.forward == 1) ? 0 : 5;
				forward.date = 1;
				m.forward = forward;
			}
			const auto fp = A::Fingerprint(m, context, key);
			Expect(
				tally,
				fp.endorsing == c.endorsing
					&& fp.privileged == c.privileged
					&& ((fp.flags & A::Flag::kPriv) != 0) == c.privileged,
				std::string("privileged definition: ") + c.what);
		}
	}
	{
		auto d = A::ChatContextData();
		d.chatId = -100;
		d.username = U"grp_demo";
		d.linkedChannelId = -200;
		d.linkedChannelUsername = U"ch_demo";
		d.description = U"官方客服 @desk_demo_helper";
		d.pinnedTexts = { U"最新價目 私聊 @seller_demo_a 交流群 t.me/+PinInvDemo" };
		const auto context = A::ChatContext(std::move(d));
		const auto text = std::u32string(U"最新價目 有需要聯繫 @seller_demo_a 交流群 t.me/+PinInvDemo");
		const auto channelForward = [](std::int64_t fromId) {
			auto forward = A::Forward();
			forward.fromKind = A::ForwardKind::Channel;
			forward.fromId = fromId;
			forward.postId = 9;
			forward.username = U"ch_demo";
			forward.date = 1;
			return forward;
		};
		const auto fp = [&](std::int64_t sender, A::Role role, std::int64_t forwardFrom, std::u32string body) {
			auto m = V3Message(1, 1, sender, role, std::move(body));
			if (forwardFrom) {
				m.forward = channelForward(forwardFrom);
			}
			return A::Fingerprint(m, context, key);
		};
		const auto admin = fp(7, A::Role::Admin, 0, text);
		Expect(tally, A::StrongNames(admin.flags).empty() && (admin.flags & A::Flag::kPrivStrong) && admin.senderKey == SenderKey(7), "PRIV_STRONG: pinned-only handle and invite of an admin");
		const auto member = fp(8, A::Role::Member, 0, text);
		Expect(tally, !(member.flags & (A::Flag::kPriv | A::Flag::kPrivStrong)) && member.senderKey == SenderKey(8), "member: no PRIV bits");
		Expect(tally, fp(7, A::Role::Admin, -200, text).senderKey == SenderKey(-200), "admin forwarding the linked channel: author is the channel");
		Expect(tally, fp(8, A::Role::Member, -200, text).senderKey == SenderKey(8), "member forwarding the linked channel: author is the member");
		Expect(tally, fp(7, A::Role::Admin, -900, text).senderKey == SenderKey(7), "admin forwarding another channel: author is the admin");
		auto automatic = V3Message(2, 1, -200, A::Role::Channel, U"新品上架 私訊 @other_seller_demo");
		automatic.autoForward = true;
		automatic.forward = channelForward(-200);
		Expect(tally, (A::Fingerprint(automatic, context, key).flags & A::Flag::kPrivStrong) != 0, "automatic forward: demoted handle is PRIV_STRONG");
		const auto description = fp(7, A::Role::Admin, 0, U"有問題找 @desk_demo_helper 或看 @ch_demo");
		Expect(tally, !(description.flags & A::Flag::kPrivStrong), "description tokens and own usernames are not PRIV_STRONG");
	}
	{
		auto chat = A::Store(-1, key);
		const auto context = V3Context(-1, -100777);
		Observe(chat, V3Message(1, 1000, 9, A::Role::Admin, U"本群合作推廣：雲端會員特價，私聊管理員"), context, 2000);
		auto pinned = V3Message(2, 1001, 8, A::Role::Member, U"本群公告：雲端會員特價，私聊管理員");
		pinned.pinned = true;
		Observe(chat, pinned, context, 2000);
		Observe(chat, V3Message(3, 1002, -100777, A::Role::Channel, U"頻道公告：私聊客服"), context, 2000);
		Observe(chat, V3Message(4, 1003, 10, A::Role::Self, U"我的公告：私聊我報名"), context, 2000);
		auto authors = std::map<std::uint64_t, std::uint64_t>();
		for (const auto &record : chat.records()) {
			authors[record.msgKey] = record.author;
		}
		Expect(tally, authors == std::map<std::uint64_t, std::uint64_t>{
			{ MsgKey(1), SenderKey(9) },
			{ MsgKey(2), 0 },
			{ MsgKey(3), SenderKey(-100777) },
			{ MsgKey(4), 0 },
		}, "endorsement record authors");
		const auto blob = chat.serialize();
		const auto back = A::Store::Deserialize(blob.data(), blob.size(), -1, key);
		auto same = back && back->records().size() == chat.records().size();
		for (std::size_t i = 0; same && i != chat.records().size(); ++i) {
			same = back->records()[i].author == chat.records()[i].author
				&& back->records()[i].msgKey == chat.records()[i].msgKey;
		}
		Expect(tally, same, "record authors round-trip");
		Expect(tally, blob.size() > 8 && blob[4] == 3 && blob[5] == 0 && blob[6] == 3 && blob[7] == 0, "header formatVersion 3, algoVersion 3");
		auto old = blob;
		old[4] = 2;
		Expect(tally, !A::Store::Deserialize(old.data(), old.size(), -1, key), "a v2 file is discarded");
	}
	{
		// Members re-post an admin announcement, each adding a line with
		// their own link: only edited overlap, never an impersonation (P1).
		const auto context = PlainContext(-1);
		auto chat = A::Store(-1, key);
		const auto announcement = std::u32string(U"本週六下午兩點社區讀書會\n地點在市立圖書館三樓會議室\n歡迎大家帶朋友一起參加");
		Observe(chat, V3Message(1, 1000, 1, A::Role::Admin, announcement), context, 9000);
		Observe(chat, V3Message(2, 2000, 2, A::Role::Member, announcement + U"\n我整理的書單 https://notes-alpha.example/l"), context, 9000);
		Observe(chat, V3Message(3, 3000, 3, A::Role::Member, announcement + U"\n我整理的書單 https://notes-beta.example/l"), context, 9000);
		Observe(chat, V3Message(4, 4000, 4, A::Role::Member, announcement + U"\n讀書會加油"), context, 9000);
		auto ok = true;
		auto fourth = A::State::NotTriggered;
		for (const auto sensitivity : kSensitivities) {
			auto settings = A::Settings();
			settings.sensitivity = sensitivity;
			for (const auto id : { 2, 3, 4 }) {
				const auto d = chat.decide(id, context, settings, 9000);
				ok = ok && !d.fold && (id == 4 || d.cls == A::DecisionClass::Weak);
				if (id == 4) {
					fourth = d.state;
				}
			}
		}
		Expect(tally, ok && fourth == A::State::Endorsed, "edited overlap with a record protects, never impersonates");
	}
	{
		// Group membership of the VAR rules (ref test_var_matching_rules).
		const auto context = PlainContext(-1);
		const auto groupSize = [&](const std::vector<std::u32string> &texts) {
			auto chat = A::Store(-1, key);
			for (std::size_t i = 0; i != texts.size(); ++i) {
				Observe(chat, V3Message(std::int64_t(i + 1), 1000 + std::int64_t(i), 5, A::Role::Member, texts[i]), context, 2000);
			}
			return chat.decide(1, context, A::Settings(), 2000).groupSize;
		};
		const auto head = std::u32string(U"【社區公告】本週社區活動與重要事項");
		const auto tail = std::u32string(U"如有疑問請私聊管理員\n感謝各位鄰居配合");
		Expect(tally, groupSize({ head + U"\n週六上午九點大樓消防演練\n" + tail, head + U"\n週日下午兩點社區二手市集\n" + tail }) == 1, "VAR: replacing a long line does not link (P2a)");
		Expect(tally, groupSize({ head + U"\n" + tail, head + U"\n週六上午九點大樓消防演練\n" + tail }) == 2, "VAR: one line added links");
		const auto longText = std::u32string(U"本店雲端會員年卡限時特價全網最低價格\n季卡月卡全部現貨下單後五分鐘內開通\n企業採購量大可議歡迎私聊客服詢價\n售後保固一年有任何問題隨時找我們");
		Expect(tally, groupSize({ longText + U"\n今日特價", longText + U"\n明日特價" }) == 2, "VAR: replacing a short line links (both strict)");
	}
	{
		// The option changes nothing for a non-privileged message, and with
		// the option off a privileged message is always endorsed.
		auto checkedPrivileged = 0;
		auto same = true;
		auto endorsed = true;
		for (const auto &sc : data["scenarios"].array) {
			auto account = A::Account(424242, key);
			auto context = std::make_unique<A::ChatContext>(ContextFrom(sc["context"]));
			const auto chat = account.chat(context->chatId());
			auto seen = std::vector<std::int64_t>();
			auto now = std::int64_t(0);
			for (const auto &ev : sc["events"].array) {
				const auto op = Narrow(ev["op"]);
				if (op == "observe") {
					const auto &m = ev["message"];
					now = ev.has("now") ? ev["now"].asInt() : std::max(now, m["date"].asInt());
					Observe(*chat, MessageFrom(m), *context, now);
					if (std::find(seen.begin(), seen.end(), m["id"].asInt()) == seen.end()) {
						seen.push_back(m["id"].asInt());
					}
				} else if (op == "delete") {
					now = ev.has("now") ? ev["now"].asInt() : now;
					auto ids = std::vector<std::int64_t>();
					for (const auto &id : ev["ids"].array) {
						ids.push_back(id.asInt());
					}
					(void)chat->remove(ids, now);
				} else if (op == "notAd") {
					now = ev.has("now") ? ev["now"].asInt() : now;
					(void)chat->markNotAd(ev["id"].asInt(), *context, now);
				} else if (op == "context") {
					context = std::make_unique<A::ChatContext>(ContextFrom(ev["context"]));
				} else if (op == "expect") {
					now = ev.has("now") ? ev["now"].asInt() : now;
					for (const auto id : seen) {
						auto privileged = false;
						for (const auto &[rid, row] : chat->rows()) {
							if (row.msgId == std::uint32_t(id)) {
								privileged = (row.flags & A::Flag::kPriv) != 0;
							}
						}
						for (const auto sensitivity : kSensitivities) {
							auto off = SettingsFrom(sc["settings"], sensitivity);
							off.foldPrivilegedRepeats = false;
							auto on = off;
							on.foldPrivilegedRepeats = true;
							const auto a = chat->decide(id, *context, off, now);
							const auto b = chat->decide(id, *context, on, now);
							if (privileged) {
								++checkedPrivileged;
								if (context->adminsKnown() && context->eligible()) {
									endorsed = endorsed && (a.state == A::State::Endorsed);
								}
							} else {
								same = same && (a == b);
							}
						}
					}
				}
			}
		}
		Expect(tally, same, "foldPrivilegedRepeats changes nothing for non-privileged messages");
		Expect(tally, endorsed, "foldPrivilegedRepeats off: privileged messages are endorsed");
		Expect(tally, checkedPrivileged > 50, "privileged decisions checked: " + std::to_string(checkedPrivileged));
	}
}

void CheckV3(const Json &data) {
	auto &tally = Section("v3 unit checks (ref TestV3/TestV3Fixes)");
	CheckV3Keys(tally);
	CheckV3Privileged(tally, data);
}

// ---- account: forgetAll, orphans, panic wipe, total cap ----

void ObserveSimple(
		A::Store &chat,
		std::int64_t chatId,
		const std::string &text) {
	const auto context = PlainContext(chatId);
	Observe(chat, SimpleMessage(1, 1000, 5, text), context, 2000);
}

void CheckAccount(const Json &data) {
	auto &tally = Section("account / forgetAll / orphans / cap");
	const auto key = TestKey();
	const auto invite = std::string("\xe7\xa7\x81\xe8\x81\x8a\xe6\x88\x91 t.me/+abc");
	{
		auto fs = MemoryFileSystem();
		fs.makeDirectories("/d");
		auto account = A::Account(777, key, &fs, "/d");
		auto f = FilledChat(data, account);
		Expect(tally, account.flush(), "flush writes");
		const auto directory = "/d/" + account.directoryName();
		Expect(tally, !fs.names(directory).empty(), "account directory has files");
		for (const auto &name : fs.names(directory)) {
			Expect(tally, name.find("1000000001") == std::string::npos, "file name hides chat id");
		}
		account.forgetAll();
		Expect(tally, !fs.exists(directory), "forgetAll removes account directory");
		Expect(tally, !account.flush(), "flush refused after forgetAll");
		Expect(tally, !fs.exists(directory), "late flush does not recreate files");
		const auto &first = (*f.scenario)["events"].array[0]["message"];
		Expect(tally, f.chat->observe(MessageFrom(first), *f.context, f.now) == A::ObserveResult::Refused, "observe refused after forgetAll (held store)");
		Expect(tally, !f.chat->remove({ 1 }, f.now), "delete refused after forgetAll");
		Expect(tally, f.chat->markNotAd(1, *f.context, f.now) == A::MarkResult::Refused, "notAd refused after forgetAll");
		Expect(tally, account.chat(f.context->chatId()) == nullptr, "chat() refused after forgetAll");
		Expect(tally, !fs.exists(directory), "nothing written after refusals");
	}
	{
		auto a = A::Account(111, key);
		auto b = A::Account(222, key);
		Expect(tally, a.directoryName() != b.directoryName(), "per-account directory by user id");
		Expect(tally, a.directoryName().find("111") == std::string::npos, "directory name hides user id");
	}
	{
		auto fs = MemoryFileSystem();
		fs.makeDirectories("/d");
		auto account = A::Account(9, key, &fs, "/d");
		auto f = FilledChat(data, account);
		Expect(tally, account.flush(), "flush for reload");
		auto again = A::Account(9, key, &fs, "/d");
		const auto chat = again.chat(f.context->chatId());
		Expect(tally, chat && chat->serialize() == f.chat->serialize(), "reload from disk identical");
	}
	{
		auto fs = MemoryFileSystem();
		fs.makeDirectories("/d");
		auto old = A::Account(777, FilledKey(1), &fs, "/d");
		ObserveSimple(*old.chat(-1), -1, invite);
		Expect(tally, old.flush(), "old key flush");
		fs.putFile("/d/" + old.directoryName() + "/junk.tmp", { 'x' });
		auto fresh = A::Account(777, key, &fs, "/d");
		auto other = A::Account(888, key, &fs, "/d");
		ObserveSimple(*other.chat(-2), -2, invite);
		Expect(tally, other.flush(), "other account flush");
		Expect(tally, fresh.directoryName() != old.directoryName(), "key change changes directory name");
		const auto removed = A::PurgeOrphans(fs, "/d", key);
		Expect(tally, removed == 2, "purgeOrphans removed stale chat file and temp file");
		Expect(tally, fs.names("/d") == std::vector<std::string>{ other.directoryName() }, "purgeOrphans keeps only current-key accounts");
	}
	{
		auto fs = MemoryFileSystem();
		fs.makeDirectories("/d");
		const auto root = std::string("/d/lumina_adshield");
		auto old = A::Account(1, FilledKey(2), &fs, root);
		ObserveSimple(*old.chat(-1), -1, invite);
		Expect(tally, old.flush(), "old key flush (logout test)");
		auto a = A::Account(1, key, &fs, root);
		auto b = A::Account(2, key, &fs, root);
		for (auto *account : { &a, &b }) {
			ObserveSimple(*account->chat(-1), -1, invite);
			Expect(tally, account->flush(), "flush (logout test)");
		}
		a.forgetAll();
		Expect(tally, fs.names(root) == std::vector<std::string>{ b.directoryName() }, "logout removes own directory and orphans only");
		b.forgetAll(true);
		Expect(tally, !fs.exists(root), "panic wipe removes the whole root");
		Expect(tally, !b.flush(), "flush refused after panic wipe");
		Expect(tally, !fs.exists(root), "root stays gone");
	}
	{
		auto fs = MemoryFileSystem();
		fs.makeDirectories("/d");
		auto account = A::Account(1, key, &fs, "/d");
		for (auto i = 0; i != 4; ++i) {
			ObserveSimple(*account.chat(-i - 1), -i - 1, invite + std::to_string(i));
		}
		Expect(tally, account.flush(), "flush (cap test)");
		const auto directory = "/d/" + account.directoryName();
		const auto files = fs.names(directory);
		for (std::size_t k = 0; k != files.size(); ++k) {
			fs.setModified(directory + "/" + files[k], 1000000000000000000LL + std::int64_t(k));
		}
		auto size = std::int64_t(0);
		auto modified = std::int64_t(0);
		Expect(tally, fs.info(directory + "/" + files[0], size, modified), "stat chat file");
		Expect(tally, A::EnforceTotalCap(fs, "/d", 2 * size) == 2, "total cap removes two oldest");
		Expect(tally, fs.names(directory) == std::vector<std::string>(files.begin() + 2, files.end()), "total cap keeps newest by mtime");
	}
	{
		auto fs = MemoryFileSystem();
		fs.makeDirectories("/d");
		auto account = A::Account(5, key, &fs, "/d");
		Expect(tally, !account.forgotten() && account.loadedChats().empty(), "account idle");
		Expect(tally, fs.names("/d").empty() && fs.writes == 0, "disabled feature creates nothing");
	}
}

// ---- tables sanity (generated header) ----

void CheckTables() {
	auto &tally = Section("tables sanity");
	auto idempotent = true;
	for (const auto &entry : T::kFold) {
		const auto out = std::u32string(T::kFoldPool + entry.offset, entry.length);
		if (T::Fold(out) != out || T::Fold(std::u32string(1, entry.key)) != out) {
			idempotent = false;
		}
		for (const auto c : out) {
			if (T::IsDrop(c)) {
				idempotent = false;
			}
		}
	}
	Expect(tally, idempotent, "fold idempotent and never outputs DROP");
	Expect(tally, T::Fold(U"ㄱㅏ") == U"가", "hangul compose L+V");
	Expect(tally, T::Fold(U"가ㄳ") == U"갃", "hangul compose LV+T");
	Expect(tally, T::Fold(U"ㄱㅏㄳ") == U"갃", "hangul compose L+V+T");
	Expect(tally, T::Fold(U"ㅋㅋㅋ") == std::u32string(3, U'ᄏ'), "hangul jamo kept");
	for (const auto cp : { 0x3164, 0x115F, 0x1160, 0xFFA0, 0x200B, 0x200C, 0x200D, 0x200E, 0x200F, 0x202A, 0x202E,
			0x2060, 0x2066, 0xFEFF, 0x00AD, 0x034F, 0x180E, 0xFE0F, 0x20E3, 0xE0067, 0xE0100, 0x2800 }) {
		auto input = std::u32string(U"a");
		input.push_back(char32_t(cp));
		input.push_back(U'b');
		Expect(tally, T::Fold(input) == U"ab", "invisible dropped " + ScalarHex(char32_t(cp)));
	}
}

// ---- performance ----

double Milliseconds(std::chrono::steady_clock::duration d) {
	return std::chrono::duration<double, std::milli>(d).count();
}

void CheckRowCapAndPerformance() {
	auto &tally = Section("row cap 3000 + timing");
	const auto key = TestKey();
	const auto context = PlainContext(-1);
	auto chat = A::Store(-1, key);
	const auto now = std::int64_t(2000000000);
	const auto started = std::chrono::steady_clock::now();
	const auto total = 3005;
	for (auto i = 1; i <= total; ++i) {
		auto m = SimpleMessage(
			i,
			now - 3 * A::kDay + i * 60,
			i % 37,
			"\xe7\xa7\x81\xe8\x81\x8a\xe6\x88\x91 \xe7\xac\xac" + std::to_string(i) + "\xe8\x99\x9f t.me/+x" + std::to_string(i));
		Observe(chat, m, context, now);
	}
	const auto observed = std::chrono::steady_clock::now();
	Expect(tally, chat.rows().size() == A::kMaxRows, "row cap keeps 3000 rows");
	auto ids = std::vector<std::uint32_t>();
	for (const auto &[rid, row] : chat.rows()) {
		ids.push_back(row.msgId);
	}
	Expect(tally, !ids.empty() && ids.front() == 6 && ids.back() == std::uint32_t(total), "row cap evicts oldest (max(date,seen), rid)");

	auto settings = A::Settings();
	const auto coldStart = std::chrono::steady_clock::now();
	const auto cold = chat.decide(total, context, settings, now);
	const auto coldEnd = std::chrono::steady_clock::now();
	auto folded = 0;
	const auto allStart = std::chrono::steady_clock::now();
	for (auto i = 6; i <= total; ++i) {
		folded += chat.decide(i, context, settings, now).fold ? 1 : 0;
	}
	const auto allEnd = std::chrono::steady_clock::now();
	Expect(tally, cold.state == A::State::Fold && folded > 2900, "3000-row flood folds");

	auto mixed = A::Store(-2, key);
	const auto mixedContext = PlainContext(-2);
	for (auto i = 1; i <= 3000; ++i) {
		const auto cluster = i % 300;
		const auto tag = std::string{ char('a' + cluster % 26), char('a' + cluster / 26) };
		auto m = SimpleMessage(
			i,
			now - 6 * A::kDay + i * 120,
			1000 + (i % 97),
			"\xe5\x84\xaa\xe6\x83\xa0\xe6\xb4\xbb\xe5\x8b\x95" + tag
				+ "\xe5\xa0\xb4 \xe5\x8a\xa0V\xef\xbc\x9a" + "deal_" + tag + "9");
		Observe(mixed, m, mixedContext, now);
	}
	const auto mixedCold = std::chrono::steady_clock::now();
	const auto first = mixed.decide(3000, mixedContext, settings, now);
	const auto mixedColdEnd = std::chrono::steady_clock::now();
	auto mixedFolded = 0;
	for (auto i = 1; i <= 3000; ++i) {
		mixedFolded += mixed.decide(i, mixedContext, settings, now).fold ? 1 : 0;
	}
	const auto mixedAllEnd = std::chrono::steady_clock::now();
	auto clusters = std::set<int>();
	for (auto i = 2991; i <= 3000; ++i) {
		clusters.insert(mixed.decide(i, mixedContext, settings, now).groupSize);
	}
	Expect(tally, first.state == A::State::Fold && mixedFolded > 2000, "mixed chat folds repeated contact ads");
	Expect(tally, clusters == std::set<int>{ 10 }, "mixed chat clusters have 10 units each");

	// v3: the linked channel floods a 14-line price list, each copy with its
	// own short first line (linked only by the VAR family and BLOCK keys;
	// about 30 keys a row). Off: all endorsed; on: all folded.
	const auto linked = std::int64_t(-1002000000002);
	auto priceData = A::ChatContextData();
	priceData.chatId = -3;
	priceData.linkedChannelId = linked;
	const auto priceContext = A::ChatContext(std::move(priceData));
	auto prices = A::Store(-3, key);
	// Digits fold to '#', so the lines and the first lines differ by
	// ideographs: 12 different price lines, 900 different first lines.
	const auto stems = std::u32string(U"甲乙丙丁戊己庚辛壬癸子丑寅卯辰巳午未申酉戌亥天地玄黄宇宙洪荒");
	auto list = std::u32string();
	for (std::size_t line = 0; line != 12; ++line) {
		list += U"\n" + std::u32string(line % 4 ? U"" : U"\n") + U"云端会员 "
			+ stems.substr(line, 1) + U"档 特价" + U32(std::to_string(100 + line * 7)) + U"元";
	}
	list += U"\n\n下单私聊 @shop_demo_flood\n售后 t.me/+FloodDemoInv";
	const auto priceStarted = std::chrono::steady_clock::now();
	for (auto i = 1; i <= 3000; ++i) {
		auto m = SimpleMessage(i, now - 6 * A::kDay + i * 120, 0, "");
		m.sender.id = linked;
		m.sender.role = A::Role::Channel;
		m.text = stems.substr(std::size_t(i % 30), 1)
			+ stems.substr(std::size_t((i / 30) % 30), 1) + U"提交：\n" + list;
		Observe(prices, m, priceContext, now);
	}
	const auto priceObserved = std::chrono::steady_clock::now();
	auto keys = std::size_t(0);
	for (const auto &[rid, row] : prices.rows()) {
		keys += row.keys.size();
	}
	auto on = A::Settings();
	on.foldPrivilegedRepeats = true;
	auto onFolded = 0;
	for (auto i = 1; i <= 3000; ++i) {
		onFolded += prices.decide(i, priceContext, on, now).fold ? 1 : 0;
	}
	const auto priceDecided = std::chrono::steady_clock::now();
	auto offEndorsed = 0;
	for (auto i = 1; i <= 3000; ++i) {
		offEndorsed += (prices.decide(i, priceContext, settings, now).state == A::State::Endorsed) ? 1 : 0;
	}
	const auto priceBlob = prices.serialize();
	Expect(tally, onFolded == 3000 && offEndorsed == 3000, "v3 channel price-list flood: off endorsed, on folded");

	std::printf(
		"  timing: observe 3005 msgs %.1f ms (%.1f us/msg)\n"
		"          flood (1 cluster x 3000): decide cold %.2f ms, decide x3000 %.1f ms (%.1f us each)\n"
		"          mixed (300 clusters x 10): decide cold %.2f ms, decide x3000 %.1f ms (%.1f us each)\n"
		"          v3 price-list flood (3000 rows, %.1f keys a row, file %zu bytes): observe %.1f ms, decide x3000 (option on) %.1f ms (%.1f us each)\n",
		Milliseconds(observed - started),
		1000. * Milliseconds(observed - started) / total,
		Milliseconds(coldEnd - coldStart),
		Milliseconds(allEnd - allStart),
		1000. * Milliseconds(allEnd - allStart) / (total - 5),
		Milliseconds(mixedColdEnd - mixedCold),
		Milliseconds(mixedAllEnd - mixedColdEnd),
		1000. * Milliseconds(mixedAllEnd - mixedColdEnd) / 3000,
		double(keys) / double(std::max<std::size_t>(prices.rows().size(), 1)),
		priceBlob.size(),
		Milliseconds(priceObserved - priceStarted),
		Milliseconds(priceDecided - priceObserved),
		1000. * Milliseconds(priceDecided - priceObserved) / 3000);
}

} // namespace

int main(int argc, char **argv) {
	const auto dir = std::string(argc > 1 ? argv[1] : ".");
	const auto scenarios = LoadJson(dir + "/corpus/scenarios.json");
	std::printf("AdShield desktop checker, tables %s, algo %d, format %d\n",
		std::string(T::kTablesVersion).c_str(),
		A::kAlgoVersion,
		A::kFormatVersion);
	CheckNormalize(dir);
	CheckTsv(dir, false);
	CheckTsv(dir, true);
	CheckSipHash(dir);
	CheckFingerprint(dir);
	CheckScenarios(dir, scenarios);
	CheckLoadOrder(scenarios);
	CheckExtraction(dir);
	CheckFuzzFingerprint(dir);
	CheckFuzzStore(dir);
	CheckStorage(scenarios);
	CheckRejections();
	CheckV3(scenarios);
	CheckAccount(scenarios);
	CheckTables();
	CheckRowCapAndPerformance();
	Expect(Section("harness"), Unexpected == 0, "no unexpected refusals");
	auto failed = 0;
	auto passed = 0;
	for (const auto &tally : Tallies) {
		std::printf("  %-48s %5d passed, %d failed\n", tally.name.c_str(), tally.passed, tally.failed);
		failed += tally.failed;
		passed += tally.passed;
	}
	std::printf("TOTAL %d passed, %d failed\n", passed, failed);
	return failed == 0 ? 0 : 1;
}
