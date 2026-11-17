/*
This file is part of LuminaGram,
a fork of Telegram Desktop.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "lumina/lumina_ad_shield_account.h"

namespace Lumina::AdShield {

// The engine's FileSystem on Qt (QFile / QDir / QSaveFile). Paths are
// '/'-separated UTF-8. Used only from the AdShield queue: Qt's file classes
// are reentrant, and every instance here is created and destroyed on the
// calling thread.
//
// writeAtomic() goes through QSaveFile (temporary file + rename), which also
// refreshes the modification time EnforceTotalCap() orders by. On macOS the
// AdShield root directory is marked as excluded from Time Machine when it is
// created (makeDirectories()); the data is a local cache that is worthless
// anywhere else, and the per-install key it depends on is not in a backup
// either.
class QtFileSystem final : public FileSystem {
public:
	explicit QtFileSystem(std::string root);

	[[nodiscard]] bool isDirectory(const std::string &path) override;
	[[nodiscard]] bool isFile(const std::string &path) override;
	[[nodiscard]] std::vector<Entry> list(
		const std::string &directory) override;
	[[nodiscard]] bool read(
		const std::string &path,
		std::vector<unsigned char> &out) override;
	[[nodiscard]] bool readHead(
		const std::string &path,
		std::size_t size,
		std::vector<unsigned char> &out) override;
	[[nodiscard]] bool info(
		const std::string &path,
		std::int64_t &size,
		std::int64_t &modified) override;
	bool makeDirectories(const std::string &path) override;
	bool writeAtomic(
		const std::string &path,
		const std::vector<unsigned char> &data) override;
	bool removeFile(const std::string &path) override;
	bool removeTree(const std::string &path) override;
	bool removeEmptyDirectory(const std::string &path) override;

private:
	std::string _root;

};

} // namespace Lumina::AdShield
