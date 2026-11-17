// In-memory FileSystem for the AdShield checker (dev tool, not app code).
#pragma once

#include "lumina/lumina_ad_shield_account.h"

#include <algorithm>
#include <map>
#include <string>
#include <vector>

namespace AdShieldCheck {

class MemoryFileSystem final : public Lumina::AdShield::FileSystem {
public:
	struct Node {
		bool directory = false;
		std::vector<unsigned char> data;
		std::int64_t modified = 0;
	};

	bool isDirectory(const std::string &path) override {
		const auto i = _nodes.find(path);
		return (i != _nodes.end()) && i->second.directory;
	}
	bool isFile(const std::string &path) override {
		const auto i = _nodes.find(path);
		return (i != _nodes.end()) && !i->second.directory;
	}
	std::vector<Entry> list(const std::string &directory) override {
		auto result = std::vector<Entry>();
		if (!isDirectory(directory)) {
			return result;
		}
		const auto prefix = directory + '/';
		for (auto i = _nodes.lower_bound(prefix); i != _nodes.end(); ++i) {
			if (i->first.compare(0, prefix.size(), prefix) != 0) {
				break;
			}
			const auto name = i->first.substr(prefix.size());
			if (name.find('/') == std::string::npos) {
				result.push_back({ name, i->second.directory });
			}
		}
		return result;
	}
	bool read(
			const std::string &path,
			std::vector<unsigned char> &out) override {
		if (!isFile(path)) {
			return false;
		}
		out = _nodes[path].data;
		return true;
	}
	bool readHead(
			const std::string &path,
			std::size_t size,
			std::vector<unsigned char> &out) override {
		if (!isFile(path)) {
			return false;
		}
		const auto &data = _nodes[path].data;
		out.assign(data.begin(), data.begin() + std::min(size, data.size()));
		return true;
	}
	bool info(
			const std::string &path,
			std::int64_t &size,
			std::int64_t &modified) override {
		if (!isFile(path)) {
			return false;
		}
		const auto &node = _nodes[path];
		size = std::int64_t(node.data.size());
		modified = node.modified;
		return true;
	}
	bool makeDirectories(const std::string &path) override {
		auto position = std::size_t(1);
		while (true) {
			const auto slash = path.find('/', position);
			const auto part = path.substr(0, slash);
			const auto i = _nodes.find(part);
			if (i == _nodes.end()) {
				_nodes[part].directory = true;
			} else if (!i->second.directory) {
				return false;
			}
			if (slash == std::string::npos) {
				return true;
			}
			position = slash + 1;
		}
	}
	bool writeAtomic(
			const std::string &path,
			const std::vector<unsigned char> &data) override {
		const auto slash = path.rfind('/');
		if (slash == std::string::npos || !isDirectory(path.substr(0, slash))) {
			return false;
		}
		if (isDirectory(path)) {
			return false;
		}
		++writes;
		auto &node = _nodes[path];
		node.directory = false;
		node.data = data;
		node.modified = ++_clock;
		return true;
	}
	bool removeFile(const std::string &path) override {
		if (!isFile(path)) {
			return false;
		}
		_nodes.erase(path);
		return true;
	}
	bool removeTree(const std::string &path) override {
		if (_nodes.find(path) == _nodes.end()) {
			return false;
		}
		const auto prefix = path + '/';
		for (auto i = _nodes.lower_bound(prefix); i != _nodes.end();) {
			if (i->first.compare(0, prefix.size(), prefix) != 0) {
				break;
			}
			i = _nodes.erase(i);
		}
		_nodes.erase(path);
		return true;
	}
	bool removeEmptyDirectory(const std::string &path) override {
		if (!isDirectory(path) || !list(path).empty()) {
			return false;
		}
		_nodes.erase(path);
		return true;
	}

	void putFile(const std::string &path, std::vector<unsigned char> data) {
		auto &node = _nodes[path];
		node.directory = false;
		node.data = std::move(data);
		node.modified = ++_clock;
	}
	void setModified(const std::string &path, std::int64_t modified) {
		_nodes[path].modified = modified;
	}
	[[nodiscard]] bool exists(const std::string &path) const {
		return _nodes.find(path) != _nodes.end();
	}
	[[nodiscard]] std::vector<std::string> names(const std::string &path) {
		auto result = std::vector<std::string>();
		for (const auto &entry : list(path)) {
			result.push_back(entry.name);
		}
		return result;
	}

	int writes = 0;

private:
	std::map<std::string, Node> _nodes;
	std::int64_t _clock = 1000;

};

} // namespace AdShieldCheck
