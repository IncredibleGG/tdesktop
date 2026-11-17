/*
This file is part of LuminaGram,
a fork of Telegram Desktop.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "lumina/lumina_ad_shield_store.h"

namespace Lumina::AdShield {

// File access used by the account layer, so the engine stays free of Qt and
// tests run against an in-memory implementation. Paths are '/'-separated;
// names created by AdShield are lowercase hex plus ".bin". writeAtomic must
// write a temporary file next to the target and rename it over the target
// (QSaveFile), which also refreshes the modification time that the total
// size cap uses for its LRU order.
class FileSystem {
public:
	struct Entry {
		std::string name;
		bool directory = false;
	};

	virtual ~FileSystem() = default;

	[[nodiscard]] virtual bool isDirectory(const std::string &path) = 0;
	[[nodiscard]] virtual bool isFile(const std::string &path) = 0;
	[[nodiscard]] virtual std::vector<Entry> list(
		const std::string &directory) = 0;
	[[nodiscard]] virtual bool read(
		const std::string &path,
		std::vector<unsigned char> &out) = 0;
	[[nodiscard]] virtual bool readHead(
		const std::string &path,
		std::size_t size,
		std::vector<unsigned char> &out) = 0;
	[[nodiscard]] virtual bool info(
		const std::string &path,
		std::int64_t &size,
		std::int64_t &modified) = 0;
	virtual bool makeDirectories(const std::string &path) = 0;
	virtual bool writeAtomic(
		const std::string &path,
		const std::vector<unsigned char> &data) = 0;
	virtual bool removeFile(const std::string &path) = 0;
	virtual bool removeTree(const std::string &path) = 0;
	virtual bool removeEmptyDirectory(const std::string &path) = 0;
};

[[nodiscard]] std::string AccountDirectoryName(
	const Key &key,
	std::int64_t userId);
[[nodiscard]] std::string ChatFileName(const Key &key, std::int64_t chatId);

// Removes chat files whose key check does not match the current key (or
// that have no readable header), stray files and empty account directories.
// Call on start-up and from forgetAll(). Returns the number of removals.
int PurgeOrphans(FileSystem &fs, const std::string &root, const Key &key);

// Panic wipe: removes the whole AdShield root (all accounts, orphans, temps).
void WipeRoot(FileSystem &fs, const std::string &root);

// Deletes whole chat files, least recently written first, until the total
// size of all accounts is at most cap. Returns the number of removed files.
int EnforceTotalCap(
	FileSystem &fs,
	const std::string &root,
	std::int64_t cap = kTotalCapBytes);

// One per Telegram account, keyed by user id (not by slot index). Without a
// file system everything stays in memory (tests, dry runs).
class Account {
public:
	Account(std::int64_t userId, const Key &key);
	Account(
		std::int64_t userId,
		const Key &key,
		FileSystem *fs,
		std::string root);

	// nullptr after forgetAll().
	[[nodiscard]] std::shared_ptr<Store> chat(std::int64_t chatId);
	[[nodiscard]] std::shared_ptr<Store> loaded(std::int64_t chatId) const;
	[[nodiscard]] std::vector<std::int64_t> loadedChats() const;
	void unload(std::int64_t chatId);

	// false after forgetAll(), without a file system, or on a write error.
	bool flush();
	bool flush(std::int64_t chatId);

	// Logout / account removal: forget everything in memory, delete this
	// account's directory and orphans, refuse every later write. panic also
	// wipes the whole root (all accounts).
	void forgetAll(bool panic = false);

	[[nodiscard]] bool forgotten() const {
		return *_forgotten;
	}
	[[nodiscard]] std::int64_t userId() const {
		return _userId;
	}
	[[nodiscard]] std::string directoryName() const;
	[[nodiscard]] std::string directoryPath() const;
	[[nodiscard]] std::string chatFileName(std::int64_t chatId) const;

private:
	[[nodiscard]] bool write(std::int64_t chatId, const Store &store);

	std::int64_t _userId = 0;
	Key _key = {};
	FileSystem *_fs = nullptr;
	std::string _root;
	std::shared_ptr<bool> _forgotten;
	std::map<std::int64_t, std::shared_ptr<Store>> _chats;

};

} // namespace Lumina::AdShield
