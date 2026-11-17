/*
This file is part of LuminaGram,
a fork of Telegram Desktop.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "lumina/lumina_short_link.h"

#include "base/algorithm.h"
#include "lumina/lumina_http_guard.h"
#include "lumina/lumina_link_safety.h"
#include "lumina/lumina_short_link_parse.h"

#include <QtNetwork/QNetworkAccessManager>
#include <QtNetwork/QNetworkReply>
#include <QtNetwork/QNetworkRequest>

#include <string_view>

namespace Lumina {
namespace {

constexpr auto kMaxHops = 5;
constexpr auto kHopTimeout = crl::time(5000);
constexpr auto kTotalTimeout = crl::time(12000);
constexpr auto kMaxBody = 64 * 1024;

// Generic on purpose: not a browser's - t.co answers a browser with a script
// page instead of a 301 - and not ours, so the service learns nothing about
// which app or which version is asking.
constexpr auto kUserAgent = "LinkResolver/1.0";

[[nodiscard]] bool IsWebScheme(const QUrl &url) {
	const auto scheme = url.scheme().toLower();
	return (scheme == u"http"_q) || (scheme == u"https"_q);
}

// What is actually requested: the address without its userinfo, so no
// credential written into a link is ever sent, and without the fragment,
// which HTTP never sends anyway.
[[nodiscard]] QUrl RequestUrl(QUrl url) {
	url.setUserInfo(QString());
	url.setFragment(QString());
	return url;
}

} // namespace

ShortLinkResolver::ShortLinkResolver(
	const QUrl &url,
	Fn<void(ShortLinkResult)> done)
: _done(std::move(done))
, _network(std::make_unique<QNetworkAccessManager>())
, _current(url)
, _hopTimer([=] { fail(ShortLinkFailure::Timeout); })
, _totalTimer([=] { fail(ShortLinkFailure::Timeout); }) {
	_network->setRedirectPolicy(QNetworkRequest::ManualRedirectPolicy);
	_totalTimer.callOnce(kTotalTimeout);
	if (!url.isValid() || !IsWebScheme(url) || url.host().isEmpty()) {
		// Reported from the event loop, so that `done` never runs while
		// the caller is still constructing this object.
		crl::on_main(this, [=] { fail(ShortLinkFailure::Invalid); });
		return;
	}
	request();
}

ShortLinkResolver::~ShortLinkResolver() {
	// Nothing may call back from here on: aborting a reply emits finished()
	// synchronously, and every handler is guarded by the weak pointers that
	// this invalidates, as well as by _finished.
	_finished = true;
	invalidate_weak_ptrs(this);
	if (const auto reply = base::take(_reply)) {
		reply->abort();
	}
	// _network goes after this body, and the replies it owns with it.
}

void ShortLinkResolver::request() {
	const auto url = RequestUrl(_current);
	_hopTimer.callOnce(kHopTimeout);
	StartWithResolvedProxy(_network.get(), url, crl::guard(this, [=] {
		if (!_finished) {
			send(url);
		}
	}));
}

void ShortLinkResolver::send(const QUrl &url) {
	auto request = QNetworkRequest(url);
	request.setTransferTimeout(int(kHopTimeout));
	request.setAttribute(
		QNetworkRequest::RedirectPolicyAttribute,
		QVariant(int(QNetworkRequest::ManualRedirectPolicy)));
	request.setAttribute(
		QNetworkRequest::CookieLoadControlAttribute,
		QVariant(int(QNetworkRequest::Manual)));
	request.setAttribute(
		QNetworkRequest::CookieSaveControlAttribute,
		QVariant(int(QNetworkRequest::Manual)));
	request.setAttribute(
		QNetworkRequest::AuthenticationReuseAttribute,
		QVariant(int(QNetworkRequest::Manual)));
	request.setAttribute(
		QNetworkRequest::CacheLoadControlAttribute,
		QVariant(int(QNetworkRequest::AlwaysNetwork)));
	request.setAttribute(
		QNetworkRequest::CacheSaveControlAttribute,
		QVariant(false));
	request.setHeader(
		QNetworkRequest::UserAgentHeader,
		QVariant(QByteArray(kUserAgent)));
	request.setRawHeader("Accept", "*/*");
	// Qt would otherwise fill this in from the system locale.
	request.setRawHeader("Accept-Language", "en");

	_body.clear();
	const auto reply = (_method == Method::Head)
		? _network->head(request)
		: _network->get(request);
	_reply = reply;
	const auto weak = base::make_weak(this);
	QObject::connect(reply, &QNetworkReply::readyRead, reply, [=] {
		if (const auto strong = weak.get()) {
			strong->received(reply);
		}
	});
	QObject::connect(reply, &QNetworkReply::finished, reply, [=] {
		if (const auto strong = weak.get()) {
			strong->finished(reply);
		}
	});
}

