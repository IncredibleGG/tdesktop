/*
This file is part of LuminaGram,
a fork of Telegram Desktop.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "base/basic_types.h"

#include <rpl/producer.h>

#include <array>

namespace Ui {
class VerticalLayout;
} // namespace Ui

namespace Lumina {

// Android's outgoing-photo quality: the JPEG quality LuminaGram encodes a sent
// photo with. It changes ONE parameter of Telegram's ordinary upload path -
// the quality passed to the JPEG writer in ComputePhotoJpegBytes()
// (storage/localimageloader.cpp) - and nothing else. It does not raise any
// limit, does not touch the MTProto chunking, and does not change the photo's
// resolution: the resolution is still governed by Telegram's own
// sendLargePhotos() / PhotoSideLimit() (2560 vs 1280), which is a native
// Telegram feature exposed in the attach menu and remembered by Core::Settings.
//
// WHAT IT AFFECTS, AND WHAT IT DELIBERATELY DOES NOT. ComputePhotoJpegBytes()
// re-encodes a photo only when it had to be downscaled, or when its bytes are
// not already a progressive JPEG small enough to pass through. A photo Telegram
// would pass through untouched (an already-progressive JPEG under the size
// limit) is returned verbatim and this quality never enters into it - exactly
// as before. So the control changes the quality of the RE-ENCODE, which is the
// only place a quality number has ever meant anything on this path.
//
// DEFAULT IS BYTE-IDENTICAL. kOutgoingPhotoQualityDefault is 87, the constant
// the writer used before this existed, so at the default the encoder is handed
// the same number it always was and produces the same bytes.
//
// THREADING. FileLoadTask::process() runs on the TaskQueue worker thread and
// Lumina::Settings is main-thread only, so the worker reads a mirror
// (OutgoingPhotoQualityCached()) rather than the store. The file loader
// refreshes that mirror in the FileLoadTask constructor, on the main thread,
// before the task is queued - the same shape the location-strip feature uses.

// The values the settings page offers, matching Android's radio list, with 87
// - "the quality the app has always encoded with" - the default.
inline constexpr auto kOutgoingPhotoQualityChoices = std::array{
	75,
	87,
	92,
	100,
};

inline constexpr auto kOutgoingPhotoQualityDefault = 87;

// Defensive bounds for a value the settings page cannot produce (a restored
// backup or a hand-edited luminagram.json). Qt's JPEG writer treats quality as
// 0..100; below ~30 a photo turns to mush, and this feature is not a way to
// destroy an upload.
inline constexpr auto kOutgoingPhotoQualityMin = 30;
inline constexpr auto kOutgoingPhotoQualityMax = 100;

// Stored preference `outgoingPhotoQuality`, Store::Prefs, default 87, clamped.
// Main thread only.
[[nodiscard]] int OutgoingPhotoQuality();
void SetOutgoingPhotoQuality(int value);
[[nodiscard]] rpl::producer<int> OutgoingPhotoQualityValue();

// The worker-thread mirror. RefreshOutgoingPhotoQualityCache() must be called
// from the main thread; the file loader calls it in the FileLoadTask
// constructor. The cache starts at the default, so even a path that never
// refreshed it encodes at the stock quality.
void RefreshOutgoingPhotoQualityCache();
[[nodiscard]] int OutgoingPhotoQualityCached();

// The outgoing-photo-quality rows on the LuminaGram Chats sub-page.
void AddPhotoQualityRows(not_null<Ui::VerticalLayout*> container);

} // namespace Lumina
