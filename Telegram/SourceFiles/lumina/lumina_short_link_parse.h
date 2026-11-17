/*
This file is part of LuminaGram,
a fork of Telegram Desktop.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

// Pure string helpers for the short-link resolver (lumina_short_link.h) and
// its confirmation box. Standard C++ only - no Qt, no tdesktop headers - so
// they can be compiled and tested on their own. They never log, and they see
// nothing but the bytes they are handed.
namespace Lumina::ShortLinkParse {

namespace details {

[[nodiscard]] inline char Lower(char c) {
	return (c >= 'A' && c <= 'Z') ? char(c - 'A' + 'a') : c;
}

[[nodiscard]] inline bool IsSpace(char c) {
	return (c == ' ')
		|| (c == '\t')
		|| (c == '\n')
		|| (c == '\r')
		|| (c == '\f');
}

[[nodiscard]] inline bool IsDigit(char c) {
	return (c >= '0' && c <= '9');
}

// `prefix` must be lower-case.
[[nodiscard]] inline bool StartsWithNoCase(
		std::string_view text,
		std::size_t at,
		std::string_view prefix) {
	if (at > text.size() || text.size() - at < prefix.size()) {
		return false;
	}
	for (std::size_t i = 0; i != prefix.size(); ++i) {
		if (Lower(text[at + i]) != prefix[i]) {
			return false;
		}
	}
	return true;
}

// `needle` must be lower-case.
[[nodiscard]] inline std::size_t FindNoCase(
		std::string_view text,
		std::string_view needle,
		std::size_t from) {
	if (needle.empty() || from > text.size()) {
		return std::string_view::npos;
	}
	for (auto i = from; text.size() - i >= needle.size(); ++i) {
		if (StartsWithNoCase(text, i, needle)) {
			return i;
		}
	}
	return std::string_view::npos;
}

[[nodiscard]] inline std::string_view Trimmed(std::string_view text) {
	while (!text.empty() && IsSpace(text.front())) {
		text.remove_prefix(1);
	}
	while (!text.empty() && IsSpace(text.back())) {
		text.remove_suffix(1);
	}
	return text;
}

inline void AppendUtf8(std::string &to, std::uint32_t code) {
	if (code < 0x80) {
		to += char(code);
	} else if (code < 0x800) {
		to += char(0xC0 | (code >> 6));
		to += char(0x80 | (code & 0x3F));
	} else if (code < 0x10000) {
		to += char(0xE0 | (code >> 12));
		to += char(0x80 | ((code >> 6) & 0x3F));
		to += char(0x80 | (code & 0x3F));
	} else {
		to += char(0xF0 | (code >> 18));
		to += char(0x80 | ((code >> 12) & 0x3F));
		to += char(0x80 | ((code >> 6) & 0x3F));
		to += char(0x80 | (code & 0x3F));
	}
}

// The character references an address in an attribute value can carry: the
// five named ones and the numeric forms. Anything else is left as written.
[[nodiscard]] inline std::string DecodeEntities(std::string_view value) {
	auto result = std::string();
	result.reserve(value.size());
	auto i = std::size_t(0);
	while (i < value.size()) {
		if (value[i] != '&') {
			result += value[i++];
			continue;
		}
		const auto semicolon = value.find(';', i + 1);
		if (semicolon == std::string_view::npos || semicolon - i > 12) {
			result += value[i++];
			continue;
		}
		const auto name = value.substr(i + 1, semicolon - i - 1);
		auto code = std::uint32_t(0);
		if (name == "amp") {
			code = '&';
		} else if (name == "quot") {
			code = '"';
		} else if (name == "apos") {
			code = '\'';
		} else if (name == "lt") {
			code = '<';
		} else if (name == "gt") {
			code = '>';
		} else if (name.size() > 1 && name[0] == '#') {
			const auto hex = (Lower(name[1]) == 'x');
			const auto digits = name.substr(hex ? 2 : 1);
			auto valid = !digits.empty();
			for (const auto c : digits) {
				const auto lower = Lower(c);
				const auto digit = IsDigit(c)
					? std::uint32_t(c - '0')
					: (hex && lower >= 'a' && lower <= 'f')
					? std::uint32_t(lower - 'a' + 10)
					: std::uint32_t(0xFF);
				if (digit == 0xFF || code > 0x10FFFF) {
					valid = false;
					break;
				}
				code = code * (hex ? 16 : 10) + digit;
			}
			if (!valid
				|| code == 0
				|| code > 0x10FFFF
				|| (code >= 0xD800 && code <= 0xDFFF)) {
				code = 0;
			}
		}
		if (!code) {
			result += value[i++];
			continue;
		}
		AppendUtf8(result, code);
		i = semicolon + 1;
	}
	return result;
}

struct Attribute {
	std::string name; // lower-case
	std::string value; // as written, references not decoded
};

// Reads the attributes of a tag whose name ends right before `from`, up to
// and including its closing '>'. Returns false when the input ends first -
// a body cut at the 64 KB limit in the middle of a tag - so a half-read tag
// is never acted on.
[[nodiscard]] inline bool ReadAttributes(
		std::string_view html,
		std::size_t from,
		std::vector<Attribute> &attributes,
		std::size_t &end) {
	auto i = from;
	const auto size = html.size();
	while (true) {
		while (i < size && (IsSpace(html[i]) || html[i] == '/')) {
			++i;
		}
		if (i >= size) {
			return false;
		} else if (html[i] == '>') {
			end = i + 1;
			return true;
		}
		auto name = std::string();
		while (i < size
			&& !IsSpace(html[i])
			&& html[i] != '='
			&& html[i] != '>'
			&& html[i] != '/') {
			name += Lower(html[i++]);
		}
		while (i < size && IsSpace(html[i])) {
			++i;
		}
		auto value = std::string();
		if (i < size && html[i] == '=') {
			++i;
			while (i < size && IsSpace(html[i])) {
				++i;
			}
			if (i >= size) {
				return false;
			}
			if (html[i] == '"' || html[i] == '\'') {
				const auto quote = html[i++];
				const auto close = html.find(quote, i);
				if (close == std::string_view::npos) {
					return false;
				}
				value = std::string(html.substr(i, close - i));
				i = close + 1;
			} else {
				while (i < size && !IsSpace(html[i]) && html[i] != '>') {
					value += html[i++];
				}
			}
		}
		if (!name.empty()) {
			attributes.push_back({ std::move(name), std::move(value) });
		}
	}
}

[[nodiscard]] inline const std::string *FindAttribute(
		const std::vector<Attribute> &attributes,
		std::string_view name) {
	for (const auto &attribute : attributes) {
		if (attribute.name == name) {
			return &attribute.value; // The first one wins, as in HTML.
		}
	}
	return nullptr;
}

// The URL in the content of a refresh declaration - "0;URL=https://..." -
// following the "shared declarative refresh steps" of the HTML standard.
// Empty when there is none: a bare delay only reloads the page itself.
[[nodiscard]] inline std::string RefreshContentUrl(std::string_view input) {
	const auto size = input.size();
	auto position = std::size_t(0);
	const auto skipSpaces = [&] {
		while (position < size && IsSpace(input[position])) {
			++position;
		}
	};
	skipSpaces();
	const auto timeStart = position;
	while (position < size && IsDigit(input[position])) {
		++position;
	}
	if (position == timeStart
		&& (position >= size || input[position] != '.')) {
		return std::string();
	}
	while (position < size
		&& (IsDigit(input[position]) || input[position] == '.')) {
		++position;
	}
	if (position < size) {
		const auto c = input[position];
		if (c != ';' && c != ',' && !IsSpace(c)) {
			return std::string();
		}
	}
	skipSpaces();
	if (position < size
		&& (input[position] == ';' || input[position] == ',')) {
		++position;
	}
	skipSpaces();
	if (position >= size) {
		return std::string();
	}
	auto urlString = input.substr(position);
	auto skipQuotes = false;
	if (Lower(input[position]) != 'u') {
		skipQuotes = true;
	} else if (position + 1 < size && Lower(input[position + 1]) == 'r'
		&& position + 2 < size && Lower(input[position + 2]) == 'l') {
		position += 3;
		skipSpaces();
		if (position < size && input[position] == '=') {
			++position;
			skipSpaces();
			skipQuotes = true;
		}
	}
	if (skipQuotes) {
		auto quote = char(0);
		if (position < size
			&& (input[position] == '"' || input[position] == '\'')) {
			quote = input[position++];
		}
		urlString = input.substr(position);
		if (quote) {
			const auto close = urlString.find(quote);
			if (close != std::string_view::npos) {
				urlString = urlString.substr(0, close);
			}
		}
	}
	return std::string(Trimmed(urlString));
}

[[nodiscard]] inline bool IsTagNameEnd(std::string_view html, std::size_t at) {
	return (at >= html.size())
		|| IsSpace(html[at])
		|| (html[at] == '/')
		|| (html[at] == '>');
}

} // namespace details

// The target of the first <meta http-equiv="refresh"> in `html`, exactly as
// written (character references decoded), or empty when the page has none,
// when the first one names no address, or when the tag is cut off.
//
// Comments are skipped, and so are <script> and <style> bodies, where a
// "<meta" would only be text. A <meta> inside <noscript> counts: that is
// where t.co puts it, and it is what a client without scripts follows.
//
// The result is still relative to the page and still unchecked - the caller
// resolves it against the page address and decides whether it may be used.
[[nodiscard]] inline std::string MetaRefreshUrl(std::string_view html) {
	using namespace details;
	auto i = std::size_t(0);
	while (i < html.size()) {
		const auto open = html.find('<', i);
		if (open == std::string_view::npos) {
			break;
		}
		if (StartsWithNoCase(html, open, "<!--")) {
			const auto close = html.find("-->", open + 4);
			if (close == std::string_view::npos) {
				break;
			}
			i = close + 3;
			continue;
		}
		const auto rawText = StartsWithNoCase(html, open, "<script")
			? std::string_view("</script")
			: StartsWithNoCase(html, open, "<style")
			? std::string_view("</style")
			: std::string_view();
		if (!rawText.empty()
			&& IsTagNameEnd(html, open + rawText.size() - 1)) {
			const auto close = FindNoCase(html, rawText, open + 1);
			if (close == std::string_view::npos) {
				break;
			}
			i = close + rawText.size();
			continue;
		}
		if (StartsWithNoCase(html, open, "<meta")
			&& IsTagNameEnd(html, open + 5)) {
			auto attributes = std::vector<Attribute>();
			auto end = std::size_t(0);
			if (!ReadAttributes(html, open + 5, attributes, end)) {
				break;
			}
			const auto equiv = FindAttribute(attributes, "http-equiv");
			const auto content = FindAttribute(attributes, "content");
			if (equiv && content) {
				auto lowered = std::string(Trimmed(*equiv));
				for (auto &c : lowered) {
					c = Lower(c);
				}
				if (lowered == "refresh") {
					return RefreshContentUrl(DecodeEntities(*content));
				}
			}
			i = end;
			continue;
		}
		i = open + 1;
	}
	return std::string();
}

// Where the registrable domain - "example.co.uk" in "www.example.co.uk" -
// starts inside `host`, which must be the ASCII (punycode) form, lower-case,
// without a trailing dot. 0 for an IP literal and for a host of two labels
// or fewer.
//
// An approximation of the Public Suffix List, which this build does not
// carry: the last two labels, or the last three under a two-letter country
// code whose second level is a generic one ("co.uk", "com.cn"), or under one
// of a few hosting suffixes on which every subdomain is somebody else's site
// ("github.io"). When it guesses wrong it can only make the emphasised part
// LONGER than the true registrable domain - never shorter - except for a
// hosting suffix missing from the list below; the full host is printed in
// full right next to it either way.
[[nodiscard]] inline std::size_t RegistrableDomainOffset(
		std::string_view host) {
	using namespace details;
	if (host.empty()
		|| host.front() == '['
		|| host.find(':') != std::string_view::npos) {
		return 0; // An IPv6 literal, with or without its brackets.
	}
	auto numeric = true;
	for (const auto c : host) {
		if (!IsDigit(c) && c != '.') {
			numeric = false;
			break;
		}
	}
	if (numeric) {
		return 0;
	}
	auto dots = std::vector<std::size_t>();
	for (std::size_t i = 0; i != host.size(); ++i) {
		if (host[i] == '.') {
			dots.push_back(i);
		}
	}
	const auto labels = dots.size() + 1;
	if (labels <= 2) {
		return 0;
	}
	const auto last = host.substr(dots[dots.size() - 1] + 1);
	const auto second = host.substr(
		dots[dots.size() - 2] + 1,
		dots[dots.size() - 1] - dots[dots.size() - 2] - 1);
	const auto lastTwo = host.substr(dots[dots.size() - 2] + 1);

	static constexpr std::string_view kHostingSuffixes[] = {
		"appspot.com",
		"azurewebsites.net",
		"blogspot.com",
		"cloudfront.net",
		"firebaseapp.com",
		"fly.dev",
		"github.io",
		"gitlab.io",
		"glitch.me",
		"herokuapp.com",
		"myshopify.com",
		"netlify.app",
		"ngrok-free.app",
		"ngrok.io",
		"notion.site",
		"onrender.com",
		"pages.dev",
		"repl.co",
		"replit.app",
		"surge.sh",
		"translate.goog",
		"tumblr.com",
		"vercel.app",
		"web.app",
		"webflow.io",
		"weebly.com",
		"wixsite.com",
		"wordpress.com",
		"workers.dev",
	};
	static constexpr std::string_view kGenericSecondLevel[] = {
		"ac",
		"co",
		"com",
		"edu",
		"gen",
		"go",
		"gob",
		"gov",
		"gv",
		"ltd",
		"mil",
		"ne",
		"net",
		"nic",
		"or",
		"org",
		"plc",
		"sch",
	};
	auto take = std::size_t(2);
	for (const auto suffix : kHostingSuffixes) {
		if (lastTwo == suffix) {
			take = 3;
			break;
		}
	}
	if (take == 2 && last.size() == 2) {
		for (const auto generic : kGenericSecondLevel) {
			if (second == generic) {
				take = 3;
				break;
			}
		}
	}
	if (labels <= take) {
		return 0;
	}
	return dots[dots.size() - take] + 1;
}

} // namespace Lumina::ShortLinkParse
