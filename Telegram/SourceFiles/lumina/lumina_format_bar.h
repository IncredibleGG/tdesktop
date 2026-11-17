/*
This file is part of LuminaGram,
a fork of Telegram Desktop.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

namespace Ui {
class InputField;
class RpWidget;
} // namespace Ui

namespace Lumina {

// LuminaGram #23: the floating formatting bar.
//
// When the user selects text in a chat's message field, a small pill with
// nine buttons - bold, italic, underline, strikethrough, monospace, spoiler,
// quote | link, clear formatting - appears just above the compose stack,
// centred on the selection. Every button calls the stock public InputField
// markdown API (toggleCurrentMarkdownTag / editCurrentMarkdownLink /
// clearCurrentMarkdown). With a selection those run the very same
// toggleSelectionMarkdown / editMarkdownLink path as the right-click
// "Formatting" submenu and the Ctrl+B-style shortcuts, so the bar produces
// exactly the entities they do. lib_ui is not touched and no formatting logic
// of our own exists here.
//
// Off by default ("formattingBar" in Lumina::Settings, the key Android uses).
// While it is off nothing but the settings subscription exists: no widget, no
// event filter, no signal subscription on the field. Turning it on or off
// takes effect at once in every open composer.
//
// It never takes focus: the bar is an ordinary child widget of the host (not
// a top-level popup, which on Wayland cannot be placed and would steal the
// selection's focus), its buttons are NoFocus, and the field's events are only
// observed, never consumed. It stays hidden in screen reader mode, while the
// field's context menu, the emoji / sticker panel, an autocomplete list or an
// inline bot result list is open, and while a mouse button is held (it shows
// on release; a keyboard selection shows it after a short pause). It sits
// above the whole compose stack - reply / edit bar and the translate-before-
// send preview included - so it never covers the text being typed or the
// preview, and it changes nothing about what translate-before-send does. It
// is kept over the message list (FormatBarHost::area), never over subsection
// tabs or other bars beside or below it.
//
// Hosts: HistoryWidget::_field and the non-story ComposeControls::_field only.
// Deliberately NOT InitMessageFieldHandlers(), which the Instant View rich
// editor's own fields go through as well (it has its own toolbar).
//
// Upstream merge markers to grep for in the hosts: "LuminaGram #23",
// "Lumina::AttachFormatBar", "Lumina::MoveFormatBar", "Lumina::RaiseFormatBar",
// "Lumina::SetFormatBarArea" (ComposeControls only).

struct FormatBarHost {
	not_null<Ui::InputField*> field;

	// The bar's parent. Must be an ancestor of the field.
	not_null<Ui::RpWidget*> overlay;

	// The top of the whole compose stack (reply / edit bar and the translate
	// preview included) in overlay coordinates. The bar is kept above it.
	Fn<int()> composeTop;

	// True while a host panel that the bar must make way for is open.
	Fn<bool()> suppressed;

	// Optional. The part of the overlay the bar may cover - the message
	// list's viewport - in overlay coordinates. The pill is kept inside it,
	// so it never lands on what sits beside or below the list: a vertical
	// subsection tabs column, bottom tabs, a suggest-post bar. Without it the
	// last area given to SetFormatBarArea() is used, and before any was
	// given the whole overlay.
	Fn<QRect()> area;
};

// Once per field. Owns its state through the field's lifetime.
void AttachFormatBar(FormatBarHost host);

// Re-place the bar after the host moved things around. No-op when the field
// has no bar or the bar is hidden.
void MoveFormatBar(not_null<Ui::InputField*> field);

// The area (see FormatBarHost::area) for a host that only learns it from
// its owner: ComposeControls gets the message list's viewport through
// setAutocompleteBoundingRect(). No-op when the field has no bar.
void SetFormatBarArea(not_null<Ui::InputField*> field, QRect area);

// Keep the bar above its siblings after the host re-stacked its children.
// No-op when the field has no bar.
void RaiseFormatBar(not_null<Ui::InputField*> field);

[[nodiscard]] bool FormatBarEnabled();

} // namespace Lumina
