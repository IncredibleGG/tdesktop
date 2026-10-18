/*
This file is part of LuminaGram,
a fork of Telegram Desktop.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include <QtCore/QUrl>
#include <QtCore/QVariant>
#include <QtNetwork/QNetworkReply>
#include <QtNetwork/QNetworkRequest>

namespace Lumina {

// Redirect policy for every request that carries the user's API key - an
// Authorization header, DeepL's auth header, Google's X-goog-api-key.
//
// Qt 6 follows redirects on its own (NoLessSafeRedirectPolicy), including to
// another host, and a followed redirect repeats every raw header of the
// original request - the key included. A relay or a hijacked endpoint could
// therefore collect the key with nothing more than a 307. So such a request
// follows a redirect only when it stays on the same host and does not drop
// from https to http; anything else is aborted and reported as a network
// failure with no HTTP status.
//
// What is deliberately NOT refused: a plain http:// endpoint the user typed
// (some relays only speak http), an http -> https upgrade, and a redirect
// within the same host (a trailing-slash or path fix-up).
//
// Use: KeyedRequest(request) before sending, WatchKeyedRedirects(reply, url)
// right after, and KeyedRedirectRefused(reply) in the finished handler.

[[nodiscard]] inline const char *KeyedRedirectRefusedProperty() {
	return "luminaKeyedRedirectRefused";
}

[[nodiscard]] inline bool KeyedRedirectAllowed(
		const QUrl &origin,
		const QUrl &target) {
	if (!target.isValid()) {
		return false;
	}
	const auto https = QLatin1String("https");
	const auto http = QLatin1String("http");
	const auto from = origin.scheme().toLower();
	const auto to = target.scheme().toLower();
	if (to != https && to != http) {
		return false;
	} else if (from == https && to != https) {
		return false;
	}
	const auto host = origin.host();
	return !host.isEmpty()
		&& (host.compare(target.host(), Qt::CaseInsensitive) == 0);
}

inline void KeyedRequest(QNetworkRequest &request) {
	request.setAttribute(
		QNetworkRequest::RedirectPolicyAttribute,
		QVariant(int(QNetworkRequest::UserVerifiedRedirectPolicy)));
}

inline void WatchKeyedRedirects(QNetworkReply *reply, const QUrl &origin) {
	QObject::connect(reply, &QNetworkReply::redirected, reply, [=](
			const QUrl &target) {
		if (KeyedRedirectAllowed(origin, origin.resolved(target))) {
			Q_EMIT reply->redirectAllowed();
		} else {
			reply->setProperty(KeyedRedirectRefusedProperty(), true);
			reply->abort();
		}
	});
}

[[nodiscard]] inline bool KeyedRedirectRefused(QNetworkReply *reply) {
	return reply->property(KeyedRedirectRefusedProperty()).toBool();
}

} // namespace Lumina
