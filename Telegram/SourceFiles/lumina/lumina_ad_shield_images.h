/*
This file is part of LuminaGram,
a fork of Telegram Desktop.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include <QtCore/QByteArray>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace Lumina::AdShield {

// The similar-image hook of SPEC 5.4: "the same sender re-posts a similar
// picture". The core only ever compares media ids, so before a photo is
// observed its id is replaced with the id of an earlier, similar photo of the
// same sender in the same group - after that the core's ordinary same-media
// rules count it.
//
// Similar = a 64-bit difference hash (dHash) of the message's inline
// thumbnail (the stripped JPEG every photo carries), Hamming distance <= 8,
// and the same aspect-ratio group. Same sender only; cross-sender matching is
// Phase 2.
//
// AdShield queue only.

inline constexpr auto kSimilarImageMaxDistance = 8;
inline constexpr auto kSimilarImageMaxEntries = std::size_t(500);
inline constexpr auto kSimilarImageWindow = std::int64_t(7 * 86400);

// dHash of a photo's inline thumbnail bytes (PhotoData::inlineThumbnailBytes,
// the "stripped" format): 9x8 grey, one bit per horizontal neighbour pair.
// Nothing when the bytes do not decode.
[[nodiscard]] std::optional<std::uint64_t> InlineThumbnailHash(
	const QByteArray &inlineBytes);

// 0..4, from portrait to wide; -1 for an unknown size.
[[nodiscard]] int AspectGroup(int width, int height);

[[nodiscard]] int HammingDistance(std::uint64_t a, std::uint64_t b);

// One group's table: (H("sender", id), dHash, aspect group, the media id the
// photo was observed with, message id, date). In memory only - after a
// restart it is rebuilt from the messages that get observed again - and
// dropped by forgetAll / "clear local data" / the panic wipe together with
// the rest of AdShield's data. No text, no plain sender id.
class SimilarImages final {
public:
	// The media id photo `msgId` is to be observed with: the id an earlier
	// similar photo of the same sender (within 7 days of `date`) was observed
	// with, or `photoId` itself. A message seen again keeps the id it got the
	// first time, as long as its picture did not change.
	[[nodiscard]] std::string canonical(
		std::int64_t msgId,
		std::uint64_t senderKey,
		std::uint64_t hash,
		int aspect,
		const std::string &photoId,
		std::int64_t date,
		std::int64_t now);

	void compact(std::int64_t now);
	void clear();

	[[nodiscard]] std::size_t size() const {
		return _entries.size();
	}

private:
	struct Entry {
		std::int64_t msgId = 0;
		std::uint64_t sender = 0;
		std::uint64_t hash = 0;
		int aspect = -1;
		std::string mediaId;
		std::int64_t date = 0;
		std::int64_t seen = 0;
	};

	void enforceCap();

	std::vector<Entry> _entries;

};

} // namespace Lumina::AdShield
