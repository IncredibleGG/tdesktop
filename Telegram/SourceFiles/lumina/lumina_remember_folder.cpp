/*
This file is part of LuminaGram,
a fork of Telegram Desktop.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "lumina/lumina_remember_folder.h"

#include "lumina/lumina_settings.h"

#include <QtCore/QJsonValue>

namespace Lumina {
namespace {

const auto kKeyRememberLastFolder = u"rememberLastFolder"_q;
const auto kKeyLastFolderId = u"lastFolderId"_q;

} // namespace

bool RememberLastFolder() {
	return Settings::Instance().getBool(kKeyRememberLastFolder, false);
}

void SetRememberLastFolder(bool value) {
	Settings::Instance().set(kKeyRememberLastFolder, value, Store::Prefs);
}

rpl::producer<bool> RememberLastFolderValue() {
	return rpl::single(
		rpl::empty
	) | rpl::then(
		RememberLastFolderChanges()
	) | rpl::map([] {
		return RememberLastFolder();
	});
}

rpl::producer<> RememberLastFolderChanges() {
	return Settings::Instance().changesFor(kKeyRememberLastFolder);
}

int LastFolderId() {
	return Settings::Instance().getInt(kKeyLastFolderId, 0);
}

void SetLastFolderId(int id) {
	Settings::Instance().set(kKeyLastFolderId, id, Store::Prefs);
}

} // namespace Lumina