void ShortLinkResolver::received(not_null<QNetworkReply*> reply) {
	if (_finished || reply != _reply || _method != Method::Get) {
		return;
	}
	const auto room = kMaxBody - int(_body.size());
	if (room > 0) {
		_body.append(reply->read(room));
	}
	if (_body.size() >= kMaxBody) {
		// Enough to find a refresh declaration in the head of the page;
		// the rest is not downloaded.
		judge(reply);
	}
}

void ShortLinkResolver::finished(not_null<QNetworkReply*> reply) {
	if (_finished || reply != _reply) {
		return;
	}
	if (_method == Method::Get) {
		const auto room = kMaxBody - int(_body.size());
		if (room > 0) {
			_body.append(reply->read(room));
		}
	}
	judge(reply);
}

void ShortLinkResolver::release(not_null<QNetworkReply*> reply) {
	if (_reply == reply) {
		_reply = nullptr;
	}
	if (reply->isRunning()) {
		// Its finished() now finds _reply changed and does nothing.
		reply->abort();
	}
	reply->deleteLater();
}

void ShortLinkResolver::judge(not_null<QNetworkReply*> reply) {
	_hopTimer.cancel();
	const auto status = reply->attribute(
		QNetworkRequest::HttpStatusCodeAttribute).toInt();
	const auto location = reply->rawHeader("Location");
	const auto error = reply->error();
	release(reply);

	if (status >= 300 && status < 400 && !location.isEmpty()) {
		hop(QUrl(QString::fromUtf8(location).trimmed()));
		return;
	} else if (status <= 0) {
		fail((error == QNetworkReply::TimeoutError
			|| error == QNetworkReply::OperationCanceledError)
			? ShortLinkFailure::Timeout
			: ShortLinkFailure::Network);
		return;
	} else if (_method == Method::Head) {
		// No target in the answer to HEAD: some services refuse it (405,
		// 400), and some answer it with the page itself (200). Ask again,
		// the way a browser would, for the same address.
		_method = Method::Get;
		request();
		return;
	} else if (status >= 200 && status < 300) {
		const auto meta = ShortLinkParse::MetaRefreshUrl(std::string_view(
			_body.constData(),
			std::size_t(_body.size())));
		_body.clear();
		if (!meta.empty()) {
			hop(QUrl(QString::fromStdString(meta)));
			return;
		}
		fail(ShortLinkFailure::NoTarget);
		return;
	} else if (status < 400) {
		fail(ShortLinkFailure::NoTarget);
		return;
	}
	fail(ShortLinkFailure::Status, status);
}

void ShortLinkResolver::hop(const QUrl &relative) {
	if (!relative.isValid()) {
		fail(ShortLinkFailure::Invalid);
		return;
	}
	auto next = _current.resolved(relative);
	if (!next.hasFragment() && _current.hasFragment()) {
		// A redirect without a fragment keeps the one of the address it
		// came from, as browsers do (RFC 9110, 10.2.2).
		next.setFragment(_current.fragment(QUrl::FullyEncoded));
	}
	++_redirects;
	if (!next.isValid()) {
		fail(ShortLinkFailure::Invalid);
		return;
	} else if (!IsWebScheme(next)) {
		fail(ShortLinkFailure::NotWeb);
		return;
	} else if (next.host().isEmpty()) {
		fail(ShortLinkFailure::Invalid);
		return;
	} else if (!IsKnownUrlShortener(next.host())) {
		// The first address that is not a short link is the answer, and it
		// is not contacted.
		finish({ .target = next, .redirects = _redirects });
		return;
	} else if (_redirects >= kMaxHops) {
		fail(ShortLinkFailure::TooManyHops);
		return;
	}
	_current = next;
	_method = Method::Head;
	request();
}

void ShortLinkResolver::fail(ShortLinkFailure failure, int status) {
	finish({
		.redirects = _redirects,
		.failure = failure,
		.status = status,
	});
}

void ShortLinkResolver::finish(ShortLinkResult result) {
	if (_finished) {
		return;
	}
	_finished = true;
	_hopTimer.cancel();
	_totalTimer.cancel();
	if (const auto reply = _reply) {
		release(reply);
	}
	if (const auto done = base::take(_done)) {
		done(std::move(result));
	}
}

} // namespace Lumina
