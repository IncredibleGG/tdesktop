/*
This file is part of LuminaGram,
a fork of Telegram Desktop.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "base/basic_types.h"
#include "base/timer.h"
#include "base/weak_ptr.h"

#include <QtCore/QByteArray>
#include <QtCore/QUrl>

#include <memory>

class QNetworkAccessManager;
class QNetworkReply;

namespace Lumina {

// Looks up where a short link really leads, for the link safety box
// (lumina_link_safety.h), WITHOUT following it.
//
// Every hop is one request that this class makes itself, with redirects
// switched off: it reads the Location header and decides on its own whether
// to go on. So nothing past the last short-link service is ever contacted -
// the destination site does not see the request, and a redirect to a scheme
// that is not http(s) is reported, never followed.
//
//   * At most kMaxHops requests to different addresses, ~5 s each and
//     ~12 s in all; it stops at the first address whose host is not a known
//     shortener (IsKnownUrlShortener()) - that address is the answer.
//   * HEAD first; GET when HEAD names no target (405 / 400 / 200 ...). From
//     a GET only the first 64 KB are read, for a
//     <meta http-equiv="refresh"> fallback.
//   * No cookies are sent or stored, no credentials (the userinfo of the
//     address is dropped before the request, and no cached authentication is
//     reused), no Referer, and a generic User-Agent that names neither the
//     app nor its version.
//   * The proxy is the one tdesktop uses for the whole application, looked up
//     off the main thread (StartWithResolvedProxy() in lumina_http_guard.h).
//   * Nothing is logged - not the address, not the answer.
//
// Each resolver has a network manager of its own, so no connection, cookie
// or cache entry outlives it. Destroying it cancels the lookup, and `done`
// is then never called.

enum class ShortLinkFailure {
	None,
	Network, // No answer at all.
	Timeout,
	Status, // The service answered with an HTTP error (`status`).
	NoTarget, // It answered, but named no address.
	NotWeb, // A hop leads to a scheme other than http(s).
	Invalid, // A hop names an address that does not parse.
	TooManyHops,
};

struct ShortLinkResult {
	QUrl target; // Valid only when resolved.
	int redirects = 0;
	ShortLinkFailure failure = ShortLinkFailure::None;
	int status = 0; // For ShortLinkFailure::Status.

	[[nodiscard]] bool resolved() const {
		return (failure == ShortLinkFailure::None) && target.isValid();
	}
};

class ShortLinkResolver final : public base::has_weak_ptr {
public:
	// Starts right away. `done` runs once, on the main thread - unless the
	// resolver is destroyed first.
	ShortLinkResolver(const QUrl &url, Fn<void(ShortLinkResult)> done);
	~ShortLinkResolver();

private:
	enum class Method {
		Head,
		Get,
	};

	void request();
	void send(const QUrl &url);
	void received(not_null<QNetworkReply*> reply);
	void finished(not_null<QNetworkReply*> reply);
	void judge(not_null<QNetworkReply*> reply);
	void release(not_null<QNetworkReply*> reply);
	void hop(const QUrl &relative);
	void fail(ShortLinkFailure failure, int status = 0);
	void finish(ShortLinkResult result);

	Fn<void(ShortLinkResult)> _done;
	std::unique_ptr<QNetworkAccessManager> _network;
	QNetworkReply *_reply = nullptr;
	QUrl _current;
	Method _method = Method::Head;
	QByteArray _body;
	int _redirects = 0;
	bool _finished = false;
	base::Timer _hopTimer;
	base::Timer _totalTimer;

};

} // namespace Lumina
