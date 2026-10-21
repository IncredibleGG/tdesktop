/*
This file is part of LuminaGram,
a fork of Telegram Desktop.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "lumina/lumina_whisper_model.h"

#include "base/timer.h"
#include "lumina/lumina_locale.h"
#include "settings.h" // cWorkingDir()

#include <rpl/event_stream.h>

#include <algorithm>
#include <memory>

#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QCryptographicHash>
#include <QtCore/QPointer>
#include <QtNetwork/QNetworkAccessManager>
#include <QtNetwork/QNetworkReply>
#include <QtNetwork/QNetworkRequest>

namespace Lumina {
namespace {

using Failure = WhisperModelState::Failure;
using Stage = WhisperModelState::Stage;

// The exact model, hosted on the project's own R2 bucket. The size is the
// readiness gate (a truncated download is not `kModelSize` bytes), and the
// sha256 is verified once on finish so a corrupted or substituted file is
// deleted rather than loaded into whisper.cpp.
constexpr auto kModelSize = qint64(59707625);
const auto kModelUrl = QString::fromLatin1(
	"https://pub-d6a54d2e5f5947e2b0b23fb8e27ce0a5.r2.dev"
	"/models/ggml-base-q5_1.bin");
const auto kModelSha256 = QByteArray(
	"422f1ae452ade6f30a004d7e5c6a43195e4433bc370bf23fac9cc591f01a8898");

// How long the download may go without receiving a single byte - connecting
// included - before it is abandoned as timed out. A slow line that keeps
// trickling in is never cut off; one that has stopped is, instead of sitting
// on the same percentage until the user gives up on it.
constexpr auto kStallTimeout = crl::time(30000);

[[nodiscard]] QString ModelsDir() {
	return cWorkingDir() + u"tdata/luminagram-models"_q;
}

// A single main-thread owner of the download: the QNetworkAccessManager, the
// running reply, the open .tmp file, the running hash and the state the UI
// reads. Everything here touches the network reply's signals, which fire on
// the main thread, so no locking is needed.
class Manager final {
public:
	[[nodiscard]] static Manager &Instance() {
		static Manager instance;
		return instance;
	}

	[[nodiscard]] WhisperModelState state() const {
		return _state;
	}
	[[nodiscard]] rpl::producer<> changes() const {
		return _changes.events();
	}

	void download();
	void cancel();
	void remove();

private:
	void setState(WhisperModelState state);
	void fail(Failure failure);
	void cleanupReply();

	QNetworkAccessManager _network;
	QPointer<QNetworkReply> _reply;
	std::unique_ptr<QFile> _tmp;
	QCryptographicHash _hash{ QCryptographicHash::Sha256 };
	base::Timer _stall;
	WhisperModelState _state;
	rpl::event_stream<> _changes;

};

void Manager::setState(WhisperModelState state) {
	_state = state;
	_changes.fire({});
}

void Manager::cleanupReply() {
	_stall.cancel();
	if (const auto reply = _reply.data()) {
		_reply = nullptr;
		// Disconnected first, so nothing it still emits reaches the handlers
		// below; deleted later rather than now, because this may run from
		// inside one of that reply's own signals. Deleting a reply that is
		// still running aborts it.
		reply->disconnect();
		reply->deleteLater();
	}
	if (_tmp) {
		_tmp->close();
		_tmp = nullptr;
	}
}

void Manager::fail(Failure failure) {
	const auto tmpPath = WhisperModelPath() + u".tmp"_q;
	cleanupReply();
	QFile::remove(tmpPath);
	setState({ .stage = Stage::Absent, .failure = failure });
}

void Manager::download() {
	if (_reply) {
		return; // Already in flight.
	} else if (WhisperModelReady()) {
		setState({ .stage = Stage::Ready });
		return;
	}
	QDir().mkpath(ModelsDir());

	const auto tmpPath = WhisperModelPath() + u".tmp"_q;
	QFile::remove(tmpPath);
	_tmp = std::make_unique<QFile>(tmpPath);
	if (!_tmp->open(QIODevice::WriteOnly | QIODevice::Truncate)) {
		fail(Failure::Storage);
		return;
	}
	_hash.reset();

	setState({ .stage = Stage::Downloading, .progress = 0 });

	auto request = QNetworkRequest(QUrl(kModelUrl));
	request.setAttribute(
		QNetworkRequest::RedirectPolicyAttribute,
		QNetworkRequest::NoLessSafeRedirectPolicy);
	_reply = _network.get(request);

	// Armed now, so a host that never answers is caught as well, and pushed
	// back every time bytes arrive.
	_stall.setCallback([=] { fail(Failure::Timeout); });
	_stall.callOnce(kStallTimeout);

	QObject::connect(_reply, &QNetworkReply::readyRead, _reply, [=] {
		if (!_reply || !_tmp) {
			return;
		}
		const auto chunk = _reply->readAll();
		if (chunk.isEmpty()) {
			return;
		}
		_stall.callOnce(kStallTimeout);
		_hash.addData(chunk);
		if (_tmp->write(chunk) != chunk.size()) {
			// Disk full or unwritable; stop rather than keep a partial.
			fail(Failure::Storage);
		}
	});
	QObject::connect(_reply, &QNetworkReply::downloadProgress, _reply, [=](
			qint64 received,
			qint64 total) {
		if (_state.stage != Stage::Downloading) {
			return;
		}
		const auto known = (total > 0) ? total : kModelSize;
		const auto percent = (known > 0)
			? int((received * 100) / known)
			: 0;
		const auto clamped = std::clamp(percent, 0, 100);
		if (clamped == _state.progress) {
			return;
		}
		setState({ .stage = Stage::Downloading, .progress = clamped });
	});
	QObject::connect(_reply, &QNetworkReply::finished, _reply, [=] {
		if (!_reply || !_tmp) {
			return;
		}
		_stall.cancel();
		const auto failure = (_reply->error() != QNetworkReply::NoError);
		// Any bytes still buffered after the last readyRead.
		const auto tail = _reply->readAll();
		if (!tail.isEmpty()) {
			_hash.addData(tail);
			_tmp->write(tail);
		}
		const auto flushed = _tmp->flush();
		_tmp->close();
		const auto digest = _hash.result().toHex();
		const auto size = QFileInfo(tmpPath).size();

		if (failure) {
			fail(Failure::Network);
			return;
		} else if (!flushed) {
			fail(Failure::Storage);
			return;
		} else if (size != kModelSize || digest != kModelSha256) {
			fail(Failure::Corrupt);
			return;
		}
		// Verified. Atomic rename into place; drop any stale target first so
		// the rename cannot fail on an existing file.
		const auto finalPath = WhisperModelPath();
		QFile::remove(finalPath);
		cleanupReply();
		if (!QFile::rename(tmpPath, finalPath)) {
			QFile::remove(tmpPath);
			setState({ .stage = Stage::Absent, .failure = Failure::Storage });
			return;
		}
		setState({ .stage = Stage::Ready });
	});
}

void Manager::cancel() {
	if (!_reply) {
		return;
	}
	cleanupReply();
	QFile::remove(WhisperModelPath() + u".tmp"_q);
	setState({ .stage = Stage::Absent });
}

void Manager::remove() {
	// A running download counts as the model too; cancel it first.
	cleanupReply();
	QFile::remove(WhisperModelPath() + u".tmp"_q);
	QFile::remove(WhisperModelPath());
	setState({ .stage = Stage::Absent });
}

} // namespace

QString WhisperModelPath() {
	return cWorkingDir() + u"tdata/luminagram-models/ggml-base-q5_1.bin"_q;
}

bool WhisperModelReady() {
	const auto info = QFileInfo(WhisperModelPath());
	return info.exists() && (info.size() == kModelSize);
}

WhisperModelState CurrentWhisperModelState() {
	auto &manager = Manager::Instance();
	// A model that is already on disk reads as Ready even if this process has
	// never touched the manager (a download from a previous run).
	if (manager.state().stage != Stage::Downloading
		&& WhisperModelReady()) {
		return { .stage = Stage::Ready };
	}
	return manager.state();
}

rpl::producer<> WhisperModelChanges() {
	return Manager::Instance().changes();
}

void DownloadWhisperModel() {
	Manager::Instance().download();
}

void CancelWhisperModelDownload() {
	Manager::Instance().cancel();
}

void DeleteWhisperModel() {
	Manager::Instance().remove();
}

QString WhisperModelFailureText(WhisperModelState::Failure failure) {
	switch (failure) {
	case Failure::None:
		return QString();
	case Failure::Network:
		return Tr(u"LuminaSttModelFailedNetwork"_q);
	case Failure::Timeout:
		return Tr(u"LuminaSttModelFailedTimeout"_q);
	case Failure::Storage:
		return Tr(u"LuminaSttModelFailedStorage"_q);
	case Failure::Corrupt:
		return Tr(u"LuminaSttModelFailedCorrupt"_q);
	}
	return Tr(u"LuminaSttModelFailedNetwork"_q);
}

} // namespace Lumina
