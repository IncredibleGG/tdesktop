/*
This file is part of LuminaGram,
a fork of Telegram Desktop.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "lumina/lumina_ad_shield_fs.h"

#include <QtCore/QDateTime>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QSaveFile>

#ifdef Q_OS_MAC
#include <sys/xattr.h>
#endif // Q_OS_MAC

namespace Lumina::AdShield {
namespace {

// A chat file at every SPEC 8.2 cap is about 2 MiB. Anything far larger is
// not ours to parse.
constexpr auto kMaxReadSize = qint64(8 * 1024 * 1024);

[[nodiscard]] QString Q(const std::string &path) {
	return QString::fromStdString(path);
}

void ExcludeFromBackup(const QString &path) {
#ifdef Q_OS_MAC
	// What CSBackupSetItemExcluded(url, true, false) writes; Time Machine
	// skips an item that carries it. Best effort: a failure changes nothing
	// about the data itself.
	static constexpr char kName[]
		= "com.apple.metadata:com_apple_backup_excludeItem";
	static constexpr char kValue[] = "com.apple.backupd";
	const auto native = QFile::encodeName(path);
	setxattr(
		native.constData(),
		kName,
		kValue,
		sizeof(kValue) - 1,
		0,
		0);
#else // Q_OS_MAC
	Q_UNUSED(path);
#endif // Q_OS_MAC
}

} // namespace

QtFileSystem::QtFileSystem(std::string root)
: _root(std::move(root)) {
}

bool QtFileSystem::isDirectory(const std::string &path) {
	return QFileInfo(Q(path)).isDir();
}

bool QtFileSystem::isFile(const std::string &path) {
	return QFileInfo(Q(path)).isFile();
}

std::vector<FileSystem::Entry> QtFileSystem::list(
		const std::string &directory) {
	auto result = std::vector<Entry>();
	const auto entries = QDir(Q(directory)).entryInfoList(
		QDir::AllEntries
			| QDir::NoDotAndDotDot
			| QDir::Hidden
			| QDir::System,
		QDir::Name);
	result.reserve(entries.size());
	for (const auto &entry : entries) {
		result.push_back({
			.name = entry.fileName().toStdString(),
			.directory = entry.isDir() && !entry.isSymLink(),
		});
	}
	return result;
}

bool QtFileSystem::read(
		const std::string &path,
		std::vector<unsigned char> &out) {
	out.clear();
	auto file = QFile(Q(path));
	if (!file.open(QIODevice::ReadOnly)) {
		return false;
	} else if (file.size() > kMaxReadSize) {
		return false;
	}
	const auto bytes = file.readAll();
	out.assign(
		reinterpret_cast<const unsigned char*>(bytes.constData()),
		reinterpret_cast<const unsigned char*>(bytes.constData())
			+ bytes.size());
	return true;
}

bool QtFileSystem::readHead(
		const std::string &path,
		std::size_t size,
		std::vector<unsigned char> &out) {
	out.clear();
	auto file = QFile(Q(path));
	if (!file.open(QIODevice::ReadOnly)) {
		return false;
	}
	const auto bytes = file.read(qint64(size));
	out.assign(
		reinterpret_cast<const unsigned char*>(bytes.constData()),
		reinterpret_cast<const unsigned char*>(bytes.constData())
			+ bytes.size());
	return true;
}

bool QtFileSystem::info(
		const std::string &path,
		std::int64_t &size,
		std::int64_t &modified) {
	const auto info = QFileInfo(Q(path));
	if (!info.exists()) {
		return false;
	}
	size = std::int64_t(info.size());
	modified = std::int64_t(info.lastModified().toMSecsSinceEpoch());
	return true;
}

bool QtFileSystem::makeDirectories(const std::string &path) {
	const auto rootExisted = !_root.empty() && QDir(Q(_root)).exists();
	if (!QDir().mkpath(Q(path))) {
		return false;
	}
	if (!rootExisted && !_root.empty() && QDir(Q(_root)).exists()) {
		ExcludeFromBackup(Q(_root));
	}
	return true;
}

bool QtFileSystem::writeAtomic(
		const std::string &path,
		const std::vector<unsigned char> &data) {
	auto file = QSaveFile(Q(path));
	if (!file.open(QIODevice::WriteOnly)) {
		return false;
	}
	const auto size = qint64(data.size());
	const auto written = size
		? file.write(reinterpret_cast<const char*>(data.data()), size)
		: qint64(0);
	if (written != size) {
		// No commit(): the destructor discards the temporary file and the
		// previous version stays in place.
		return false;
	}
	return file.commit();
}

bool QtFileSystem::removeFile(const std::string &path) {
	return QFile::remove(Q(path));
}

bool QtFileSystem::removeTree(const std::string &path) {
	return QDir(Q(path)).removeRecursively();
}

bool QtFileSystem::removeEmptyDirectory(const std::string &path) {
	return QDir().rmdir(Q(path));
}

} // namespace Lumina::AdShield
