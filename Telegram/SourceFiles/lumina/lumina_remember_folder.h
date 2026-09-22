/*
This file is part of LuminaGram,
a fork of Telegram Desktop.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include <rpl/producer.h>

namespace Lumina {

// Android's DialogsActivity "remember last folder": reopen the chat list on
// the folder that was selected last, instead of always starting on the
// account default (which for a Premium account whose first tab is a folder is
// not even "All chats").
//
// Two preferences, both Store::Prefs, main thread only:
//
//   `rememberLastFolder` (bool, default false) - the opt-in. While it is off
//   nothing reads or writes `lastFolderId`, and the startup filter is picked
//   exactly as before (Data::ChatFilters::defaultId()).
//
//   `lastFolderId` (int, default 0) - the id of the last active folder. It is
//   written at the one choke point every folder switch passes through
//   (SessionController::setActiveChatsFilter) and read only when restoring the
//   initial filter, where it is validated against the loaded filter list and
//   falls back to the default if it no longer exists. 0 means "All chats".
//
// Interaction with HideChatFolders: while the folder bars are hidden the
// active filter is clamped to 0 everywhere, so a stored non-zero id is never
// restored (the restore points check HideChatFolders first) and 0 is what gets
// stored - a neutral no-op.

[[nodiscard]] bool RememberLastFolder();
void SetRememberLastFolder(bool value);
[[nodiscard]] rpl::producer<bool> RememberLastFolderValue();
[[nodiscard]] rpl::producer<> RememberLastFolderChanges();

[[nodiscard]] int LastFolderId();
void SetLastFolderId(int id);

} // namespace Lumina
