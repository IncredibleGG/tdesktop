/*
This file is part of LuminaGram,
a fork of Telegram Desktop.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include <rpl/producer.h>

namespace Lumina {

// Batch 4, #14. When on, double-clicking one of your OWN, still-editable
// messages opens it in the composer for editing instead of running the normal
// double-click quick action (reply / react). Off by default, in which case the
// two history widgets behave exactly as stock Telegram Desktop does.
//
// "Own and editable" is decided by HistoryItem::allowsEdit(), the same test the
// context-menu "Edit" action uses, so this can never open the editor on a
// message the server would refuse to let you edit.
//
// Key `doubleTapEdit`, Store::Prefs, default false.
[[nodiscard]] bool DoubleTapEditEnabled();
[[nodiscard]] rpl::producer<bool> DoubleTapEditEnabledValue();
void SetDoubleTapEditEnabled(bool value);

} // namespace Lumina
