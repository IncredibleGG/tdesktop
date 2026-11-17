/*
This file is part of LuminaGram,
a fork of Telegram Desktop.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "lumina/lumina_ad_shield_account.h"

#include <algorithm>
#include <tuple>
#include <utility>

namespace Lumina::AdShield {
namespace {

constexpr auto kBinSuffix = std::string_view(".bin");

[[nodiscard]] std::string JoinPath(
		const std::string &directory,
		const std::string &name) {
	if (directory.empty()) {
		return name;
	}
	return (directory.back() == '/')
		? (directory + name)
		: (directory + '/' + name);
}

[[nodiscard]] bool HasBinSuffix(const std::string &name) {
	return (name.size() >= kBinSuffix.size())
		&& (std::string_view(name).substr(name.size() - kBinSuffix.size())
			== kBinSuffix);
}

[[nodiscard]] std::uint64_t CurrentKeyCheck(const Key &key) {
	return Hash(key, "check", std::string_view("LGAS"));
}

[[nodiscard]] bool FileKeyCheck(
		FileSystem &fs,
		const std::string &path,
		std::uint64_t &check) {
	auto head = std::vector<unsigned char>();
	if (!fs.readHead(path, 24, head) || head.size() < 24) {
		return false;
	}
	if (head[0] != 'L' || head[1] != 'G' || head[2] != 'A' || head[3] != 'S') {
		return false;
	}
	check = 0;
	for (auto i = 0; i != 8; ++i) {
		check |= std::uint64_t(head[16 + i]) << (8 * i);
	}
	return true;
}

} // namespace

std::string AccountDirectoryName(const Key &key, std::int64_t userId) {
	return Hex64(Hash(key, "acct", std::string_view(std::to_string(userId))));
}

std::string ChatFileName(const Key &key, std::int64_t chatId) {
	return Hex64(Hash(key, "chat", std::string_view(std::to_string(chatId))))
		+ std::string(kBinSuffix);
}

int PurgeOrphans(FileSystem &fs, const std::string &root, const Key &key) {
	if (root.empty() || !fs.isDirectory(root)) {
		return 0;
	}
	const auto want = CurrentKeyCheck(key);
	auto removed = 0;
	for (const auto &entry : fs.list(root)) {
		const auto path = JoinPath(root, entry.name);
		if (!entry.directory) {
			if (fs.removeFile(path)) {
				++removed;
			}
			continue;
		}
		for (const auto &file : fs.list(path)) {
			const auto filePath = JoinPath(path, file.name);
			auto check = std::uint64_t();
			const auto keep = HasBinSuffix(file.name)
				&& !file.directory
				&& FileKeyCheck(fs, filePath, check)
				&& (check == want);
			if (keep) {
				continue;
			}
			const auto done = file.directory
				? fs.removeTree(filePath)
				: fs.removeFile(filePath);
			if (done) {
				++removed;
			}
		}
		if (fs.list(path).empty()) {
			fs.removeEmptyDirectory(path);
		}
	}
	return removed;
}

void WipeRoot(FileSystem &fs, const std::string &root) {
	if (!root.empty() && fs.isDirectory(root)) {
		fs.removeTree(root);
	}
}

int EnforceTotalCap(
		FileSystem &fs,
		const std::string &root,
		std::int64_t cap) {
	if (root.empty() || !fs.isDirectory(root)) {
		return 0;
	}
	struct File {
		std::int64_t modified = 0;
		std::string path;
		std::int64_t size = 0;
	};
	auto files = std::vector<File>();
	auto total = std::int64_t(0);
	for (const auto &entry : fs.list(root)) {
		if (!entry.directory) {
			continue;
		}
		const auto directory = JoinPath(root, entry.name);
		for (const auto &file : fs.list(directory)) {
			if (file.directory || !HasBinSuffix(file.name)) {
				continue;
			}
			auto item = File();
			item.path = JoinPath(directory, file.name);
			if (!fs.info(item.path, item.size, item.modified)) {
				continue;
			}
			total += item.size;
			files.push_back(std::move(item));
		}
	}
	std::sort(files.begin(), files.end(), [](const File &a, const File &b) {
		return std::tie(a.modified, a.path) < std::tie(b.modified, b.path);
	});
	auto removed = 0;
	for (const auto &file : files) {
		if (total <= cap) {
			break;
		}
		if (fs.removeFile(file.path)) {
			total -= file.size;
			++removed;
		}
	}
	return removed;
}

Account::Account(std::int64_t userId, const Key &key)
: Account(userId, key, nullptr, std::string()) {
}

Account::Account(
	std::int64_t userId,
	const Key &key,
	FileSystem *fs,
	std::string root)
: _userId(userId)
, _key(key)
, _fs(fs)
, _root(std::move(root))
, _forgotten(std::make_shared<bool>(false)) {
}

std::string Account::directoryName() const {
	return AccountDirectoryName(_key, _userId);
}

std::string Account::directoryPath() const {
	return JoinPath(_root, directoryName());
}

std::string Account::chatFileName(std::int64_t chatId) const {
	return ChatFileName(_key, chatId);
}

std::shared_ptr<Store> Account::chat(std::int64_t chatId) {
	if (*_forgotten) {
		return nullptr;
	}
	const auto i = _chats.find(chatId);
	if (i != _chats.end()) {
		return i->second;
	}
	auto result = std::shared_ptr<Store>();
	if (_fs && !_root.empty()) {
		const auto path = JoinPath(directoryPath(), chatFileName(chatId));
		auto bytes = std::vector<unsigned char>();
		if (_fs->isFile(path) && _fs->read(path, bytes)) {
			result = Store::Deserialize(
				bytes.data(),
				bytes.size(),
				chatId,
				_key,
				_forgotten);
		}
	}
	if (!result) {
		result = std::make_shared<Store>(chatId, _key, _forgotten);
	}
	_chats.emplace(chatId, result);
	return result;
}

std::shared_ptr<Store> Account::loaded(std::int64_t chatId) const {
	const auto i = _chats.find(chatId);
	return (i != _chats.end()) ? i->second : nullptr;
}

std::vector<std::int64_t> Account::loadedChats() const {
	auto result = std::vector<std::int64_t>();
	result.reserve(_chats.size());
	for (const auto &[chatId, store] : _chats) {
		result.push_back(chatId);
	}
	return result;
}

void Account::unload(std::int64_t chatId) {
	_chats.erase(chatId);
}

bool Account::write(std::int64_t chatId, const Store &store) {
	const auto directory = directoryPath();
	if (!_fs->isDirectory(directory) && !_fs->makeDirectories(directory)) {
		return false;
	}
	return _fs->writeAtomic(
		JoinPath(directory, chatFileName(chatId)),
		store.serialize());
}

bool Account::flush() {
	if (*_forgotten || !_fs || _root.empty()) {
		return false;
	}
	auto result = true;
	for (const auto &[chatId, store] : _chats) {
		if (!write(chatId, *store)) {
			result = false;
		}
	}
	return result;
}

bool Account::flush(std::int64_t chatId) {
	if (*_forgotten || !_fs || _root.empty()) {
		return false;
	}
	const auto i = _chats.find(chatId);
	return (i != _chats.end()) && write(chatId, *i->second);
}

void Account::forgetAll(bool panic) {
	*_forgotten = true;
	_chats.clear();
	if (!_fs || _root.empty()) {
		return;
	}
	if (panic) {
		WipeRoot(*_fs, _root);
		return;
	}
	const auto directory = directoryPath();
	if (_fs->isDirectory(directory)) {
		_fs->removeTree(directory);
	}
	PurgeOrphans(*_fs, _root, _key);
}

} // namespace Lumina::AdShield
