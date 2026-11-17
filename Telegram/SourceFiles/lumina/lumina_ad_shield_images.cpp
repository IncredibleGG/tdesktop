/*
This file is part of LuminaGram,
a fork of Telegram Desktop.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "lumina/lumina_ad_shield_images.h"

#include "ui/image/image_prepare.h"

#include <QtGui/QImage>

#include <algorithm>
#include <bitset>
#include <tuple>

namespace Lumina::AdShield {
namespace {

constexpr auto kHashWidth = 9;
constexpr auto kHashHeight = 8;

[[nodiscard]] std::int64_t LastSeen(
		std::int64_t date,
		std::int64_t seen) {
	return std::max(date, seen);
}

} // namespace

std::optional<std::uint64_t> InlineThumbnailHash(
		const QByteArray &inlineBytes) {
	if (inlineBytes.isEmpty()) {
		return std::nullopt;
	}
	const auto image = Images::FromInlineBytes(inlineBytes);
	if (image.isNull() || image.width() < 2 || image.height() < 1) {
		return std::nullopt;
	}
	const auto small = image.convertToFormat(
		QImage::Format_RGB32
	).scaled(
		kHashWidth,
		kHashHeight,
		Qt::IgnoreAspectRatio,
		Qt::SmoothTransformation);
	if (small.width() != kHashWidth || small.height() != kHashHeight) {
		return std::nullopt;
	}
	auto result = std::uint64_t(0);
	auto bit = 0;
	for (auto y = 0; y != kHashHeight; ++y) {
		for (auto x = 0; x + 1 != kHashWidth; ++x) {
			const auto left = qGray(small.pixel(x, y));
			const auto right = qGray(small.pixel(x + 1, y));
			if (left > right) {
				result |= (std::uint64_t(1) << bit);
			}
			++bit;
		}
	}
	return result;
}

int AspectGroup(int width, int height) {
	if (width <= 0 || height <= 0) {
		return -1;
	}
	// Compared as width * 100 against height * limit, so no floating point.
	const auto w = std::int64_t(width) * 100;
	const auto h = std::int64_t(height);
	if (w < h * 60) {
		return 0;
	} else if (w < h * 85) {
		return 1;
	} else if (w < h * 118) {
		return 2;
	} else if (w < h * 167) {
		return 3;
	}
	return 4;
}

int HammingDistance(std::uint64_t a, std::uint64_t b) {
	return int(std::bitset<64>(a ^ b).count());
}

std::string SimilarImages::canonical(
		std::int64_t msgId,
		std::uint64_t senderKey,
		std::uint64_t hash,
		int aspect,
		const std::string &photoId,
		std::int64_t date,
		std::int64_t now) {
	compact(now);

	// Seen before: keep the id it was observed with, unless the picture
	// itself changed (an edited photo), in which case it is matched afresh.
	const auto same = std::find_if(
		_entries.begin(),
		_entries.end(),
		[&](const Entry &entry) { return entry.msgId == msgId; });
	if (same != _entries.end()) {
		if (same->sender == senderKey
			&& same->hash == hash
			&& same->aspect == aspect) {
			same->seen = std::max(same->seen, now);
			return same->mediaId;
		}
		_entries.erase(same);
	}

	// The earliest similar photo of the same sender within the window wins,
	// so every later re-post converges on one id.
	const Entry *best = nullptr;
	for (const auto &entry : _entries) {
		if (entry.sender != senderKey
			|| entry.aspect != aspect
			|| aspect < 0
			|| HammingDistance(entry.hash, hash) > kSimilarImageMaxDistance) {
			continue;
		}
		const auto distance = (entry.date > date)
			? (entry.date - date)
			: (date - entry.date);
		if (distance > kSimilarImageWindow) {
			continue;
		}
		if (!best
			|| std::tie(entry.date, entry.msgId)
				< std::tie(best->date, best->msgId)) {
			best = &entry;
		}
	}
	auto result = best ? best->mediaId : photoId;
	_entries.push_back({
		.msgId = msgId,
		.sender = senderKey,
		.hash = hash,
		.aspect = aspect,
		.mediaId = result,
		.date = date,
		.seen = now,
	});
	enforceCap();
	return result;
}

void SimilarImages::compact(std::int64_t now) {
	_entries.erase(std::remove_if(
		_entries.begin(),
		_entries.end(),
		[&](const Entry &entry) {
			return (now - LastSeen(entry.date, entry.seen))
				> kSimilarImageWindow;
		}), _entries.end());
}

void SimilarImages::enforceCap() {
	if (_entries.size() <= kSimilarImageMaxEntries) {
		return;
	}
	std::sort(_entries.begin(), _entries.end(), [](
			const Entry &a,
			const Entry &b) {
		return std::make_tuple(LastSeen(a.date, a.seen), a.msgId)
			> std::make_tuple(LastSeen(b.date, b.seen), b.msgId);
	});
	_entries.resize(kSimilarImageMaxEntries);
}

void SimilarImages::clear() {
	_entries.clear();
}

} // namespace Lumina::AdShield
