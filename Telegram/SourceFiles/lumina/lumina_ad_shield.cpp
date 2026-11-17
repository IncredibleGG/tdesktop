/*
This file is part of LuminaGram,
a fork of Telegram Desktop.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "lumina/lumina_ad_shield.h"

#include "lumina/ad_shield_tables.h"

#include <algorithm>
#include <cstdio>
#include <iterator>
#include <utility>

namespace Lumina::AdShield {
namespace {

namespace Tables = Lumina::AdShieldTables;

using String = std::u32string;
using View = std::u32string_view;

constexpr auto kNpos = View::npos;

// "No line" for VarKeys().
constexpr auto kNoLine = std::size_t(-1);

// The text rounds of SPEC 4.2. Every pattern there uses ASCII classes only,
// so each one is matched by a small hand-written scanner that reproduces the
// backtracking regex semantics (leftmost match, greedy, lookarounds looking
// at the whole current string, scanning resumes at the end of the match).
// All positions are scalar indices into std::u32string.
enum class Round {
	Scheme,
	Tme,
	TmeSub,
	Email,
	Domain,
	Mention,
	MentionSpace,
	PhoneIntl,
	PhoneCn,
	PhoneTw,
};

struct Span {
	std::size_t from = 0;
	std::size_t till = 0;
};

struct Match {
	std::size_t from = 0;
	std::size_t till = 0;
	std::size_t group = 0;
};

[[nodiscard]] bool IsLower(char32_t c) {
	return (c >= U'a') && (c <= U'z');
}

[[nodiscard]] bool IsDigit(char32_t c) {
	return (c >= U'0') && (c <= U'9');
}

[[nodiscard]] bool IsAlnum(char32_t c) {
	return IsLower(c) || IsDigit(c);
}

[[nodiscard]] bool IsOneOf(char32_t c, View set) {
	return set.find(c) != kNpos;
}

[[nodiscard]] bool IsTrim(char32_t c) {
	return IsOneOf(c, U".,;:!?*");
}

[[nodiscard]] bool IsUrlChar(char32_t c) {
	return IsAlnum(c) || IsOneOf(c, U"-._~:/?#@!$&*+,;=%");
}

[[nodiscard]] bool IsUrlBehind(char32_t c) {
	return IsAlnum(c) || IsOneOf(c, U"_-.@/");
}

[[nodiscard]] bool IsEmailLocal(char32_t c) {
	return IsAlnum(c) || IsOneOf(c, U"._%+-");
}

[[nodiscard]] bool IsDomainChar(char32_t c) {
	return IsAlnum(c) || (c == U'-');
}

[[nodiscard]] bool IsUserChar(char32_t c) {
	return IsAlnum(c) || (c == U'_');
}

[[nodiscard]] bool IsSeparator(char32_t c) {
	return (c == U' ') || (c == U'-') || (c == U'.');
}

[[nodiscard]] bool IsAccountChar(char32_t c) {
	return IsAlnum(c) || IsOneOf(c, U"_-.");
}

// Python str.isspace(), which str.strip() uses.
[[nodiscard]] bool IsPySpace(char32_t c) {
	return (c >= 0x09 && c <= 0x0D)
		|| (c >= 0x1C && c <= 0x20)
		|| (c == 0x85)
		|| (c == 0xA0)
		|| (c == 0x1680)
		|| (c >= 0x2000 && c <= 0x200A)
		|| (c == 0x2028)
		|| (c == 0x2029)
		|| (c == 0x202F)
		|| (c == 0x205F)
		|| (c == 0x3000);
}

[[nodiscard]] bool StartsWith(View s, std::size_t at, View w) {
	return (at <= s.size())
		&& (s.size() - at >= w.size())
		&& (s.compare(at, w.size(), w) == 0);
}

[[nodiscard]] bool StartsWith(View s, View w) {
	return StartsWith(s, 0, w);
}

[[nodiscard]] bool EndsWith(View s, View w) {
	return (s.size() >= w.size())
		&& (s.compare(s.size() - w.size(), w.size(), w) == 0);
}

[[nodiscard]] bool Contains(View s, View w) {
	return s.find(w) != kNpos;
}

[[nodiscard]] bool IsAsciiWord(View s) {
	return std::all_of(s.begin(), s.end(), IsLower);
}

[[nodiscard]] bool IsAllAscii(View s) {
	return std::all_of(s.begin(), s.end(), [](char32_t c) {
		return c < 0x80;
	});
}

[[nodiscard]] bool IsDigits(View s) {
	return !s.empty() && std::all_of(s.begin(), s.end(), IsDigit);
}

[[nodiscard]] String Digits(View s) {
	auto result = String();
	for (const auto c : s) {
		if (IsDigit(c)) {
			result.push_back(c);
		}
	}
	return result;
}

[[nodiscard]] String FromAscii(std::string_view s) {
	auto result = String();
	result.reserve(s.size());
	for (const auto c : s) {
		result.push_back(char32_t(static_cast<unsigned char>(c)));
	}
	return result;
}

[[nodiscard]] String FromUtf8(std::string_view s) {
	auto result = String();
	result.reserve(s.size());
	for (std::size_t i = 0; i != s.size();) {
		const auto c = static_cast<unsigned char>(s[i]);
		auto cp = char32_t(0xFFFD);
		auto length = std::size_t(1);
		if (c < 0x80) {
			cp = c;
		} else if ((c >> 5) == 0x06) {
			cp = c & 0x1F;
			length = 2;
		} else if ((c >> 4) == 0x0E) {
			cp = c & 0x0F;
			length = 3;
		} else if ((c >> 3) == 0x1E) {
			cp = c & 0x07;
			length = 4;
		}
		if (length > 1) {
			if (i + length > s.size()) {
				cp = 0xFFFD;
				length = 1;
			} else {
				for (std::size_t j = 1; j != length; ++j) {
					const auto next = static_cast<unsigned char>(s[i + j]);
					if ((next >> 6) != 0x02) {
						cp = 0xFFFD;
						length = j;
						break;
					}
					cp = (cp << 6) | (next & 0x3F);
				}
			}
		}
		result.push_back(cp);
		i += length;
	}
	return result;
}

[[nodiscard]] String Dec(std::int64_t value) {
	return FromAscii(std::to_string(value));
}

[[nodiscard]] String Strip(View s, char32_t c) {
	auto from = std::size_t(0);
	auto till = s.size();
	while (from < till && s[from] == c) {
		++from;
	}
	while (till > from && s[till - 1] == c) {
		--till;
	}
	return String(s.substr(from, till - from));
}

[[nodiscard]] String RightStrip(View s, View chars) {
	auto till = s.size();
	while (till > 0 && IsOneOf(s[till - 1], chars)) {
		--till;
	}
	return String(s.substr(0, till));
}

[[nodiscard]] std::vector<String> SplitNonEmpty(View s, char32_t c) {
	auto result = std::vector<String>();
	auto from = std::size_t(0);
	while (from <= s.size()) {
		const auto i = s.find(c, from);
		const auto till = (i == kNpos) ? s.size() : i;
		if (till > from) {
			result.emplace_back(s.substr(from, till - from));
		}
		if (i == kNpos) {
			break;
		}
		from = i + 1;
	}
	return result;
}

[[nodiscard]] String Join(const std::vector<String> &parts, View separator) {
	auto result = String();
	for (std::size_t i = 0; i != parts.size(); ++i) {
		if (i > 0) {
			result.append(separator);
		}
		result.append(parts[i]);
	}
	return result;
}

void ReplaceAll(String &s, View what) {
	if (what.empty()) {
		return;
	}
	auto result = String();
	auto from = std::size_t(0);
	while (true) {
		const auto i = s.find(what, from);
		if (i == kNpos) {
			break;
		}
		result.append(s, from, i - from);
		from = i + what.size();
	}
	if (from == 0) {
		return;
	}
	result.append(s, from, kNpos);
	s = std::move(result);
}

template <std::size_t N>
[[nodiscard]] bool InList(const std::u32string_view (&list)[N], View value) {
	return std::find(std::begin(list), std::end(list), value)
		!= std::end(list);
}

template <std::size_t N>
[[nodiscard]] const View *Lookup(
		const std::u32string_view (&from)[N],
		const std::u32string_view (&to)[N],
		View key) {
	for (std::size_t i = 0; i != N; ++i) {
		if (from[i] == key) {
			return &to[i];
		}
	}
	return nullptr;
}

[[nodiscard]] Link MakeLink(
		String token,
		LinkKind kind,
		String type,
		LinkSource source,
		String domain = String(),
		bool shortener = false) {
	auto result = Link();
	result.token = std::move(token);
	result.kind = kind;
	result.type = std::move(type);
	result.domain = std::move(domain);
	result.shortener = shortener;
	result.source = source;
	return result;
}

[[nodiscard]] std::optional<Link> PhoneLink(String digits, LinkSource source) {
	if (digits.size() < 7 || digits.size() > 15) {
		return std::nullopt;
	}
	return MakeLink(U"tel:" + digits, LinkKind::Phone, U"PHONE", source);
}

[[nodiscard]] std::vector<std::pair<String, String>> ParseQuery(View query) {
	auto result = std::vector<std::pair<String, String>>();
	auto from = std::size_t(0);
	while (from <= query.size()) {
		const auto i = query.find(U'&', from);
		const auto till = (i == kNpos) ? query.size() : i;
		const auto part = query.substr(from, till - from);
		if (!part.empty()) {
			const auto eq = part.find(U'=');
			if (eq == kNpos) {
				result.emplace_back(String(part), String());
			} else {
				result.emplace_back(
					String(part.substr(0, eq)),
					String(part.substr(eq + 1)));
			}
		}
		if (i == kNpos) {
			break;
		}
		from = i + 1;
	}
	return result;
}

// Repeated query keys: the last one wins (SPEC 4.3).
[[nodiscard]] const String *QueryValue(
		const std::vector<std::pair<String, String>> &query,
		View key) {
	const String *result = nullptr;
	for (const auto &[k, v] : query) {
		if (k == key) {
			result = &v;
		}
	}
	return result;
}

[[nodiscard]] String CleanQuery(View query) {
	auto keep = std::vector<std::pair<String, String>>();
	for (auto &[k, v] : ParseQuery(query)) {
		if (InList(Tables::kTrackingParams32, k)) {
			continue;
		}
		const auto tracked = std::any_of(
			std::begin(Tables::kTrackingPrefixes32),
			std::end(Tables::kTrackingPrefixes32),
			[&](View prefix) { return StartsWith(k, prefix); });
		if (tracked) {
			continue;
		}
		keep.emplace_back(std::move(k), std::move(v));
	}
	std::sort(keep.begin(), keep.end());
	auto parts = std::vector<String>();
	parts.reserve(keep.size());
	for (const auto &[k, v] : keep) {
		parts.push_back(v.empty() ? k : (k + U"=" + v));
	}
	return Join(parts, U"&");
}

[[nodiscard]] std::optional<Link> TmeLink(View path, LinkSource source) {
	const auto segs = SplitNonEmpty(path, U'/');
	if (segs.empty()) {
		return MakeLink(
			U"url:t.me",
			LinkKind::Url,
			U"URL:t.me",
			source,
			U"t.me");
	}
	const auto &s0 = segs[0];
	if (s0[0] == U'+') {
		const auto h = s0.substr(1);
		if (IsDigits(h)) {
			return PhoneLink(h, source);
		}
		if (!h.empty()) {
			return MakeLink(
				U"invite:" + h,
				LinkKind::Invite,
				U"INVITE",
				source);
		}
	}
	if (s0 == U"joinchat" && segs.size() > 1) {
		return MakeLink(
			U"invite:" + segs[1],
			LinkKind::Invite,
			U"INVITE",
			source);
	}
	if (s0 == U"addlist" && segs.size() > 1) {
		return MakeLink(
			U"invite:addlist/" + segs[1],
			LinkKind::Invite,
			U"INVITE",
			source);
	}
	if (s0 == U"c" && segs.size() > 1 && IsDigits(segs[1])) {
		return MakeLink(
			U"tgc:" + segs[1],
			LinkKind::Handle,
			U"HANDLE",
			source);
	}
	if (s0 == U"s" && segs.size() > 1 && ValidUsername(segs[1])) {
		return MakeLink(U"@" + segs[1], LinkKind::Handle, U"HANDLE", source);
	}
	if (InList(Tables::kTmeReserved32, s0)) {
		return MakeLink(
			U"url:t.me/" + s0,
			LinkKind::Url,
			U"URL:t.me",
			source,
			U"t.me");
	}
	if (ValidUsername(s0)) {
		return MakeLink(U"@" + s0, LinkKind::Handle, U"HANDLE", source);
	}
	return MakeLink(
		U"url:t.me/" + Join(segs, U"/"),
		LinkKind::Url,
		U"URL:t.me",
		source,
		U"t.me");
}

void NormalizePhoneToken(Link &link) {
	if (link.kind != LinkKind::Phone) {
		return;
	}
	const auto d = View(link.token).substr(4);
	if (d.size() == 11 && d[0] == U'1' && d[1] >= U'3' && d[1] <= U'9') {
		link.token = U"tel:86" + String(d);
	} else if (d.size() == 10 && StartsWith(d, U"09")) {
		link.token = U"tel:886" + String(d.substr(1));
	}
}

[[nodiscard]] String RemoveSpans(View s, std::vector<Span> spans) {
	if (spans.empty()) {
		return String(s);
	}
	std::sort(spans.begin(), spans.end(), [](Span a, Span b) {
		return (a.from != b.from) ? (a.from < b.from) : (a.till < b.till);
	});
	auto result = String();
	result.reserve(s.size());
	auto position = std::size_t(0);
	for (const auto span : spans) {
		if (span.from < position) {
			continue;
		}
		result.append(s.substr(position, span.from - position));
		result.push_back(U' ');
		position = span.till;
	}
	result.append(s.substr(position));
	return result;
}

[[nodiscard]] std::size_t RunEnd(
		View s,
		std::size_t from,
		bool (*predicate)(char32_t)) {
	while (from < s.size() && predicate(s[from])) {
		++from;
	}
	return from;
}

// (?:https?|ftp|tg)://[URL]+
[[nodiscard]] std::optional<Match> MatchScheme(View s, std::size_t p) {
	auto q = std::size_t(0);
	if (StartsWith(s, p, U"https://")) {
		q = p + 8;
	} else if (StartsWith(s, p, U"http://")) {
		q = p + 7;
	} else if (StartsWith(s, p, U"ftp://")) {
		q = p + 6;
	} else if (StartsWith(s, p, U"tg://")) {
		q = p + 5;
	} else {
		return std::nullopt;
	}
	const auto till = RunEnd(s, q, IsUrlChar);
	if (till == q) {
		return std::nullopt;
	}
	return Match{ p, till, p };
}

// ' ?\. ?(?:me|dog) ?/[URL]*' after the "t" or "telegram" part.
[[nodiscard]] std::optional<std::size_t> MatchTmeTail(
		View s,
		std::size_t q) {
	if (q < s.size() && s[q] == U' ') {
		++q;
	}
	if (q >= s.size() || s[q] != U'.') {
		return std::nullopt;
	}
	++q;
	if (q < s.size() && s[q] == U' ') {
		++q;
	}
	if (StartsWith(s, q, U"me")) {
		q += 2;
	} else if (StartsWith(s, q, U"dog")) {
		q += 3;
	} else {
		return std::nullopt;
	}
	if (q < s.size() && s[q] == U' ') {
		++q;
	}
	if (q >= s.size() || s[q] != U'/') {
		return std::nullopt;
	}
	return RunEnd(s, q + 1, IsUrlChar);
}

[[nodiscard]] std::optional<std::size_t> MatchTmeBody(
		View s,
		std::size_t q) {
	if (q < s.size() && s[q] == U't') {
		if (const auto till = MatchTmeTail(s, q + 1)) {
			return till;
		}
	}
	if (StartsWith(s, q, U"telegram")) {
		return MatchTmeTail(s, q + 8);
	}
	return std::nullopt;
}

// (?<![a-z0-9_\-.@/])(?:www\.)?(?:t|telegram) ?\. ?(?:me|dog) ?/[URL]*
[[nodiscard]] std::optional<Match> MatchTme(View s, std::size_t p) {
	if (p > 0 && IsUrlBehind(s[p - 1])) {
		return std::nullopt;
	}
	if (StartsWith(s, p, U"www.")) {
		if (const auto till = MatchTmeBody(s, p + 4)) {
			return Match{ p, *till, p };
		}
	}
	if (const auto till = MatchTmeBody(s, p)) {
		return Match{ p, *till, p };
	}
	return std::nullopt;
}

// (?<![a-z0-9_\-.@/])[a-z][a-z0-9_]{3,31}\.t\.me(?![a-z0-9\-])(?:/[URL]*)?
[[nodiscard]] std::optional<Match> MatchTmeSub(View s, std::size_t p) {
	if (p > 0 && IsUrlBehind(s[p - 1])) {
		return std::nullopt;
	}
	if (p >= s.size() || !IsLower(s[p])) {
		return std::nullopt;
	}
	const auto runEnd = RunEnd(s, p + 1, IsUserChar);
	const auto length = runEnd - (p + 1);
	if (length < 3 || length > 31 || !StartsWith(s, runEnd, U".t.me")) {
		return std::nullopt;
	}
	auto till = runEnd + 5;
	if (till < s.size() && IsDomainChar(s[till])) {
		return std::nullopt;
	}
	if (till < s.size() && s[till] == U'/') {
		till = RunEnd(s, till + 1, IsUrlChar);
	}
	return Match{ p, till, p };
}

struct Label {
	std::size_t from = 0;
	std::size_t till = 0;
};

// Labels of [a-z0-9\-]+ separated by single dots, starting at "from".
[[nodiscard]] std::vector<Label> DottedLabels(View s, std::size_t from) {
	auto result = std::vector<Label>();
	auto q = from;
	while (true) {
		const auto till = RunEnd(s, q, IsDomainChar);
		if (till == q) {
			break;
		}
		result.push_back({ q, till });
		if (till + 1 < s.size()
			&& s[till] == U'.'
			&& IsDomainChar(s[till + 1])) {
			q = till + 1;
		} else {
			break;
		}
	}
	return result;
}

// (?<![a-z0-9._%+\-])[a-z0-9._%+\-]+@[a-z0-9\-]+(?:\.[a-z0-9\-]+)*
//   \.[a-z]{2,24}(?![a-z0-9\-])
[[nodiscard]] std::optional<Match> MatchEmail(View s, std::size_t p) {
	if (p > 0 && IsEmailLocal(s[p - 1])) {
		return std::nullopt;
	}
	const auto at = RunEnd(s, p, IsEmailLocal);
	if (at == p || at >= s.size() || s[at] != U'@') {
		return std::nullopt;
	}
	const auto labels = DottedLabels(s, at + 1);
	for (auto j = labels.size(); j >= 2; --j) {
		const auto &label = labels[j - 1];
		const auto length = label.till - label.from;
		if (length < 2 || length > 24) {
			continue;
		}
		const auto letters = std::all_of(
			s.begin() + label.from,
			s.begin() + label.till,
			IsLower);
		if (letters) {
			return Match{ p, label.till, p };
		}
	}
	return std::nullopt;
}

[[nodiscard]] bool IsTld(View s) {
	return InList(Tables::kTlds32, s);
}

// (?<![a-z0-9_\-.@/])(?:[a-z0-9](?:[a-z0-9\-]{0,61}[a-z0-9])?\.)+(?:TLDS)
//   (?![a-z0-9\-])(?::[0-9]{1,5})?(?:/[URL]*)?
[[nodiscard]] std::optional<Match> MatchDomain(View s, std::size_t p) {
	if (p > 0 && IsUrlBehind(s[p - 1])) {
		return std::nullopt;
	}
	auto tldStarts = std::vector<std::size_t>();
	auto q = p;
	while (true) {
		const auto till = RunEnd(s, q, IsDomainChar);
		const auto length = till - q;
		if (length < 1
			|| length > 63
			|| !IsAlnum(s[q])
			|| !IsAlnum(s[till - 1])
			|| till >= s.size()
			|| s[till] != U'.') {
			break;
		}
		q = till + 1;
		tldStarts.push_back(q);
	}
	for (auto j = tldStarts.size(); j >= 1; --j) {
		const auto t = tldStarts[j - 1];
		const auto runEnd = RunEnd(s, t, IsDomainChar);
		if (runEnd == t || !IsTld(s.substr(t, runEnd - t))) {
			continue;
		}
		auto till = runEnd;
		if (till + 1 < s.size() && s[till] == U':' && IsDigit(s[till + 1])) {
			auto digits = std::size_t(0);
			auto r = till + 1;
			while (r < s.size() && digits < 5 && IsDigit(s[r])) {
				++r;
				++digits;
			}
			till = r;
		}
		if (till < s.size() && s[till] == U'/') {
			till = RunEnd(s, till + 1, IsUrlChar);
		}
		return Match{ p, till, p };
	}
	return std::nullopt;
}

// (?<![a-z0-9_])@([a-z][a-z0-9_]{3,31})(?![a-z0-9_]), optionally "@ ".
[[nodiscard]] std::optional<Match> MatchMention(
		View s,
		std::size_t p,
		bool space) {
	if (p >= s.size() || s[p] != U'@') {
		return std::nullopt;
	}
	if (p > 0 && IsUserChar(s[p - 1])) {
		return std::nullopt;
	}
	auto q = p + 1;
	if (space) {
		if (q >= s.size() || s[q] != U' ') {
			return std::nullopt;
		}
		++q;
	}
	if (q >= s.size() || !IsLower(s[q])) {
		return std::nullopt;
	}
	const auto till = RunEnd(s, q + 1, IsUserChar);
	const auto length = till - (q + 1);
	if (length < 3 || length > 31) {
		return std::nullopt;
	}
	return Match{ p, till, q };
}

// [0-9](?:[ \-.]?[0-9]){min,max}(?![0-9]) anchored at "from".
[[nodiscard]] std::optional<std::size_t> MatchDigitRun(
		View s,
		std::size_t from,
		std::size_t minimum,
		std::size_t maximum) {
	if (from >= s.size() || !IsDigit(s[from])) {
		return std::nullopt;
	}
	auto ends = std::vector<std::size_t>();
	auto q = from + 1;
	while (ends.size() < maximum) {
		if (q + 1 < s.size() && IsSeparator(s[q]) && IsDigit(s[q + 1])) {
			q += 2;
		} else if (q < s.size() && IsDigit(s[q])) {
			q += 1;
		} else {
			break;
		}
		ends.push_back(q);
	}
	for (auto r = ends.size(); r >= minimum && r > 0; --r) {
		const auto till = ends[r - 1];
		if (till >= s.size() || !IsDigit(s[till])) {
			return till;
		}
	}
	return std::nullopt;
}

// [0-9](?:[ \-.]?[0-9]){count}(?![0-9]) with exactly count repetitions.
[[nodiscard]] std::optional<std::size_t> MatchDigitsExact(
		View s,
		std::size_t q,
		int count) {
	for (auto i = 0; i != count; ++i) {
		if (q + 1 < s.size() && IsSeparator(s[q]) && IsDigit(s[q + 1])) {
			q += 2;
		} else if (q < s.size() && IsDigit(s[q])) {
			q += 1;
		} else {
			return std::nullopt;
		}
	}
	if (q < s.size() && IsDigit(s[q])) {
		return std::nullopt;
	}
	return q;
}

// (?<![0-9a-z+])\+[0-9](?:[ \-.]?[0-9]){6,14}(?![0-9])
[[nodiscard]] std::optional<Match> MatchPhoneIntl(View s, std::size_t p) {
	if (p >= s.size() || s[p] != U'+') {
		return std::nullopt;
	}
	if (p > 0 && (IsAlnum(s[p - 1]) || s[p - 1] == U'+')) {
		return std::nullopt;
	}
	if (const auto till = MatchDigitRun(s, p + 1, 6, 14)) {
		return Match{ p, *till, p };
	}
	return std::nullopt;
}

// (?<![0-9])1[ \-.]?[3-9](?:[ \-.]?[0-9]){9}(?![0-9])
[[nodiscard]] std::optional<Match> MatchPhoneCn(View s, std::size_t p) {
	if (p >= s.size() || s[p] != U'1') {
		return std::nullopt;
	}
	if (p > 0 && IsDigit(s[p - 1])) {
		return std::nullopt;
	}
	const auto isThreeToNine = [](char32_t c) {
		return (c >= U'3') && (c <= U'9');
	};
	auto q = p + 1;
	if (q + 1 < s.size() && IsSeparator(s[q]) && isThreeToNine(s[q + 1])) {
		q += 2;
	} else if (q < s.size() && isThreeToNine(s[q])) {
		q += 1;
	} else {
		return std::nullopt;
	}
	if (const auto till = MatchDigitsExact(s, q, 9)) {
		return Match{ p, *till, p };
	}
	return std::nullopt;
}

// (?<![0-9])09[0-9](?:[ \-.]?[0-9]){7}(?![0-9])
[[nodiscard]] std::optional<Match> MatchPhoneTw(View s, std::size_t p) {
	if (!StartsWith(s, p, U"09") || p + 2 >= s.size() || !IsDigit(s[p + 2])) {
		return std::nullopt;
	}
	if (p > 0 && IsDigit(s[p - 1])) {
		return std::nullopt;
	}
	if (const auto till = MatchDigitsExact(s, p + 3, 7)) {
		return Match{ p, *till, p };
	}
	return std::nullopt;
}

[[nodiscard]] std::optional<Match> MatchAt(Round round, View s, std::size_t p) {
	switch (round) {
	case Round::Scheme: return MatchScheme(s, p);
	case Round::Tme: return MatchTme(s, p);
	case Round::TmeSub: return MatchTmeSub(s, p);
	case Round::Email: return MatchEmail(s, p);
	case Round::Domain: return MatchDomain(s, p);
	case Round::Mention: return MatchMention(s, p, false);
	case Round::MentionSpace: return MatchMention(s, p, true);
	case Round::PhoneIntl: return MatchPhoneIntl(s, p);
	case Round::PhoneCn: return MatchPhoneCn(s, p);
	case Round::PhoneTw: return MatchPhoneTw(s, p);
	}
	return std::nullopt;
}

template <typename Callback>
void ForEachMatch(Round round, View s, Callback &&callback) {
	auto position = std::size_t(0);
	while (position < s.size()) {
		auto found = std::optional<Match>();
		for (auto p = position; p < s.size(); ++p) {
			found = MatchAt(round, s, p);
			if (found) {
				break;
			}
		}
		if (!found) {
			return;
		}
		position = found->till;
		callback(*found);
	}
}

[[nodiscard]] String WindowBefore(View s, std::size_t at) {
	const auto from = (at > kNumCtxWindow) ? (at - kNumCtxWindow) : 0;
	return String(s.substr(from, at - from));
}

[[nodiscard]] bool PriceContext(View s, std::size_t from, std::size_t till) {
	auto after = s.substr(till);
	if (!after.empty() && after[0] == U' ') {
		after.remove_prefix(1);
	}
	for (const auto c : Tables::kCurrencyAfter32) {
		if (StartsWith(after, c)) {
			if (IsAsciiWord(c)
				&& after.size() > c.size()
				&& IsLower(after[c.size()])) {
				continue;
			}
			return true;
		}
	}
	auto before = s.substr(0, from);
	if (!before.empty() && before.back() == U' ') {
		before.remove_suffix(1);
	}
	for (const auto c : Tables::kCurrencyBefore32) {
		if (EndsWith(before, c)) {
			if (IsAsciiWord(c)
				&& before.size() > c.size()
				&& IsLower(before[before.size() - c.size() - 1])) {
				continue;
			}
			return true;
		}
	}
	return false;
}

[[nodiscard]] bool PhoneExempt(View s, std::size_t from, std::size_t till) {
	if (PriceContext(s, from, till)) {
		return true;
	}
	const auto window = WindowBefore(s, from);
	return std::any_of(
		std::begin(Tables::kPhoneOrderContextCjk32),
		std::end(Tables::kPhoneOrderContextCjk32),
		[&](View w) { return Contains(window, w); });
}

[[nodiscard]] bool OrderAsciiInWindow(View window, View word) {
	auto from = std::size_t(0);
	while (true) {
		const auto i = window.find(word, from);
		if (i == kNpos) {
			return false;
		}
		from = i + 1;
		const auto till = i + word.size();
		if (i > 0 && IsLower(window[i - 1])) {
			continue;
		}
		if (till < window.size() && IsLower(window[till])) {
			continue;
		}
		return true;
	}
}

[[nodiscard]] bool IsDate8(View run) {
	const auto d = [&](std::size_t i) {
		return int(run[i] - U'0');
	};
	const auto century = d(0) * 10 + d(1);
	const auto month = d(4) * 10 + d(5);
	const auto day = d(6) * 10 + d(7);
	return (century == 19 || century == 20)
		&& (month >= 1 && month <= 12)
		&& (day >= 1 && day <= 31);
}

[[nodiscard]] std::vector<String> LongNumbers(View s) {
	auto result = std::vector<String>();
	auto i = std::size_t(0);
	while (i < s.size()) {
		if (!IsDigit(s[i])) {
			++i;
			continue;
		}
		const auto from = i;
		const auto till = RunEnd(s, i, IsDigit);
		i = till;
		if (till - from < 6) {
			continue;
		}
		const auto run = s.substr(from, till - from);
		if (run.size() == 8 && IsDate8(run)) {
			continue;
		}
		if (PriceContext(s, from, till)) {
			continue;
		}
		const auto window = WindowBefore(s, from);
		const auto cjk = std::any_of(
			std::begin(Tables::kOrderContextCjk32),
			std::end(Tables::kOrderContextCjk32),
			[&](View w) { return Contains(window, w); });
		if (cjk) {
			continue;
		}
		const auto ascii = std::any_of(
			std::begin(Tables::kOrderContextAscii32),
			std::end(Tables::kOrderContextAscii32),
			[&](View w) { return OrderAsciiInWindow(window, w); });
		if (ascii) {
			continue;
		}
		result.emplace_back(run);
	}
	return result;
}

[[nodiscard]] String WordForm(View s) {
	auto result = String(U" ");
	auto pending = false;
	for (const auto c : s) {
		if (IsAlnum(c)) {
			if (pending && result.size() > 1) {
				result.push_back(U' ');
			}
			pending = false;
			result.push_back(c);
		} else {
			pending = true;
		}
	}
	result.push_back(U' ');
	return result;
}

[[nodiscard]] bool PointerHitOk(View tc, View p, std::size_t i) {
	if (const auto chars = Lookup(
			Tables::kPointerNotAfterPhrase32,
			Tables::kPointerNotAfterChars32,
			p)) {
		if (!chars->empty() && i > 0 && IsOneOf(tc[i - 1], *chars)) {
			return false;
		}
	}
	if (const auto chars = Lookup(
			Tables::kPointerNotBeforePhrase32,
			Tables::kPointerNotBeforeChars32,
			p)) {
		const auto after = tc.substr(i + p.size());
		if (!chars->empty() && !after.empty() && IsOneOf(after[0], *chars)) {
			const auto ok = std::any_of(
				std::begin(Tables::kPointerBeforeOk32),
				std::end(Tables::kPointerBeforeOk32),
				[&](View w) { return StartsWith(after, w); });
			if (!ok) {
				return false;
			}
		}
	}
	const auto from = (i > kPointerNegWindow) ? (i - kPointerNegWindow) : 0;
	const auto pre = tc.substr(from, i - from);
	for (const auto negation : Tables::kPointerNegation32) {
		auto k = pre.find(negation);
		while (k != kNpos) {
			const auto between = pre.substr(k + negation.size());
			const auto broken = std::any_of(
				std::begin(Tables::kPointerNegationBreak32),
				std::end(Tables::kPointerNegationBreak32),
				[&](View w) { return Contains(between, w); });
			if (!broken) {
				return false;
			}
			k = pre.find(negation, k + 1);
		}
	}
	return true;
}

[[nodiscard]] bool HasPointer(View foldNoLinks) {
	const auto tc = Tables::CjkForm(Tables::TextForm(foldNoLinks));
	for (const auto p : Tables::kPointerCjk32) {
		auto from = std::size_t(0);
		while (true) {
			const auto i = tc.find(p, from);
			if (i == kNpos) {
				break;
			}
			from = i + 1;
			if (PointerHitOk(tc, p, i)) {
				return true;
			}
		}
	}
	const auto words = WordForm(foldNoLinks);
	for (const auto p : Tables::kPointerAscii32) {
		if (Contains(words, U" " + String(p) + U" ")) {
			return true;
		}
	}
	return false;
}

struct AccountSpan {
	std::size_t from = 0;
	std::size_t till = 0;
	Link link;
};

// SPEC 4.6: contact keyword + account, phone keyword + number.
void ContactAccounts(
		View s,
		std::vector<Link> &links,
		std::vector<Span> &spans) {
	auto found = std::vector<AccountSpan>();
	const auto add = [&](std::size_t from, std::size_t till, Link link) {
		for (const auto &existing : found) {
			if (existing.from == from && existing.till == till) {
				return;
			}
		}
		found.push_back({ from, till, std::move(link) });
	};
	const auto count = std::size(Tables::kKeywordWords32);
	for (std::size_t k = 0; k != count; ++k) {
		const auto word = Tables::kKeywordWords32[k];
		const auto cls = Tables::kKeywordClasses32[k];
		const auto kind = Tables::kKeywordKinds32[k];
		const auto asciiWord = IsAllAscii(word);
		auto start = std::size_t(0);
		while (true) {
			const auto i = s.find(word, start);
			if (i == kNpos) {
				break;
			}
			start = i + 1;
			const auto j = i + word.size();
			if (asciiWord) {
				if (i > 0 && IsAlnum(s[i - 1])) {
					continue;
				}
				if (j < s.size() && IsLower(s[j])) {
					continue;
				}
			}
			if (kind == U"prefixed") {
				auto q = std::ptrdiff_t(i) - 1;
				while (q >= 0 && s[q] == U' ') {
					--q;
				}
				if (q >= 0 && (s[q] == U'\u6211' || s[q] == U'\u4E2A')) {
					--q;
					while (q >= 0 && s[q] == U' ') {
						--q;
					}
				}
				if (q < 0 || (s[q] != U'\u52A0' && s[q] != U'+')) {
					continue;
				}
			}
			auto g = j;
			auto gap = String();
			while (g < s.size()
				&& gap.size() < kGapMax
				&& s[g] != U'\n'
				&& !IsAlnum(s[g])) {
				gap.push_back(s[g]);
				++g;
			}
			if (kind == U"phone_cjk" || kind == U"phone_ascii") {
				const auto till = MatchDigitRun(s, g, 6, 14);
				if (!till) {
					continue;
				}
				add(g, *till, MakeLink(
					U"tel:" + Digits(s.substr(g, *till - g)),
					LinkKind::Phone,
					U"PHONE",
					LinkSource::Text));
				continue;
			}
			if (g >= s.size() || !IsAlnum(s[g])) {
				continue;
			}
			const auto run = RunEnd(s, g + 1, IsAccountChar) - (g + 1);
			if (run < 3) {
				continue;
			}
			const auto till = g + 1 + std::min(run, std::size_t(29));
			if (till < s.size() && IsAlnum(s[till])) {
				continue;
			}
			const auto account = RightStrip(s.substr(g, till - g), U".-");
			const auto allDigits = IsDigits(account);
			if (account.size() < 4 || (allDigits && account.size() < 5)) {
				continue;
			}
			const auto hasAlpha = std::any_of(
				account.begin(),
				account.end(),
				IsLower);
			const auto hasUnderscore = Contains(account, U"_");
			if (!allDigits && !hasAlpha && !hasUnderscore) {
				continue;
			}
			const auto gapMark = std::any_of(gap.begin(), gap.end(), [](
					char32_t c) {
				return (c == U':') || (c == U'=') || (c == U'@');
			});
			if (kind == U"standalone_ascii") {
				const auto hasDigit = std::any_of(
					account.begin(),
					account.end(),
					IsDigit);
				const auto ok = gapMark
					|| hasUnderscore
					|| (hasAlpha && hasDigit)
					|| (allDigits && account.size() >= 7);
				if (!ok) {
					continue;
				}
			}
			if (kind == U"standalone_strict") {
				if (!(gapMark || hasUnderscore) || !hasAlpha) {
					continue;
				}
			}
			add(g, g + account.size(), MakeLink(
				U"acct:" + String(cls) + U":" + account,
				LinkKind::Acct,
				U"ACCT:" + String(cls),
				LinkSource::Text));
		}
	}
	std::sort(found.begin(), found.end(), [](
			const AccountSpan &a,
			const AccountSpan &b) {
		return (a.from != b.from) ? (a.from < b.from) : (a.till > b.till);
	});
	auto lastEnd = std::size_t(0);
	for (auto &entry : found) {
		if (entry.from < lastEnd) {
			continue;
		}
		spans.push_back({ entry.from, entry.till });
		links.push_back(std::move(entry.link));
		lastEnd = entry.till;
	}
}

[[nodiscard]] std::vector<std::uint64_t> DedupeHashes(
		const std::vector<std::uint64_t> &hashes,
		std::size_t cap) {
	auto result = std::vector<std::uint64_t>();
	for (const auto h : hashes) {
		if (std::find(result.begin(), result.end(), h) == result.end()) {
			result.push_back(h);
			if (result.size() == cap) {
				break;
			}
		}
	}
	return result;
}

[[nodiscard]] bool IsAck(View text) {
	if (text.empty() || Contains(text, U"#")) {
		return false;
	}
	auto rest = String(text);
	auto hit = false;
	for (const auto phrase : Tables::kAckPhrases32) {
		if (Contains(rest, phrase)) {
			hit = true;
			ReplaceAll(rest, phrase);
		}
	}
	if (!hit) {
		return false;
	}
	for (const auto filler : Tables::kAckFillers32) {
		ReplaceAll(rest, filler);
	}
	return Tables::Wlen(rest) <= kAckRemainderMax;
}

[[nodiscard]] bool MediaKeyed(MediaKind kind) {
	return (kind == MediaKind::Photo)
		|| (kind == MediaKind::Video)
		|| (kind == MediaKind::Document)
		|| (kind == MediaKind::Audio);
}

[[nodiscard]] std::uint32_t StrongBit(LinkKind kind) {
	switch (kind) {
	case LinkKind::Invite: return Flag::kInvite;
	case LinkKind::Handle: return Flag::kHandle;
	case LinkKind::Url: return Flag::kShortener;
	case LinkKind::Phone: return Flag::kPhone;
	case LinkKind::Acct: return Flag::kAccount;
	case LinkKind::Mail: return 0;
	}
	return 0;
}

[[nodiscard]] std::vector<std::string> Names(
		std::uint32_t flags,
		const std::pair<std::uint32_t, const char*> *table,
		std::size_t size) {
	auto result = std::vector<std::string>();
	for (std::size_t i = 0; i != size; ++i) {
		if (flags & table[i].first) {
			result.emplace_back(table[i].second);
		}
	}
	return result;
}

// The link feature table of SPEC 5.1, in its order. A token is trusted when
// it is in trustedTokens and not in `untrusted` (v3: the author's own view of
// SPEC 6.12 leaves out the tokens that are trusted only because they are
// pinned).
struct FeatureInputs {
	const std::set<String> *trustedTokens = nullptr;
	const std::set<String> *untrusted = nullptr;
	const std::set<String> *trustedDomains = nullptr;
	bool channelForward = false;
	const String *forwardUsername = nullptr;
	const String *selfUsername = nullptr;
	const String *senderUsername = nullptr;
	const std::vector<String> *addressed = nullptr;
};

[[nodiscard]] LinkFeature FeatureOf(
		const Link &link,
		const FeatureInputs &in) {
	const auto trusted = in.trustedTokens->count(link.token)
		&& !(in.untrusted && in.untrusted->count(link.token));
	if (trusted) {
		return LinkFeature::NotCounted;
	} else if (link.kind == LinkKind::Invite) {
		return in.channelForward ? LinkFeature::Weak : LinkFeature::Strong;
	} else if (link.kind == LinkKind::Handle) {
		const auto h = StartsWith(link.token, U"@")
			? link.token.substr(1)
			: String();
		if (!h.empty() && h == *in.forwardUsername) {
			return LinkFeature::NotCounted;
		} else if (!h.empty() && h == *in.selfUsername) {
			return LinkFeature::NotCounted;
		} else if (in.channelForward) {
			return LinkFeature::Weak;
		} else if (!h.empty() && h == *in.senderUsername) {
			return LinkFeature::Strong;
		} else if (h.empty()) {
			return LinkFeature::Strong;
		} else if (std::binary_search(
				in.addressed->begin(),
				in.addressed->end(),
				h)) {
			return LinkFeature::Member;
		}
		return LinkFeature::Pending;
	} else if (link.kind == LinkKind::Url) {
		if (link.shortener) {
			return in.channelForward
				? LinkFeature::Weak
				: LinkFeature::Strong;
		} else if (in.trustedDomains->count(link.domain)) {
			return LinkFeature::NotCounted;
		}
		return LinkFeature::Weak;
	} else if (link.kind == LinkKind::Phone || link.kind == LinkKind::Acct) {
		return LinkFeature::Strong;
	} else if (link.kind == LinkKind::Mail) {
		return in.trustedDomains->count(link.domain)
			? LinkFeature::NotCounted
			: LinkFeature::Weak;
	}
	return LinkFeature::NotCounted;
}

[[nodiscard]] std::vector<View> SplitLines(View s) {
	// Keeps the empty pieces: "A\n\nB" is A, "", B (SPEC 2.7).
	auto result = std::vector<View>();
	auto from = std::size_t(0);
	while (true) {
		const auto till = s.find(U'\n', from);
		if (till == kNpos) {
			result.push_back(s.substr(from));
			return result;
		}
		result.push_back(s.substr(from, till - from));
		from = till + 1;
	}
}

} // namespace

ChatContext::ChatContext(ChatContextData data)
: _chatId(data.chatId)
, _type(data.type)
, _adminsKnown(data.adminsKnown)
, _linkedChannelId(data.linkedChannelId)
, _username(FoldUsername(data.username))
, _linkedChannelUsername(FoldUsername(data.linkedChannelUsername))
, _selfUsername(FoldUsername(data.selfUsername)) {
	_memberHandles.reserve(data.memberHandles.size());
	for (const auto &handle : data.memberHandles) {
		_memberHandles.push_back(FoldUsername(handle));
	}
	// `own`: the group's and the linked channel's usernames and what the
	// description has; `pinned`: what the pinned messages have (SPEC 6.12).
	auto own = std::set<String>();
	auto pinned = std::set<String>();
	for (const auto *handle : { &_username, &_linkedChannelUsername }) {
		if (!handle->empty()) {
			_trustedTokens.insert(U"@" + *handle);
			own.insert(U"@" + *handle);
		}
	}
	const auto add = [&](View text, std::set<String> &into) {
		auto extraction = ExtractLinks(Tables::Fold(text));
		for (auto &link : extraction.links) {
			NormalizePhoneToken(link);
			_trustedTokens.insert(link.token);
			into.insert(link.token);
			const auto urlOrMail = (link.kind == LinkKind::Url)
				|| (link.kind == LinkKind::Mail);
			if (urlOrMail && !link.shortener && !link.domain.empty()) {
				_trustedDomains.insert(link.domain);
			}
		}
	};
	add(data.description, own);
	for (const auto &text : data.pinnedTexts) {
		add(text, pinned);
	}
	for (const auto &token : pinned) {
		if (!own.count(token)) {
			_pinnedOnlyTokens.insert(token);
		}
	}
}

bool ChatContext::eligible() const {
	return (_type == ChatType::Group)
		|| (_type == ChatType::Supergroup)
		|| (_type == ChatType::Forum)
		|| (_type == ChatType::Gigagroup);
}

std::string ToUtf8(std::u32string_view s) {
	auto result = std::string();
	result.reserve(s.size());
	for (const auto cp : s) {
		if (cp < 0x80) {
			result.push_back(char(cp));
		} else if (cp < 0x800) {
			result.push_back(char(0xC0 | (cp >> 6)));
			result.push_back(char(0x80 | (cp & 0x3F)));
		} else if (cp < 0x10000) {
			result.push_back(char(0xE0 | (cp >> 12)));
			result.push_back(char(0x80 | ((cp >> 6) & 0x3F)));
			result.push_back(char(0x80 | (cp & 0x3F)));
		} else {
			result.push_back(char(0xF0 | ((cp >> 18) & 0x07)));
			result.push_back(char(0x80 | ((cp >> 12) & 0x3F)));
			result.push_back(char(0x80 | ((cp >> 6) & 0x3F)));
			result.push_back(char(0x80 | (cp & 0x3F)));
		}
	}
	return result;
}

std::string Hex64(std::uint64_t value) {
	char buffer[17] = { 0 };
	std::snprintf(
		buffer,
		sizeof(buffer),
		"%016llx",
		static_cast<unsigned long long>(value));
	return std::string(buffer, 16);
}

std::uint64_t Hash(
		const Key &key,
		std::string_view tag,
		std::string_view payloadUtf8) {
	return Tables::KeyedHash(key.data(), tag, payloadUtf8);
}

std::uint64_t Hash(
		const Key &key,
		std::string_view tag,
		std::u32string_view payload) {
	return Hash(key, tag, std::string_view(ToUtf8(payload)));
}

std::uint64_t HashBytes(
		const Key &key,
		std::string_view tag,
		const unsigned char *data,
		std::size_t size) {
	return Tables::KeyedHashBytes(key.data(), tag, data, size);
}

std::uint64_t HandleHash(const Key &key, std::u32string_view foldedName) {
	return Hash(key, "link", View(U"@" + String(foldedName)));
}

std::u32string FoldUsername(std::u32string_view username) {
	auto folded = Tables::Fold(username);
	auto from = std::size_t(0);
	while (from < folded.size() && folded[from] == U'@') {
		++from;
	}
	return folded.substr(from);
}

bool ValidUsername(std::u32string_view name) {
	if (name.size() < 4 || name.size() > 32 || !IsLower(name[0])) {
		return false;
	}
	return std::all_of(name.begin() + 1, name.end(), IsUserChar);
}

std::pair<int, int> LetterCounts(std::u32string_view text) {
	auto cjk = 0;
	auto other = 0;
	for (const auto c : text) {
		if (c == U'#') {
			continue;
		}
		if (Tables::IsCjk(c)) {
			++cjk;
		} else {
			++other;
		}
	}
	return { cjk, other };
}

std::vector<std::u32string> Paragraphs(std::u32string_view foldNoLinks) {
	auto result = std::vector<String>();
	auto from = std::size_t(0);
	while (from <= foldNoLinks.size()) {
		auto till = from;
		while (till < foldNoLinks.size()
			&& !IsOneOf(foldNoLinks[till], U"\n.!?;")) {
			++till;
		}
		auto tp = Tables::TextForm(foldNoLinks.substr(from, till - from));
		if (Tables::Wlen(tp) >= kParaMinWlen
			&& std::find(result.begin(), result.end(), tp) == result.end()) {
			result.push_back(std::move(tp));
			if (result.size() == kMaxPara) {
				break;
			}
		}
		from = till + 1;
	}
	return result;
}

std::u32string CollapseCrlf(std::u32string_view text) {
	auto result = String();
	result.reserve(text.size());
	for (std::size_t i = 0; i != text.size(); ++i) {
		if (text[i] == U'\r'
			&& i + 1 < text.size()
			&& text[i + 1] == U'\n') {
			continue;
		}
		result.push_back(text[i]);
	}
	return result;
}

std::vector<std::u32string> ContentLines(std::u32string_view foldNoLinks) {
	auto result = std::vector<String>();
	for (const auto line : SplitLines(foldNoLinks)) {
		auto t = Tables::TextForm(line);
		if (!t.empty()) {
			result.push_back(std::move(t));
		}
	}
	return result;
}

std::vector<std::pair<int, std::u32string>> VarKeys(
		const std::vector<std::u32string> &lines) {
	const auto n = lines.size();
	auto weights = std::vector<std::int64_t>();
	weights.reserve(n);
	auto total = std::int64_t(0);
	for (const auto &line : lines) {
		weights.push_back(Tables::Wlen(line));
		total += weights.back();
	}
	auto result = std::vector<std::pair<int, String>>();
	auto seen = std::set<String>();
	// Drops lines `first` and `second` (kNoLine for none).
	const auto emit = [&](std::size_t first, std::size_t second) {
		auto dropped = std::int64_t(0);
		for (const auto i : { first, second }) {
			if (i != kNoLine) {
				dropped += weights[i];
			}
		}
		const auto kept = total - dropped;
		if (kept < kVarMinWlen) {
			return;
		}
		auto kind = KeyKind::Var;
		if (dropped == 0) {
			kind = KeyKind::Var;
		} else if (kVarStrictRatio * dropped <= kept) {
			kind = KeyKind::VarS;
		} else if (kVarDropRatio * dropped <= kept) {
			kind = KeyKind::VarL;
		} else {
			return;
		}
		auto payload = String();
		for (std::size_t i = 0; i != n; ++i) {
			if (i != first && i != second) {
				payload += lines[i];
			}
		}
		if (seen.insert(payload).second) {
			result.emplace_back(int(kind), std::move(payload));
		}
	};
	emit(kNoLine, kNoLine);
	if (n >= 2) {
		for (std::size_t i = 0; i != n; ++i) {
			if (i < kVarEdge || i + kVarEdge >= n) {
				emit(i, kNoLine);
			}
		}
	}
	if (n >= 3) {
		emit(0, n - 1);
	}
	return result;
}

std::vector<std::u32string> Blocks(std::u32string_view foldNoLinks) {
	auto result = std::vector<String>();
	auto current = String();
	auto any = false;
	for (const auto line : SplitLines(foldNoLinks)) {
		auto t = Tables::TextForm(line);
		if (!t.empty()) {
			current += t;
			any = true;
		} else if (any) {
			result.push_back(std::exchange(current, String()));
			any = false;
		}
	}
	if (any) {
		result.push_back(std::move(current));
	}
	return result;
}

std::vector<std::u32string> BlockPayloads(
		const std::vector<std::u32string> &blocks) {
	auto result = std::vector<String>();
	for (const auto &block : blocks) {
		if (Tables::Wlen(block) >= kBlockMinWlen
			&& std::find(result.begin(), result.end(), block) == result.end()) {
			result.push_back(block);
			if (result.size() == kMaxBlock) {
				break;
			}
		}
	}
	if (result.size() < 2) {
		result.clear();
	}
	return result;
}

std::u32string RegisteredDomain(std::u32string_view host) {
	const auto numeric = !host.empty()
		&& std::all_of(host.begin(), host.end(), [](char32_t c) {
			return IsDigit(c) || (c == U'.');
		});
	if (numeric) {
		return String(host);
	}
	const auto labels = SplitNonEmpty(host, U'.');
	if (labels.size() <= 2) {
		return Join(labels, U".");
	}
	const auto n = labels.size();
	const auto lastTwo = labels[n - 2] + U"." + labels[n - 1];
	if (InList(Tables::kMultiSuffix32, lastTwo)) {
		return labels[n - 3] + U"." + lastTwo;
	}
	return lastTwo;
}

std::optional<Link> CanonicalizeUrl(
		std::u32string_view raw,
		LinkSource source) {
	auto from = std::size_t(0);
	auto till = raw.size();
	while (from < till && IsPySpace(raw[from])) {
		++from;
	}
	while (till > from && IsPySpace(raw[till - 1])) {
		--till;
	}
	auto u = String();
	for (auto i = from; i != till; ++i) {
		if (raw[i] != U' ') {
			u.push_back(raw[i]);
		}
	}
	while (!u.empty() && IsTrim(u.back())) {
		u.pop_back();
	}
	auto scheme = String();
	if (!u.empty() && IsLower(u[0])) {
		auto i = std::size_t(1);
		while (i < u.size()
			&& (IsAlnum(u[i]) || IsOneOf(u[i], U"+.-"))) {
			++i;
		}
		if (StartsWith(u, i, U"://")) {
			scheme = u.substr(0, i);
			u.erase(0, i + 3);
		}
	}
	if (scheme == U"tg") {
		const auto q = u.find(U'?');
		const auto host = Strip(View(u).substr(0, q), U'/');
		const auto query = ParseQuery((q == kNpos)
			? View()
			: View(u).substr(q + 1));
		if (host == U"user" || host == U"openmessage") {
			const auto uid = QueryValue(
				query,
				(host == U"user") ? U"id" : U"user_id");
			if (uid && IsDigits(*uid)) {
				return MakeLink(
					U"uid:" + *uid,
					LinkKind::Handle,
					U"HANDLE",
					source);
			}
		}
		if (host == U"resolve") {
			const auto domain = QueryValue(query, U"domain");
			if (domain && ValidUsername(*domain)) {
				return MakeLink(
					U"@" + *domain,
					LinkKind::Handle,
					U"HANDLE",
					source);
			}
		}
		if (host == U"join") {
			const auto invite = QueryValue(query, U"invite");
			if (invite && !invite->empty()) {
				return MakeLink(
					U"invite:" + *invite,
					LinkKind::Invite,
					U"INVITE",
					source);
			}
		}
		return MakeLink(
			U"url:tg/" + host,
			LinkKind::Url,
			U"URL:tg",
			source,
			U"tg");
	}
	auto cut = u.size();
	for (const auto c : View(U"/?#")) {
		const auto i = u.find(c);
		if (i != kNpos && i < cut) {
			cut = i;
		}
	}
	auto hostport = View(u).substr(0, cut);
	auto rest = View(u).substr(cut);
	const auto at = hostport.rfind(U'@');
	if (at != kNpos) {
		hostport = hostport.substr(at + 1);
	}
	auto host = Strip(hostport.substr(0, hostport.find(U':')), U'.');
	if (host.empty()) {
		return std::nullopt;
	}
	if (StartsWith(host, U"www.")) {
		host.erase(0, 4);
	}
	if (const auto alias = Lookup(
			Tables::kDomainAliasFrom32,
			Tables::kDomainAliasTo32,
			host)) {
		host = String(*alias);
	}
	rest = rest.substr(0, rest.find(U'#'));
	const auto q = rest.find(U'?');
	auto path = RightStrip(rest.substr(0, q), U"/");
	auto query = (q == kNpos) ? String() : String(rest.substr(q + 1));
	if (host == U"t.me") {
		return TmeLink(path, source);
	}
	if (EndsWith(host, U".t.me")) {
		const auto name = host.substr(0, host.size() - 5);
		if (ValidUsername(name)) {
			return MakeLink(U"@" + name, LinkKind::Handle, U"HANDLE", source);
		}
	}
	if (host == U"youtu.be") {
		const auto stripped = Strip(path, U'/');
		const auto video = stripped.substr(0, stripped.find(U'/'));
		host = U"youtube.com";
		path = U"/watch";
		query = U"v=" + video + (query.empty() ? String() : (U"&" + query));
	}
	if (host == U"wa.me") {
		const auto digits = Strip(path, U'/');
		if (IsDigits(digits)) {
			return PhoneLink(digits, source);
		}
	}
	if ((host == U"api.whatsapp.com" || host == U"whatsapp.com")
		&& path == U"/send") {
		const auto parsed = ParseQuery(query);
		const auto phone = QueryValue(parsed, U"phone");
		const auto digits = phone ? Digits(*phone) : String();
		if (!digits.empty()) {
			return PhoneLink(digits, source);
		}
	}
	const auto cleaned = CleanQuery(query);
	auto token = U"url:" + host + path;
	if (!cleaned.empty()) {
		token += U"?" + cleaned;
	}
	if (InList(Tables::kShorteners32, host)) {
		return MakeLink(
			std::move(token),
			LinkKind::Url,
			U"SHORT",
			source,
			host,
			true);
	}
	auto domain = RegisteredDomain(host);
	return MakeLink(
		std::move(token),
		LinkKind::Url,
		U"URL:" + domain,
		source,
		domain);
}

Extraction ExtractLinks(std::u32string_view folded) {
	auto result = Extraction();
	auto s = String(folded);
	for (const auto round : {
			Round::Scheme,
			Round::Tme,
			Round::TmeSub,
			Round::Email,
			Round::Domain }) {
		auto spans = std::vector<Span>();
		ForEachMatch(round, s, [&](const Match &match) {
			auto till = match.till;
			while (till > match.from && IsTrim(s[till - 1])) {
				--till;
			}
			if (till <= match.from) {
				return;
			}
			const auto raw = View(s).substr(match.from, till - match.from);
			if (round == Round::Email) {
				const auto at = raw.find(U'@');
				result.links.push_back(MakeLink(
					U"mail:" + String(raw),
					LinkKind::Mail,
					U"MAIL",
					LinkSource::Text,
					RegisteredDomain(raw.substr(at + 1))));
			} else {
				auto link = CanonicalizeUrl(raw);
				if (!link) {
					return;
				}
				result.links.push_back(std::move(*link));
			}
			spans.push_back({ match.from, till });
		});
		s = RemoveSpans(s, std::move(spans));
	}
	for (const auto round : { Round::Mention, Round::MentionSpace }) {
		auto spans = std::vector<Span>();
		ForEachMatch(round, s, [&](const Match &match) {
			auto name = s.substr(match.group, match.till - match.group);
			if (round == Round::MentionSpace) {
				const auto marked = std::any_of(
					name.begin(),
					name.end(),
					[](char32_t c) { return (c == U'_') || IsDigit(c); });
				if (!marked) {
					return;
				}
			}
			result.links.push_back(MakeLink(
				U"@" + name,
				LinkKind::Handle,
				U"HANDLE",
				LinkSource::Text));
			result.mentions.push_back(std::move(name));
			spans.push_back({ match.from, match.till });
		});
		s = RemoveSpans(s, std::move(spans));
	}
	for (const auto round : {
			Round::PhoneIntl,
			Round::PhoneCn,
			Round::PhoneTw }) {
		auto spans = std::vector<Span>();
		ForEachMatch(round, s, [&](const Match &match) {
			if (PhoneExempt(s, match.from, match.till)) {
				return;
			}
			auto digits = Digits(View(s).substr(
				match.from,
				match.till - match.from));
			if (round == Round::PhoneCn) {
				digits = U"86" + digits;
			} else if (round == Round::PhoneTw) {
				digits = U"886" + digits.substr(1);
			}
			result.links.push_back(MakeLink(
				U"tel:" + digits,
				LinkKind::Phone,
				U"PHONE",
				LinkSource::Text));
			spans.push_back({ match.from, match.till });
		});
		s = RemoveSpans(s, std::move(spans));
	}
	{
		auto spans = std::vector<Span>();
		ContactAccounts(s, result.links, spans);
		s = RemoveSpans(s, std::move(spans));
	}
	result.pointer = HasPointer(s);
	result.longNumbers = LongNumbers(s);
	result.foldNoLinks = std::move(s);
	return result;
}

std::vector<std::u32string> LeadingHandles(std::u32string_view folded) {
	auto result = std::vector<String>();
	auto q = std::size_t(0);
	while (q < folded.size() && (folded[q] == U' ' || folded[q] == U'\n')) {
		++q;
	}
	while (q + 1 < folded.size()
		&& folded[q] == U'@'
		&& IsLower(folded[q + 1])) {
		const auto till = RunEnd(folded, q + 2, IsUserChar);
		const auto length = till - (q + 2);
		if (length < 3 || length > 31) {
			break;
		}
		result.emplace_back(folded.substr(q + 1, till - (q + 1)));
		q = till;
		while (q < folded.size() && IsOneOf(folded[q], U" \n,:")) {
			++q;
		}
	}
	std::sort(result.begin(), result.end());
	result.erase(std::unique(result.begin(), result.end()), result.end());
	return result;
}

bool EligibleMessage(const MessageInput &msg) {
	return !(msg.service || msg.sponsored || msg.scheduled);
}

bool ValidMessage(const MessageInput &msg) {
	return (msg.id > 0)
		&& (msg.id <= kMaxMsgId)
		&& (msg.date >= 0)
		&& (msg.date <= kMaxDate);
}

FingerprintResult Fingerprint(
		const MessageInput &msg,
		const ChatContext &context,
		const Key &key,
		const std::vector<std::u32string> &knownHandles) {
	auto r = FingerprintResult();
	const auto role = msg.sender.role;
	const auto senderUsername = FoldUsername(msg.sender.username);
	const auto forward = msg.forward ? &*msg.forward : nullptr;
	const auto channelForward = forward
		&& (forward->fromKind == ForwardKind::Channel);
	const auto mediaKind = msg.media.kind;

	r.fold = Tables::Fold(CollapseCrlf(msg.text));
	auto extraction = ExtractLinks(r.fold);
	auto links = std::move(extraction.links);
	for (const auto &entity : msg.entities) {
		if (entity.type == EntityType::MentionName
			&& entity.hasUserId
			&& entity.userId == msg.sender.id) {
			links.push_back(MakeLink(
				U"uid:" + Dec(entity.userId),
				LinkKind::Handle,
				U"HANDLE",
				LinkSource::Hidden));
		}
		if (entity.type == EntityType::TextUrl && !entity.url.empty()) {
			if (auto link = CanonicalizeUrl(
					Tables::Fold(entity.url),
					LinkSource::Hidden)) {
				links.push_back(std::move(*link));
			}
		}
	}
	auto hasButtons = false;
	for (const auto &button : msg.buttons) {
		if (button.empty()) {
			continue;
		}
		hasButtons = true;
		if (auto link = CanonicalizeUrl(
				Tables::Fold(button),
				LinkSource::Button)) {
			links.push_back(std::move(*link));
		}
	}
	if (mediaKind == MediaKind::Contact && !msg.media.phone.empty()) {
		if (auto link = PhoneLink(
				Digits(Tables::Fold(msg.media.phone)),
				LinkSource::Media)) {
			links.push_back(std::move(*link));
		}
	}
	for (auto &link : links) {
		NormalizePhoneToken(link);
	}

	r.foldNoLinks = std::move(extraction.foldNoLinks);
	r.text = Tables::TextForm(r.foldNoLinks);
	r.cjk = Tables::CjkForm(r.text);
	r.wlen = Tables::Wlen(r.text);
	const auto [cjkCount, otherLetterCount] = LetterCounts(r.text);
	r.cjkCount = cjkCount;
	r.otherLetterCount = otherLetterCount;
	r.paragraphs = Paragraphs(r.foldNoLinks);
	r.contentLines = ContentLines(r.foldNoLinks);
	r.blocks = Blocks(r.foldNoLinks);

	const auto &trustedTokens = context.trustedTokens();
	const auto &trustedDomains = context.trustedDomains();
	const auto forwardUsername = forward
		? FoldUsername(forward->username)
		: String();
	const auto addressed = LeadingHandles(r.fold);
	auto features = FeatureInputs();
	features.trustedTokens = &trustedTokens;
	features.trustedDomains = &trustedDomains;
	features.channelForward = channelForward;
	features.forwardUsername = &forwardUsername;
	features.selfUsername = &context.selfUsername();
	features.senderUsername = &senderUsername;
	features.addressed = &addressed;
	auto strong = std::uint32_t(0);
	auto weak = std::uint32_t(0);
	r.linkFeatures.reserve(links.size());
	for (const auto &link : links) {
		const auto feature = FeatureOf(link, features);
		r.linkFeatures.push_back(feature);
		if (feature == LinkFeature::Strong) {
			strong |= StrongBit(link.kind);
		} else if (feature == LinkFeature::Weak) {
			weak |= (link.kind == LinkKind::Mail) ? Flag::kEmail : Flag::kUrl;
		} else if (feature == LinkFeature::Member) {
			weak |= Flag::kMemberMention;
		}
	}
	if (extraction.pointer) {
		strong |= Flag::kPointer;
	}
	if (hasButtons) {
		if (channelForward) {
			weak |= Flag::kUrl;
		} else {
			strong |= Flag::kButton;
		}
	}
	if (msg.viaBot) {
		if (channelForward) {
			weak |= Flag::kUrl;
		} else {
			strong |= Flag::kViaBot;
		}
	}
	if (mediaKind == MediaKind::Contact) {
		strong |= Flag::kContactCard;
	}
	if (!extraction.longNumbers.empty()) {
		weak |= Flag::kLongNumber;
	}
	auto mentionIds = std::set<String>(
		extraction.mentions.begin(),
		extraction.mentions.end());
	for (const auto &entity : msg.entities) {
		if (entity.type == EntityType::MentionName && entity.hasUserId) {
			mentionIds.insert(U"#" + Dec(entity.userId));
		}
	}
	r.mentionCount = int(mentionIds.size());
	if (mentionIds.size() >= kMassMention) {
		weak |= Flag::kMassMention;
	}
	auto pendingTokens = std::vector<String>();
	for (std::size_t i = 0; i != links.size(); ++i) {
		if (r.linkFeatures[i] == LinkFeature::Pending) {
			pendingTokens.push_back(links[i].token);
		}
	}

	auto keys = std::vector<std::pair<KeyKind, String>>();
	auto mediaPayload = String();
	if (MediaKeyed(mediaKind) && !msg.media.id.empty()) {
		mediaPayload = String((mediaKind == MediaKind::Photo) ? U"p:" : U"d:")
			+ FromUtf8(msg.media.id);
	} else if (mediaKind == MediaKind::Contact) {
		if (msg.media.userId != 0) {
			mediaPayload = U"u:" + Dec(msg.media.userId);
		} else if (!msg.media.phone.empty()) {
			mediaPayload = U"tel:" + Digits(Tables::Fold(msg.media.phone));
		}
	}
	if (forward) {
		const auto fromId = forward->fromId;
		const auto postId = forward->postId;
		if (channelForward && fromId != 0 && postId != 0) {
			keys.emplace_back(
				KeyKind::Fwd,
				U"c|" + Dec(fromId) + U"|" + Dec(postId));
		} else {
			const auto who = (fromId != 0)
				? Dec(fromId)
				: (U"n:" + Tables::TextForm(Tables::Fold(forward->fromName)));
			const auto content = !r.text.empty()
				? (U"t:" + r.text)
				: !mediaPayload.empty()
				? (U"m:" + mediaPayload)
				: String(U"e");
			keys.emplace_back(
				KeyKind::Fwd,
				U"u|" + who + U"|" + Dec(forward->date) + U"|" + content);
		}
	}
	if (!mediaPayload.empty()) {
		keys.emplace_back(KeyKind::Media, mediaPayload);
	}
	if (r.wlen >= kMinTextWlen) {
		keys.emplace_back(KeyKind::Text, r.text);
	}
	if (r.cjkCount >= kMinCjkCount) {
		keys.emplace_back(KeyKind::Cjk, r.cjk);
	}
	auto tmplCond = false;
	if ((strong != 0 || !pendingTokens.empty()) && r.wlen >= kMinTmplWlen) {
		auto types = std::set<String>();
		for (const auto &link : links) {
			types.insert(StartsWith(link.type, U"URL:") ? U"URL" : link.type);
		}
		if (hasButtons) {
			types.insert(U"BUTTON");
		}
		if (msg.viaBot) {
			types.insert(U"VIABOT");
		}
		if (mediaKind == MediaKind::Contact) {
			types.insert(U"CONTACT");
		}
		keys.emplace_back(
			KeyKind::Tmpl,
			r.text + U"\x1e" + Join(std::vector<String>(types.begin(), types.end()), U","));
		tmplCond = (strong == 0);
	}
	auto contactTokens = std::set<String>();
	for (std::size_t i = 0; i != links.size(); ++i) {
		const auto kind = links[i].kind;
		if (r.linkFeatures[i] == LinkFeature::Strong
			&& (kind == LinkKind::Invite
				|| kind == LinkKind::Phone
				|| kind == LinkKind::Acct)) {
			contactTokens.insert(links[i].token);
		}
	}
	if (!contactTokens.empty()) {
		keys.emplace_back(
			KeyKind::Contact,
			Join(
				std::vector<String>(contactTokens.begin(), contactTokens.end()),
				U","));
	}
	for (const auto &paragraph : r.paragraphs) {
		keys.emplace_back(KeyKind::Para, paragraph);
	}
	// v3: the edit-tolerant keys come last; the v2 keys and their order do
	// not change.
	for (auto &[kind, payload] : VarKeys(r.contentLines)) {
		keys.emplace_back(KeyKind(kind), std::move(payload));
	}
	for (auto &payload : BlockPayloads(r.blocks)) {
		keys.emplace_back(KeyKind::Block, std::move(payload));
	}
	r.keys.reserve(keys.size());
	for (auto &[kind, payload] : keys) {
		const auto hash = Hash(key, KeyKindTag(kind), View(payload));
		r.keys.push_back({ kind, std::move(payload), hash });
	}

	auto subset = std::vector<std::uint64_t>();
	for (std::size_t i = 0; i != links.size(); ++i) {
		const auto feature = r.linkFeatures[i];
		if (feature == LinkFeature::Strong || feature == LinkFeature::Weak) {
			subset.push_back(Hash(key, "link", View(links[i].token)));
		}
	}
	if (extraction.pointer) {
		subset.push_back(Hash(key, "link", std::string_view("ptr:")));
	}
	if (msg.viaBot) {
		subset.push_back(Hash(key, "link", std::string_view("via:")));
	}
	r.linkHashes = DedupeHashes(subset, kMaxLinksPerRow);
	auto pendingHashes = std::vector<std::uint64_t>();
	pendingHashes.reserve(pendingTokens.size());
	for (const auto &token : pendingTokens) {
		pendingHashes.push_back(Hash(key, "link", View(token)));
	}
	r.pending = DedupeHashes(pendingHashes, kMaxPendingPerRow);

	const auto endorsingRole = (role == Role::Admin)
		|| (role == Role::Creator)
		|| (role == Role::AnonAdmin)
		|| (role == Role::Self);
	const auto linked = context.linkedChannelId();
	// Forwarding a user's or a hidden sender's message endorses nothing.
	const auto forwardOther = forward && !channelForward;
	const auto sentAsLinked = (role == Role::Channel)
		&& (linked != 0)
		&& (msg.sender.id == linked);
	r.endorsing = (endorsingRole && !forwardOther)
		|| msg.pinned
		|| msg.autoForward
		|| (forward && linked != 0 && forward->fromId == linked)
		|| sentAsLinked;

	// v3 (SPEC 6.12): a privileged author - an admin, the creator or an
	// anonymous admin (not forwarding someone else's message), the linked
	// channel itself, or its automatic forward. Never self; a pinned member
	// message and a member forwarding a channel post are not privileged.
	const auto privilegedRole = (role == Role::Admin)
		|| (role == Role::Creator)
		|| (role == Role::AnonAdmin);
	r.privileged = (role != Role::Self)
		&& ((privilegedRole && !forwardOther)
			|| msg.autoForward
			|| sentAsLinked);
	// A forward of the linked channel's post: its author is the channel.
	const auto ownForward = channelForward
		&& (linked != 0)
		&& (forward->fromId == linked);
	// The author's own strong features: links that are trusted only because
	// they are pinned, or demoted only because the post is the linked
	// channel's own, are strong again from the author's point of view.
	auto privStrong = false;
	if (r.privileged) {
		auto own = features;
		own.untrusted = &context.pinnedOnlyTokens();
		own.channelForward = channelForward && !ownForward;
		for (std::size_t i = 0; i != links.size(); ++i) {
			const auto feature = r.linkFeatures[i];
			if (feature == LinkFeature::Strong
				|| feature == LinkFeature::Pending) {
				continue;
			}
			const auto mine = FeatureOf(links[i], own);
			if (mine == LinkFeature::Strong || mine == LinkFeature::Pending) {
				privStrong = true;
			}
		}
		if (ownForward && (hasButtons || msg.viaBot)) {
			privStrong = true;
		}
	}
	auto flags = strong | weak;
	if (r.wlen >= kMinTextWlen) {
		flags |= Flag::kHasText;
	}
	for (const auto &k : r.keys) {
		if (k.kind == KeyKind::Media) {
			flags |= Flag::kHasMediaKey;
		} else if (k.kind == KeyKind::Fwd) {
			flags |= Flag::kHasFwdKey;
		}
	}
	if (tmplCond) {
		flags |= Flag::kTmplCond;
	}
	const auto pure = (r.cjkCount >= kPureTextMinCjk)
		|| (r.otherLetterCount >= kPureTextMinLetters);
	if (pure && !Contains(r.text, U"#")) {
		flags |= Flag::kPureText;
	}
	if (msg.replyTo != 0) {
		flags |= Flag::kIsReply;
		if (IsAck(r.text)) {
			flags |= Flag::kAck;
		}
	}
	if (msg.replyToMe) {
		flags |= Flag::kReplyMe;
	}
	if (msg.mentionsMe) {
		flags |= Flag::kMentionMe;
	}
	if (r.endorsing) {
		flags |= Flag::kEndorsing;
	}
	if (r.privileged) {
		flags |= Flag::kPriv;
	}
	if (privStrong) {
		flags |= Flag::kPrivStrong;
	}
	if (role == Role::Bot) {
		flags |= Flag::kBot;
	}
	if (role == Role::Contact) {
		flags |= Flag::kContact;
	}
	if (role == Role::Self) {
		flags |= Flag::kSelf;
	}
	r.flags = flags;
	const auto storedMarks = Flag::kHasMediaKey
		| Flag::kHasFwdKey
		| Flag::kPureText;
	r.store = !r.keys.empty()
		&& ((strong | weak) != 0
			|| !r.pending.empty()
			|| (flags & storedMarks) != 0
			|| r.endorsing);
	// The author: the linked channel when a privileged author forwards its
	// post (an admin forwarding it once more is the channel's own repeat).
	const auto authorId = (r.privileged && ownForward)
		? linked
		: msg.sender.id;
	r.senderKey = Hash(key, "sender", View(Dec(authorId)));
	r.links = std::move(links);

	auto known = std::vector<std::uint64_t>();
	for (const auto &name : knownHandles) {
		known.push_back(HandleHash(key, name));
	}
	for (const auto &name : context.memberHandles()) {
		known.push_back(HandleHash(key, name));
	}
	std::sort(known.begin(), known.end());
	auto refs = std::vector<KeyRef>();
	refs.reserve(r.keys.size());
	for (const auto &k : r.keys) {
		refs.push_back({ k.kind, k.hash });
	}
	r.resolvedFlags = Resolve(
		r.flags,
		refs,
		r.linkHashes,
		r.pending,
		[&](std::uint64_t h) {
			return std::binary_search(known.begin(), known.end(), h);
		}).flags;
	return r;
}

ResolveResult Resolve(
		std::uint32_t flags,
		const std::vector<KeyRef> &keys,
		const std::vector<std::uint64_t> &links,
		const std::vector<std::uint64_t> &pending,
		const IsKnownHandle &isKnown) {
	auto result = ResolveResult();
	auto unknown = std::vector<std::uint64_t>();
	for (const auto h : pending) {
		if (!isKnown(h)) {
			unknown.push_back(h);
		}
	}
	auto f = flags & ~Flag::kMediaOnly;
	if (!unknown.empty()) {
		f |= Flag::kHandle;
	}
	if (unknown.size() < pending.size()) {
		f |= Flag::kMemberMention;
	}
	if ((flags & Flag::kTmplCond) && unknown.empty()) {
		result.keys.reserve(keys.size());
		for (const auto &k : keys) {
			if (k.kind != KeyKind::Tmpl) {
				result.keys.push_back(k);
			}
		}
	} else {
		result.keys = keys;
	}
	result.links = links;
	for (const auto h : unknown) {
		if (std::find(links.begin(), links.end(), h) == links.end()) {
			result.links.push_back(h);
		}
	}
	if ((f & Flag::kHasMediaKey) && !(f & Flag::kStrongMask)) {
		f |= Flag::kMediaOnly;
	}
	result.flags = f;
	return result;
}

std::vector<std::string> StrongNames(std::uint32_t flags) {
	static constexpr std::pair<std::uint32_t, const char*> kTable[] = {
		{ Flag::kAccount, "account" },
		{ Flag::kButton, "button" },
		{ Flag::kContactCard, "contact_card" },
		{ Flag::kHandle, "handle" },
		{ Flag::kInvite, "invite" },
		{ Flag::kPhone, "phone" },
		{ Flag::kPointer, "pointer" },
		{ Flag::kShortener, "shortener" },
		{ Flag::kViaBot, "via_bot" },
	};
	return Names(flags, kTable, std::size(kTable));
}

std::vector<std::string> WeakNames(std::uint32_t flags) {
	static constexpr std::pair<std::uint32_t, const char*> kTable[] = {
		{ Flag::kEmail, "email" },
		{ Flag::kLongNumber, "long_number" },
		{ Flag::kMassMention, "mass_mention" },
		{ Flag::kMemberMention, "member_mention" },
		{ Flag::kUrl, "url" },
	};
	return Names(flags, kTable, std::size(kTable));
}

std::string_view KeyKindName(KeyKind kind) {
	switch (kind) {
	case KeyKind::Fwd: return "FWD";
	case KeyKind::Media: return "MEDIA";
	case KeyKind::Text: return "TEXT";
	case KeyKind::Cjk: return "CJK";
	case KeyKind::Tmpl: return "TMPL";
	case KeyKind::Para: return "PARA";
	case KeyKind::Contact: return "CONTACT";
	case KeyKind::Var: return "VAR";
	case KeyKind::VarL: return "VARL";
	case KeyKind::VarS: return "VARS";
	case KeyKind::Block: return "BLOCK";
	}
	return "";
}

std::string_view KeyKindTag(KeyKind kind) {
	switch (kind) {
	case KeyKind::Fwd: return "fwd";
	case KeyKind::Media: return "media";
	case KeyKind::Text: return "text";
	case KeyKind::Cjk: return "cjk";
	case KeyKind::Tmpl: return "tmpl";
	case KeyKind::Para: return "para";
	case KeyKind::Contact: return "contact";
	case KeyKind::Var:
	case KeyKind::VarL:
	case KeyKind::VarS: return "var";
	case KeyKind::Block: return "block";
	}
	return "";
}

std::string_view LinkKindName(LinkKind kind) {
	switch (kind) {
	case LinkKind::Invite: return "invite";
	case LinkKind::Handle: return "handle";
	case LinkKind::Url: return "url";
	case LinkKind::Phone: return "phone";
	case LinkKind::Acct: return "acct";
	case LinkKind::Mail: return "mail";
	}
	return "";
}

std::string_view LinkSourceName(LinkSource source) {
	switch (source) {
	case LinkSource::Text: return "text";
	case LinkSource::Hidden: return "hidden";
	case LinkSource::Button: return "button";
	case LinkSource::Media: return "media";
	}
	return "";
}

std::string_view LinkFeatureName(LinkFeature feature) {
	switch (feature) {
	case LinkFeature::NotCounted: return "none";
	case LinkFeature::Strong: return "strong";
	case LinkFeature::Weak: return "weak";
	case LinkFeature::Member: return "member";
	case LinkFeature::Pending: return "pending";
	}
	return "";
}

} // namespace Lumina::AdShield
