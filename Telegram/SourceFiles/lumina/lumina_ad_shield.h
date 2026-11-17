/*
This file is part of LuminaGram,
a fork of Telegram Desktop.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// AdShield ("hide repeated ads") core, ported from the shared reference
// engine (adshield/ref/adshield.py, algoVersion 3, formatVersion 3). The
// normative rules live in adshield/SPEC.md; function names follow SPEC 9.2.
//
// Pure logic: std only, no Qt, no platform message types. Strings are
// std::u32string (one element per Unicode scalar); the platform converts
// QString with toUcs4() and maps its own message objects to MessageInput.
//
// Thread confinement: nothing here is synchronised. A Store / Account and
// every ChatContext handed to it must be used from one thread only (the
// dedicated AdShield queue, SPEC 8.4). The free functions are reentrant.
namespace Lumina::AdShield {

inline constexpr int kAlgoVersion = 3;
inline constexpr int kFormatVersion = 3;

inline constexpr std::int64_t kHour = 3600;
inline constexpr std::int64_t kDay = 86400;

inline constexpr int kMinTextWlen = 12;
inline constexpr int kMinTmplWlen = 4;
inline constexpr int kMinCjkCount = 8;
inline constexpr int kParaMinWlen = 32;
inline constexpr std::size_t kMaxPara = 16;

// v3 edit-tolerant keys (SPEC 2.7): the VAR family and BLOCK.
inline constexpr int kVarMinWlen = 24;
inline constexpr int kVarDropRatio = 2;
inline constexpr int kVarStrictRatio = 4;
inline constexpr std::size_t kVarEdge = 16;
inline constexpr std::size_t kMaxVar = 1 + 2 * kVarEdge + 1;
inline constexpr int kBlockMinWlen = 16;
inline constexpr std::size_t kMaxBlock = 16;
inline constexpr std::size_t kBlockMinShared = 2;
inline constexpr std::size_t kBlockContainNum = 4;
inline constexpr std::size_t kBlockContainDen = 5;

inline constexpr int kPureTextMinCjk = 12;
inline constexpr int kPureTextMinLetters = 24;
inline constexpr int kAckRemainderMax = 2;
inline constexpr std::size_t kMassMention = 3;
inline constexpr std::size_t kGapMax = 4;
inline constexpr std::size_t kNumCtxWindow = 12;
inline constexpr std::size_t kPointerNegWindow = 4;
inline constexpr std::int64_t kKnownMemberMinAge = kDay;

inline constexpr std::int64_t kEvictAfter = 8 * kDay;
inline constexpr std::int64_t kRecordTtl = 90 * kDay;
inline constexpr std::int64_t kHandleTtl = 30 * kDay;
inline constexpr std::size_t kMaxRows = 3000;
inline constexpr std::size_t kMaxEndorseRecords = 200;
inline constexpr std::size_t kMaxNotAdRecords = 200;
inline constexpr std::size_t kMaxHandles = 2000;
inline constexpr std::size_t kMaxLinksPerRow = 16;
inline constexpr std::size_t kMaxPendingPerRow = 16;
inline constexpr std::size_t kMaxRowKeys = 6 + kMaxPara + kMaxVar + kMaxBlock;
inline constexpr std::size_t kMaxRecordKeys = 128;
// A record starts with what a v2 record held (exact keys, then paragraph
// keys, at most this many), then the v3 keys (SPEC 6.4).
inline constexpr std::size_t kRecordV2Keys = 64;
inline constexpr std::size_t kMaxRecordLinks = 64;
inline constexpr std::int64_t kTotalCapBytes = 10 * 1024 * 1024;
inline constexpr std::int64_t kMaxDate = std::int64_t(1) << 40;
inline constexpr std::int64_t kMaxMsgId = 0x7FFFFFFF;

using Key = std::array<unsigned char, 16>;

enum class KeyKind : std::uint8_t {
	Fwd = 1,
	Media = 2,
	Text = 3,
	Cjk = 4,
	Tmpl = 5,
	Para = 6,
	Contact = 7,
	// v3: the VAR family (all three hash with the tag "var", so the same
	// payload has the same hash whatever its kind) and BLOCK.
	Var = 8,
	VarL = 9,
	VarS = 10,
	Block = 11,
};

namespace Flag {

inline constexpr std::uint32_t kInvite = 1U << 0;
inline constexpr std::uint32_t kHandle = 1U << 1;
inline constexpr std::uint32_t kShortener = 1U << 2;
inline constexpr std::uint32_t kButton = 1U << 3;
inline constexpr std::uint32_t kViaBot = 1U << 4;
inline constexpr std::uint32_t kContactCard = 1U << 5;
inline constexpr std::uint32_t kPhone = 1U << 6;
inline constexpr std::uint32_t kAccount = 1U << 7;
inline constexpr std::uint32_t kPointer = 1U << 8;
// v3: a privileged author's own strong feature that only counts as trusted
// because it is pinned, or was demoted because the post is the linked
// channel's own (SPEC 6.12). Always together with kPriv.
inline constexpr std::uint32_t kPrivStrong = 1U << 9;
inline constexpr std::uint32_t kUrl = 1U << 10;
inline constexpr std::uint32_t kMemberMention = 1U << 11;
inline constexpr std::uint32_t kLongNumber = 1U << 12;
inline constexpr std::uint32_t kMassMention = 1U << 13;
inline constexpr std::uint32_t kEmail = 1U << 14;
// v3: a privileged author (SPEC 6.12). Always together with kEndorsing.
inline constexpr std::uint32_t kPriv = 1U << 15;
inline constexpr std::uint32_t kHasText = 1U << 16;
inline constexpr std::uint32_t kMediaOnly = 1U << 17;
inline constexpr std::uint32_t kPureText = 1U << 18;
inline constexpr std::uint32_t kAck = 1U << 19;
inline constexpr std::uint32_t kReplyMe = 1U << 20;
inline constexpr std::uint32_t kMentionMe = 1U << 21;
inline constexpr std::uint32_t kIsReply = 1U << 22;
inline constexpr std::uint32_t kEndorsing = 1U << 23;
inline constexpr std::uint32_t kBot = 1U << 24;
inline constexpr std::uint32_t kContact = 1U << 25;
inline constexpr std::uint32_t kSelf = 1U << 26;
inline constexpr std::uint32_t kHasMediaKey = 1U << 27;
inline constexpr std::uint32_t kHasFwdKey = 1U << 28;
inline constexpr std::uint32_t kAnon = 1U << 29;
inline constexpr std::uint32_t kTmplCond = 1U << 30;

inline constexpr std::uint32_t kStrongMask = 0x1FFU;
inline constexpr std::uint32_t kWeakMask = 0x7C00U;
inline constexpr std::uint32_t kAnonKeep = kAck
	| kHasMediaKey
	| kHasFwdKey
	| kHasText
	| kTmplCond;
inline constexpr std::uint32_t kStoredMask = kStrongMask
	| kWeakMask
	| kHasText
	| kPureText
	| kAck
	| kReplyMe
	| kMentionMe
	| kIsReply
	| kEndorsing
	| kBot
	| kContact
	| kSelf
	| kHasMediaKey
	| kHasFwdKey
	| kAnon
	| kTmplCond
	| kPriv
	| kPrivStrong;

} // namespace Flag

enum class Role : std::uint8_t {
	Member,
	Admin,
	Creator,
	AnonAdmin,
	Bot,
	Contact,
	Self,
	Channel,
};

enum class MediaKind : std::uint8_t {
	Absent,
	Photo,
	Video,
	Document,
	Audio,
	Contact,
	Sticker,
	Gif,
	Voice,
	Round,
	Dice,
	Location,
	Venue,
	Poll,
	Webpage,
	Game,
	Story,
	Invoice,
	Giveaway,
	Paid,
};

enum class ForwardKind : std::uint8_t {
	Channel,
	User,
	Hidden,
};

enum class ChatType : std::uint8_t {
	Group,
	Supergroup,
	Forum,
	Gigagroup,
	Other,
};

enum class EntityType : std::uint8_t {
	TextUrl,
	MentionName,
	Other,
};

struct Sender {
	std::int64_t id = 0;
	Role role = Role::Member;
	std::u32string username;
};

struct Entity {
	EntityType type = EntityType::Other;
	std::u32string url;
	std::int64_t userId = 0;
	bool hasUserId = false;
};

// id: the signed int64 media id as decimal text (desktop PhotoId and
// DocumentId are uint64 and must be static_cast<int64> first, SPEC 3).
// An empty id means "no id".
struct Media {
	MediaKind kind = MediaKind::Absent;
	std::string id;
	std::int64_t userId = 0;
	std::u32string phone;
};

struct Forward {
	ForwardKind fromKind = ForwardKind::Hidden;
	std::int64_t fromId = 0;
	std::int64_t postId = 0;
	std::u32string fromName;
	std::u32string username;
	std::int64_t date = 0;
};

struct MessageInput {
	std::int64_t id = 0;
	std::int64_t date = 0;
	Sender sender;
	std::u32string text;
	std::vector<Entity> entities;
	Media media;
	std::int64_t album = 0;
	std::optional<Forward> forward;
	std::vector<std::u32string> buttons;
	std::int64_t replyTo = 0;
	bool viaBot = false;
	bool replyToMe = false;
	bool mentionsMe = false;
	bool pinned = false;
	bool autoForward = false;
	bool service = false;
	bool sponsored = false;
	bool scheduled = false;
};

struct ChatContextData {
	std::int64_t chatId = 0;
	ChatType type = ChatType::Supergroup;
	std::u32string username;
	std::int64_t linkedChannelId = 0;
	std::u32string linkedChannelUsername;
	std::u32string description;
	std::vector<std::u32string> pinnedTexts;
	bool adminsKnown = true;
	std::vector<std::u32string> memberHandles;
	std::u32string selfUsername;
};

// Immutable once built: usernames are folded and the trusted tokens and
// domains (SPEC 5.1) are extracted from the description and pinned texts in
// the constructor. Build a new one whenever the chat information changes.
//
// pinnedOnlyTokens() (v3, SPEC 6.12) are the trusted tokens that are trusted
// only because a pinned message has them: not the group's or the linked
// channel's username and nothing from the description.
class ChatContext {
public:
	explicit ChatContext(ChatContextData data);

	[[nodiscard]] std::int64_t chatId() const {
		return _chatId;
	}
	[[nodiscard]] ChatType type() const {
		return _type;
	}
	[[nodiscard]] bool eligible() const;
	[[nodiscard]] bool adminsKnown() const {
		return _adminsKnown;
	}
	[[nodiscard]] std::int64_t linkedChannelId() const {
		return _linkedChannelId;
	}
	[[nodiscard]] const std::u32string &username() const {
		return _username;
	}
	[[nodiscard]] const std::u32string &linkedChannelUsername() const {
		return _linkedChannelUsername;
	}
	[[nodiscard]] const std::u32string &selfUsername() const {
		return _selfUsername;
	}
	[[nodiscard]] const std::vector<std::u32string> &memberHandles() const {
		return _memberHandles;
	}
	[[nodiscard]] const std::set<std::u32string> &trustedTokens() const {
		return _trustedTokens;
	}
	[[nodiscard]] const std::set<std::u32string> &trustedDomains() const {
		return _trustedDomains;
	}
	[[nodiscard]] const std::set<std::u32string> &pinnedOnlyTokens() const {
		return _pinnedOnlyTokens;
	}

private:
	std::int64_t _chatId = 0;
	ChatType _type = ChatType::Supergroup;
	bool _adminsKnown = true;
	std::int64_t _linkedChannelId = 0;
	std::u32string _username;
	std::u32string _linkedChannelUsername;
	std::u32string _selfUsername;
	std::vector<std::u32string> _memberHandles;
	std::set<std::u32string> _trustedTokens;
	std::set<std::u32string> _trustedDomains;
	std::set<std::u32string> _pinnedOnlyTokens;

};

enum class LinkKind : std::uint8_t {
	Invite,
	Handle,
	Url,
	Phone,
	Acct,
	Mail,
};

enum class LinkSource : std::uint8_t {
	Text,
	Hidden,
	Button,
	Media,
};

enum class LinkFeature : std::uint8_t {
	NotCounted,
	Strong,
	Weak,
	Member,
	Pending,
};

struct Link {
	std::u32string token;
	LinkKind kind = LinkKind::Url;
	std::u32string type;
	std::u32string domain;
	bool shortener = false;
	LinkSource source = LinkSource::Text;
};

struct Extraction {
	std::u32string foldNoLinks;
	std::vector<Link> links;
	std::vector<std::u32string> mentions;
	bool pointer = false;
	std::vector<std::u32string> longNumbers;
};

struct KeyRef {
	KeyKind kind = KeyKind::Text;
	std::uint64_t hash = 0;

	friend inline bool operator==(const KeyRef &a, const KeyRef &b) {
		return (a.kind == b.kind) && (a.hash == b.hash);
	}
	friend inline bool operator!=(const KeyRef &a, const KeyRef &b) {
		return !(a == b);
	}
	friend inline bool operator<(const KeyRef &a, const KeyRef &b) {
		return (a.kind != b.kind) ? (a.kind < b.kind) : (a.hash < b.hash);
	}
};

struct FingerprintKey {
	KeyKind kind = KeyKind::Text;
	std::u32string payload;
	std::uint64_t hash = 0;
};

struct FingerprintResult {
	std::u32string fold;
	std::u32string foldNoLinks;
	std::u32string text;
	std::u32string cjk;
	int wlen = 0;
	int cjkCount = 0;
	int otherLetterCount = 0;
	std::vector<std::u32string> paragraphs;
	// v3: the intermediate values of the VAR and BLOCK keys (vectors).
	std::vector<std::u32string> contentLines;
	std::vector<std::u32string> blocks;
	std::vector<Link> links;
	std::vector<LinkFeature> linkFeatures;
	std::vector<FingerprintKey> keys;
	std::vector<std::uint64_t> linkHashes;
	std::vector<std::uint64_t> pending;
	std::uint32_t flags = 0;
	std::uint64_t senderKey = 0;
	bool store = false;
	bool endorsing = false;
	bool privileged = false;
	int mentionCount = 0;
	std::uint32_t resolvedFlags = 0;
};

struct ResolveResult {
	std::uint32_t flags = 0;
	std::vector<KeyRef> keys;
	std::vector<std::uint64_t> links;
};

using IsKnownHandle = std::function<bool(std::uint64_t handleHash)>;

[[nodiscard]] std::string ToUtf8(std::u32string_view s);
[[nodiscard]] std::string Hex64(std::uint64_t value);
[[nodiscard]] std::uint64_t Hash(
	const Key &key,
	std::string_view tag,
	std::string_view payloadUtf8);
[[nodiscard]] std::uint64_t Hash(
	const Key &key,
	std::string_view tag,
	std::u32string_view payload);
[[nodiscard]] std::uint64_t HashBytes(
	const Key &key,
	std::string_view tag,
	const unsigned char *data,
	std::size_t size);
[[nodiscard]] std::uint64_t HandleHash(
	const Key &key,
	std::u32string_view foldedName);

[[nodiscard]] std::u32string FoldUsername(std::u32string_view username);
[[nodiscard]] bool ValidUsername(std::u32string_view name);
[[nodiscard]] std::pair<int, int> LetterCounts(std::u32string_view text);
[[nodiscard]] std::vector<std::u32string> Paragraphs(
	std::u32string_view foldNoLinks);

// v3 (SPEC 2.1): every U+000D right before a U+000A is dropped (one pass, per
// scalar). Applied to the message text before Fold.
[[nodiscard]] std::u32string CollapseCrlf(std::u32string_view text);

// v3 (SPEC 2.7). ContentLines: the non-empty text forms of the lines of F'.
// VarKeys: (kind, payload) of the VAR family, kind being KeyKind::Var,
// VarL or VarS as an int. Blocks: every block (unfiltered). BlockPayloads:
// the BLOCK key payloads (none when fewer than 2 qualify).
[[nodiscard]] std::vector<std::u32string> ContentLines(
	std::u32string_view foldNoLinks);
[[nodiscard]] std::vector<std::pair<int, std::u32string>> VarKeys(
	const std::vector<std::u32string> &lines);
[[nodiscard]] std::vector<std::u32string> Blocks(
	std::u32string_view foldNoLinks);
[[nodiscard]] std::vector<std::u32string> BlockPayloads(
	const std::vector<std::u32string> &blocks);
[[nodiscard]] std::u32string RegisteredDomain(std::u32string_view host);
[[nodiscard]] std::optional<Link> CanonicalizeUrl(
	std::u32string_view raw,
	LinkSource source = LinkSource::Text);
[[nodiscard]] Extraction ExtractLinks(std::u32string_view folded);
[[nodiscard]] std::vector<std::u32string> LeadingHandles(
	std::u32string_view folded);

[[nodiscard]] bool EligibleMessage(const MessageInput &msg);
[[nodiscard]] bool ValidMessage(const MessageInput &msg);

// knownHandles (folded, without '@') only affects resolvedFlags.
[[nodiscard]] FingerprintResult Fingerprint(
	const MessageInput &msg,
	const ChatContext &context,
	const Key &key,
	const std::vector<std::u32string> &knownHandles = {});

[[nodiscard]] ResolveResult Resolve(
	std::uint32_t flags,
	const std::vector<KeyRef> &keys,
	const std::vector<std::uint64_t> &links,
	const std::vector<std::uint64_t> &pending,
	const IsKnownHandle &isKnown);

[[nodiscard]] std::vector<std::string> StrongNames(std::uint32_t flags);
[[nodiscard]] std::vector<std::string> WeakNames(std::uint32_t flags);
[[nodiscard]] std::string_view KeyKindName(KeyKind kind);
[[nodiscard]] std::string_view KeyKindTag(KeyKind kind);
[[nodiscard]] std::string_view LinkKindName(LinkKind kind);
[[nodiscard]] std::string_view LinkSourceName(LinkSource source);
[[nodiscard]] std::string_view LinkFeatureName(LinkFeature feature);

} // namespace Lumina::AdShield
