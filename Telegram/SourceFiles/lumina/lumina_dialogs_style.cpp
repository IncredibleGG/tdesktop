/*
This file is part of LuminaGram,
a fork of Telegram Desktop.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "lumina/lumina_dialogs_style.h"

#include "lumina/lumina_settings.h"

#include "ui/style/style_core.h"
#include "ui/style/style_core_scale.h"

#include "styles/style_dialogs.h"

#include <rpl/map.h>
#include <rpl/merge.h>
#include <rpl/producer.h>
#include <rpl/range.h>
#include <rpl/then.h>

#include <QtCore/QJsonValue>

#include <algorithm>
#include <array>

namespace Lumina {
namespace {

const auto kKeyCompactListRows = u"compactListRows"_q;
const auto kKeyPreviewLines = u"chatListPreviewLines"_q;

// Whether ANY metric preference is off the stock value, i.e. whether the
// patched styles differ from the codegen ones at all. When this is false
// every entry point short-circuits to its stock argument and the chat list
// is bit-identical to the stock one.
[[nodiscard]] bool Active() {
	return CompactListRows() || (PreviewLines() > kPreviewLinesMin);
}

enum Index : int {
	kDefaultRow,
	kTaggedRow,
	kForumRow,
	kTaggedForumRow,
	kForumTopicRow,
	kCommunityInfoRow,
	kCommunityInfoTaggedRow,
	kCommunityInfoForumRow,
	kCommunityInfoTaggedForumRow,
	kRowCount,
};

// One compact row family, in UNSCALED px exactly like a .style file - the
// values go through style::ConvertScale() at build time, so they follow the
// interface scale the same way the codegen originals do.
struct Spec {
	int height = 0;
	int paddingTop = 0;
	int paddingBottom = 0;
	int photoSize = 0;
	int nameTop = 0;
	int textTop = 0;
	int tagTop = 0; // 0 keeps the original, which is 0 for untagged families.
};

[[nodiscard]] auto Originals()
-> const std::array<const style::DialogRow*, kRowCount> & {
	static const auto result = std::array<const style::DialogRow*, kRowCount>{
		&st::defaultDialogRow,
		&st::taggedDialogRow,
		&st::forumDialogRow,
		&st::taggedForumDialogRow,
		&st::forumTopicRow,
		&st::communityInfoDialogRow,
		&st::communityInfoTaggedDialogRow,
		&st::communityInfoForumDialogRow,
		&st::communityInfoTaggedForumDialogRow,
	};
	return result;
}

// The communityInfo* families repeat the vertical numbers of the family they
// derive from, and that is load bearing rather than tidy: Dialogs::Row's height
// comes from the base family while Dialogs::CommunityChatsList paints the row
// with the matching communityInfo* one. dialogs.style keeps those two heights
// equal today, and compact has to keep them equal too, or every row in the
// community chats box would be painted taller than the slot it was given.
[[nodiscard]] const std::array<Spec, kRowCount> &Specs() {
	static const auto result = std::array<Spec, kRowCount>{ {
		// height  padT  padB  photo  nameTop  textTop  tagTop
		{     48,    5,    5,    38,       6,      25,       0 },
		{     64,    5,    5,    38,       6,      25,      45 },
		{     68,    5,    5,    38,       6,      25,       0 },
		{     86,    5,    5,    38,       6,      25,      67 },
		{     46,    4,    4,    20,       5,      25,       0 },
		{     48,    5,    5,    38,       6,      25,       0 },
		{     64,    5,    5,    38,       6,      25,      45 },
		{     68,    5,    5,    38,       6,      25,       0 },
		{     86,    5,    5,    38,       6,      25,      67 },
	} };
	return result;
}

struct Cache {
	std::array<style::DialogRow, kRowCount> rows;
	int paletteVersion = -1;
	int scale = 0;
	bool enabled = false;
	bool built = false;
	rpl::lifetime lifetime;
};

// The rows live in a fixed-size array that is only ever overwritten in place,
// never grown or reallocated: Dialogs::InnerWidget keeps a raw
// `const style::DialogRow *_st` into it across a rebuild.
[[nodiscard]] Cache &Data() {
	static auto result = Cache();
	[[maybe_unused]] static const auto init = [] {
		result.enabled = Active();
		Settings::Instance().changes(
		) | rpl::on_next([](const QString &key) {
			if (key == kKeyCompactListRows || key == kKeyPreviewLines) {
				result.enabled = Active();
				result.built = false;
			}
		}, result.lifetime);
		return true;
	}();
	return result;
}

void Rebuild(Cache &data) {
	const auto &originals = Originals();
	const auto &specs = Specs();
	const auto compact = CompactListRows();
	// Each preview line beyond the first adds exactly one line of the message
	// font to the row - the same height dialogs_layout.cpp gives each drawn
	// line - so the row and what it paints can never disagree. The metric is
	// already interface-scaled, so it is NOT run through ConvertScale() again.
	const auto extra = std::max(PreviewLines() - kPreviewLinesMin, 0)
		* st::dialogsTextFont->height;
	for (auto i = 0; i != int(kRowCount); ++i) {
		auto row = *originals[i];
		if (compact) {
			const auto &spec = specs[i];
			const auto top = style::ConvertScale(spec.paddingTop);
			const auto bottom = style::ConvertScale(spec.paddingBottom);
			// The avatar's shrink is rounded down to an even number of physical
			// pixels before it is split between the two sides, so that
			// padding.left() + photoSize + padding.left() stays EXACTLY the
			// stock narrow-column width at every interface scale. Taking half
			// of an odd shrink would lose that pixel, and the scales are 5%
			// apart, so at well over half of them ConvertScale(46) -
			// ConvertScale(38) is odd.
			const auto scaled = style::ConvertScale(spec.photoSize);
			const auto shrink = (row.photoSize - scaled) & ~1;
			const auto photo = row.photoSize - shrink;
			const auto shift = shrink / 2;
			row.padding = style::margins(
				row.padding.left() + shift,
				top,
				row.padding.right(),
				bottom);
			row.photoSize = photo;
			row.height = std::max(
				style::ConvertScale(spec.height),
				top + photo + bottom);
			row.nameTop = style::ConvertScale(spec.nameTop);
			row.textTop = style::ConvertScale(spec.textTop);
			if (spec.tagTop) {
				row.tagTop = style::ConvertScale(spec.tagTop);
			}
		}
		if (extra > 0) {
			// The extra lines sit below the existing text block; avatar, name
			// and the first text line keep their tops. A tagged family draws
			// its tag row immediately under what used to be a single text line,
			// so it moves down by the same amount - otherwise the added lines
			// would paint over the tags.
			row.height += extra;
			if (row.tagTop) {
				row.tagTop += extra;
			}
		}
		data.rows[i] = row;
	}
	data.paletteVersion = style::PaletteVersion();
	data.scale = style::Scale();
	data.built = true;
}

} // namespace

bool CompactListRows() {
	return Settings::Instance().getBool(kKeyCompactListRows, false);
}

void SetCompactListRows(bool value) {
	Settings::Instance().set(kKeyCompactListRows, value, Store::Prefs);
}

rpl::producer<bool> CompactListRowsValue() {
	// Data() registers the subscriber that invalidates the patched styles, and
	// it has to be registered before any consumer of this producer, because
	// both end up on Settings::changes() and an event_stream fires its
	// consumers in subscription order. A consumer that re-lays out a chat list
	// would otherwise run while DialogRowStyle() still answered with the
	// previous preference.
	[[maybe_unused]] const auto &data = Data();

	return rpl::single(
		rpl::empty
	) | rpl::then(
		Settings::Instance().changesFor(kKeyCompactListRows)
	) | rpl::map([] {
		return CompactListRows();
	});
}

int PreviewLines() {
	return std::clamp(
		Settings::Instance().getInt(kKeyPreviewLines, kPreviewLinesMin),
		kPreviewLinesMin,
		kPreviewLinesMax);
}

void SetPreviewLines(int value) {
	Settings::Instance().set(
		kKeyPreviewLines,
		std::clamp(value, kPreviewLinesMin, kPreviewLinesMax),
		Store::Prefs);
}

rpl::producer<int> PreviewLinesValue() {
	// Same ordering guarantee as CompactListRowsValue(): touch Data() first so
	// the style-invalidating subscriber is registered ahead of any consumer
	// that re-lays out a chat list off this producer.
	[[maybe_unused]] const auto &data = Data();

	return rpl::single(
		rpl::empty
	) | rpl::then(
		Settings::Instance().changesFor(kKeyPreviewLines)
	) | rpl::map([] {
		return PreviewLines();
	});
}

rpl::producer<> ChatListMetricsChanges() {
	// As above, register the invalidating subscriber before the consumer, and
	// merge the two metric preferences into one change stream so a consumer
	// re-lays out once per change from either.
	[[maybe_unused]] const auto &data = Data();

	return rpl::merge(
		Settings::Instance().changesFor(kKeyCompactListRows),
		Settings::Instance().changesFor(kKeyPreviewLines));
}

const style::DialogRow &DialogRowStyle(const style::DialogRow &original) {
	auto &data = Data();
	if (!data.enabled) {
		return original;
	}
	const auto &originals = Originals();
	for (auto i = 0; i != int(kRowCount); ++i) {
		if (originals[i] != &original) {
			continue;
		} else if (!data.built
			|| data.paletteVersion != style::PaletteVersion()
			|| data.scale != style::Scale()) {
			Rebuild(data);
		}
		return data.rows[i];
	}
	return original;
}

int DialogRowRightButtonTop(int margin, int buttonHeight) {
	if (!Data().enabled) {
		return margin;
	}
	const auto &row = DialogRowStyle(st::defaultDialogRow);
	return std::min(
		margin,
		row.height - buttonHeight - row.padding.bottom());
}

} // namespace Lumina
