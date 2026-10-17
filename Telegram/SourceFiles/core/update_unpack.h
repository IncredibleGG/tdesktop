/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include <QtCore/QByteArray>
#include <QtCore/QString>

#include <optional>

namespace Core::Updates {

struct VerifiedUpdate;

[[nodiscard]] std::optional<QByteArray> DecompressUpdatePayload(
	const VerifiedUpdate &verified,
	QString *error = nullptr);

// LuminaGram: the same parser for payload bytes that were authenticated some
// other way - our v1 package, whose bytes after its RSA signature and SHA1
// hash have exactly this layout (see UnpackUpdateV1() in update_checker.cpp).
// DecompressUpdatePayload() checks the v2 payload cap and then calls this.
[[nodiscard]] std::optional<QByteArray> DecompressPayloadBytes(
	const QByteArray &payload,
	QString *error = nullptr);

// Extraction requires a fresh caller-controlled directory.
[[nodiscard]] bool ExtractUpdateFiles(
	const QByteArray &uncompressed,
	quint64 expectedVersion,
	const QString &directory,
	QString *error = nullptr);

} // namespace Core::Updates
