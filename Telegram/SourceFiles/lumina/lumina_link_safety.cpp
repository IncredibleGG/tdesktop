/*
This file is part of LuminaGram,
a fork of Telegram Desktop.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "lumina/lumina_link_safety.h"

#include "base/flat_set.h"
#include "core/application.h"
#include "core/click_handler_types.h"
#include "lang/lang_keys.h"
#include "lumina/lumina_locale.h"
#include "lumina/lumina_settings.h"
#include "lumina/lumina_short_link.h"
#include "lumina/lumina_short_link_parse.h"
#include "ui/boxes/confirm_box.h"
#include "ui/layers/generic_box.h"
#include "ui/layers/show.h"
#include "ui/text/text_entity.h"
#include "ui/widgets/fields/input_field.h"
#include "ui/widgets/labels.h"
#include "ui/basic_click_handlers.h"
#include "window/window_controller.h"
#include "window/window_session_controller.h"

#include "styles/style_calls.h" // groupCallBoxLabel, groupCallField
#include "styles/style_layers.h"
#include "styles/style_widgets.h" // defaultInputField

#include <QtCore/QStringList>
#include <QtCore/QUrl>
#include <QtGui/QKeyEvent>

namespace Lumina {
namespace {

const auto kKeyEnabled = u"linkSafetyCheck"_q;

// Registrable hosts, lower-case, without "www.". Carried over verbatim from
// Android's LUMINA_URL_SHORTENERS so both clients warn about the same set.
[[nodiscard]] const base::flat_set<QString> &Shorteners() {
	static const auto result = base::flat_set<QString>{
		u"adf.ly"_q,
		u"bit.do"_q,
		u"bit.ly"_q,
		u"buff.ly"_q,
		u"clck.ru"_q,
		u"cutt.ly"_q,
		u"db.tt"_q,
		u"goo.gl"_q,
		u"is.gd"_q,
		u"lnkd.in"_q,
		u"ow.ly"_q,
		u"qr.ae"_q,
		u"rb.gy"_q,
		u"rebrand.ly"_q,
		u"s.id"_q,
		u"shorte.st"_q,
		u"shorturl.at"_q,
		u"shrtco.de"_q,
		u"t.co"_q,
		u"t.ly"_q,
		u"tiny.cc"_q,
		u"tinyurl.com"_q,
		u"trib.al"_q,
		u"u.to"_q,
		u"v.gd"_q,
		u"vk.cc"_q,
		u"x.co"_q,
	};
	return result;
}

// The trailing dot is not decoration: "bit.ly." is a fully qualified name that
// resolves to exactly the same host as "bit.ly", so a list lookup that keeps
// it is a one-character bypass of the whole shortener list.
[[nodiscard]] QString RegistrableHost(const QString &host) {
	auto bare = host.toLower();
	while (bare.endsWith(u"."_q)) {
		bare.chop(1);
	}
	return bare.startsWith(u"www."_q) ? bare.mid(4) : bare;
}

// An invalid QUrl means "not our business": anything that does not parse, and
// anything that is not plain http(s). Every other scheme has already been
// handled by the branches above the gate in UiIntegration::handleUrlClick.
[[nodiscard]] QUrl ParseExternalUrl(const QString &url) {
	if (url.isEmpty()) {
		return QUrl();
	}
	const auto parsed = QUrl::fromUserInput(url);
	if (!parsed.isValid()) {
		return QUrl();
	}
	const auto scheme = parsed.scheme().toLower();
	return (scheme == u"http"_q || scheme == u"https"_q) ? parsed : QUrl();
}

// A host with a punycode label, written as its ASCII punycode form.
//
// QUrl reports the host in ACE whichever way it was written, so the encoded
// host alone cannot tell the two apart - and the two need telling apart,
// because a host written in Unicode is already covered by stock (see the
// header) and would get a second box. Testing whether the clicked string
// contains the ACE host is the whole distinction, and it is exact: the ACE
// form appears in the address only if that is how the address was written.
// Testing the string for non-ASCII characters instead would misfire on a
// Unicode path or query under an ASCII host.
[[nodiscard]] bool HasWrittenPunycodeLabel(
		const QUrl &parsed,
		const QString &url) {
	const auto host = parsed.host(QUrl::FullyEncoded).toLower();
	if (host.isEmpty() || !url.contains(host, Qt::CaseInsensitive)) {
		return false;
	}
	for (const auto &label : host.split(u"."_q)) {
		if (label.startsWith(u"xn--"_q)) {
			return true;
		}
	}
	return false;
}

// The warning lines for an address that ParseExternalUrl() accepted. The
// short-link box leaves the shortener line out: it says itself what it does
// about the hidden destination.
[[nodiscard]] std::vector<QString> Warnings(
		const QUrl &parsed,
		const QString &url,
		bool withShortener) {
	auto result = std::vector<QString>();
	if (!parsed.userInfo().isEmpty()) {
		result.push_back(Tr(u"LuminaLinkSafetyWarnMismatch"_q));
	}
	if (HasWrittenPunycodeLabel(parsed, url)) {
		result.push_back(Tr(u"LuminaLinkSafetyWarnPunycode"_q));
	}
	if (withShortener && IsKnownUrlShortener(parsed.host())) {
		result.push_back(Tr(u"LuminaLinkSafetyWarnShortener"_q));
	}
	return result;
}

// The address as it is safe to print.
//
// The password is dropped before anything is displayed. toDisplayString()
// already drops it, but both fallbacks below use toEncoded(), which does not
// - and a box that prints a credential back at the user is a worse outcome
// than the one being warned about.
//
// An address that carries userinfo is always shown in its encoded form, and
// that is the whole point of this function. toDisplayString() decodes percent
// escapes, so the userinfo of
// https://paypal.com%2F%40evil.example@attacker.example/ decodes to
// "paypal.com/@evil.example" and the printed string becomes
// https://paypal.com/@evil.example@attacker.example/ - an address whose
// authority now appears to end at "paypal.com", inside the one box that
// exists to tell the user where the link really goes. Encoded, the escapes
// stay escapes and the authority cannot be forged.
[[nodiscard]] QString DisplayUrl(const QUrl &parsed) {
	const auto hasUserInfo = !parsed.userInfo().isEmpty();
	auto shown = parsed;
	shown.setPassword(QString());
	const auto encoded = QString::fromUtf8(shown.toEncoded());
	if (hasUserInfo) {
		return encoded;
	}
	const auto displayed = shown.toDisplayString();
	return !UrlClickHandler::IsSuspicious(displayed) ? displayed : encoded;
}

// The host, on its own line, in bold.
//
// This replaces bolding the host inside the printed address. Locating a host
// inside a URL string is guesswork that fails in exactly the cases the box
// exists for: stock's BoldDomainInUrl (core/click_handler_types.cpp:52) bolds
// the decoration in https://paypal.com@evil.example because it searches from
// the front, and any search confined to the authority is defeated by an
// escaped "/" in the userinfo, which moves where the authority appears to
// end. A separate line taken straight from QUrl::host() cannot be pointed at
// the wrong span, because it is not a span - it is the parser's own answer to
// "where does this go".
//
// FullyEncoded, so a Unicode homograph host is shown as its punycode.
[[nodiscard]] TextWithEntities DestinationLine(const QUrl &parsed) {
	const auto host = parsed.host(QUrl::FullyEncoded);
	if (host.isEmpty()) {
		return TextWithEntities();
	}
	const auto caption = Tr(u"LuminaLinkSafetyDestination"_q);
	auto result = TextWithEntities{
		.text = caption + u"\n"_q + host,
	};
	result.entities.push_back(EntityInText(
		EntityType::Bold,
		int(caption.size()) + 1,
		int(host.size())));
	return result;
}

[[nodiscard]] QString JoinWarnings(const std::vector<QString> &warnings) {
	auto result = QString();
	for (const auto &warning : warnings) {
		if (!result.isEmpty()) {
			result += u"\n\n"_q;
		}
		result += warning;
	}
	return result;
}

// FlatLabel::setMarkedText() parses with _labelMarkedOptions
// (lib_ui/ui/widgets/labels.cpp:41), which carries TextParseLinks - so a host
// or an address printed as marked text becomes a live link of its own. In an
// ordinary confirmation that is merely odd; in the box that exists to warn
// about this exact address it would be a one-click launcher sitting inside
// the warning. Swallow the activation; selecting and the copy context menu
// still work, they do not go through this filter.
[[nodiscard]] Fn<bool(const ClickHandlerPtr&, Qt::MouseButton)> Inert() {
	return [](const ClickHandlerPtr&, Qt::MouseButton) { return false; };
}

void FillBox(
		not_null<Ui::GenericBox*> box,
		const QString &displayUrl,
		const TextWithEntities &destination,
		const QString &warnings,
		bool dark,
		Fn<void()> open) {
	Ui::ConfirmBox(box, {
		.text = warnings,
		.confirmed = [=](Fn<void()> hide) { hide(); open(); },
		.confirmText = tr::lng_open_link(),
		.labelStyle = dark ? &st::groupCallBoxLabel : nullptr,
		.title = TrValue(u"LuminaLinkSafetyTitle"_q),
	});
	const auto &st = dark ? st::groupCallBoxLabel : st::boxLabel;
	const auto skip = st.style.lineHeight - st::boxPadding.bottom();
	if (!destination.text.isEmpty()) {
		box->addSkip(skip);
		box->addRow(object_ptr<Ui::FlatLabel>(
			box,
			rpl::single(destination),
			st)
		)->setClickHandlerFilter(Inert());
	}
	box->addSkip(skip);

	// The plain-QString overload on purpose: FlatLabel::setText() parses with
	// _labelOptions, which has no TextParseLinks, so the address cannot become
	// a handler in the first place. The filter above is only needed where bold
	// forces marked text.
	const auto label = box->addRow(object_ptr<Ui::FlatLabel>(
		box,
		displayUrl,
		st));
	label->setSelectable(true);
	label->setBreakEverywhere(true);
	label->setContextCopyText(tr::lng_context_copy_link(tr::now));
	label->setClickHandlerFilter(Inert());
}

// The short-link box (requested on 2026-11-01): a link whose host is a known
// shortener names no destination at all, so "Real destination: t.co" would
// be a lie. Instead the box asks the shortener where it leads - one lookup,
// redirects not followed, nothing past the last shortener contacted (see
// lumina_short_link.h) - and shows the answer, or why there is none. The
// address that "Open" opens is in an editable field.
//
//   * The field starts with the clicked address, and is replaced by the
//     answer when one arrives - unless the user has typed into it by then.
//   * "Open" on the clicked address, untouched, opens exactly what the box
//     without this feature would have opened (`openUrl`).
//   * Anything else in the field - the looked-up address or an edited one -
//     must be an absolute http(s) address, goes through the same checks as
//     a clicked link, and opens only once those checks' warnings have been
//     on screen: if there are any, the first "Open" only shows them.
//     t.me and the other Telegram addresses typed here still go to the app's
//     own handler - the open is an ordinary UrlClickHandler::Open().
//   * Cancel, Escape, or closing the box in any other way stops the lookup
//     and leaves everything as it was.

// The address as the box prints it and as the field holds it: DisplayUrl(),
// but encoded as soon as the readable form would carry a character that does
// not print - a direction override in a decoded path reorders what is seen.
[[nodiscard]] QString FieldUrl(const QUrl &url) {
	const auto shown = DisplayUrl(url);
	for (const auto ch : shown.toUcs4()) {
		if (!QChar::isPrint(ch)) {
			auto encoded = url;
			encoded.setPassword(QString());
			return QString::fromUtf8(encoded.toEncoded());
		}
	}
	return shown;
}

// The registrable domain of the looked-up address, for the bold line. Its
// ASCII (punycode) form, like DestinationLine(), so a homograph stays
// visible as one.
[[nodiscard]] QString RegistrableDomain(const QUrl &url) {
	auto host = url.host(QUrl::FullyEncoded).toLower();
	while (host.endsWith(u"."_q)) {
		host.chop(1);
	}
	for (const auto ch : host) {
		if (ch.unicode() > 0x7F) {
			return host;
		}
	}
	const auto offset = ShortLinkParse::RegistrableDomainOffset(
		host.toStdString());
	return host.mid(int(offset));
}

// Only letters, digits, dots and hyphens - nothing the marked-text parser of
// a FlatLabel could read as markdown.
[[nodiscard]] bool IsPlainHost(const QString &host) {
	if (host.isEmpty()) {
		return false;
	}
	for (const auto ch : host) {
		const auto c = ch.unicode();
		const auto plain = (c >= 'a' && c <= 'z')
			|| (c >= 'A' && c <= 'Z')
			|| (c >= '0' && c <= '9')
			|| (c == '.')
			|| (c == '-');
		if (!plain) {
			return false;
		}
	}
	return true;
}

[[nodiscard]] QString FailureReason(const ShortLinkResult &result) {
	switch (result.failure) {
	case ShortLinkFailure::Timeout:
		return Tr(u"LuminaShortLinkFailTimeout"_q);
	case ShortLinkFailure::Status:
		return Tr(
			u"LuminaShortLinkFailStatus"_q,
			QString::number(result.status));
	case ShortLinkFailure::NoTarget:
		return Tr(u"LuminaShortLinkFailNoTarget"_q);
	case ShortLinkFailure::NotWeb:
		return Tr(u"LuminaShortLinkFailNotWeb"_q);
	case ShortLinkFailure::Invalid:
		return Tr(u"LuminaShortLinkFailInvalid"_q);
	case ShortLinkFailure::TooManyHops:
		return Tr(u"LuminaShortLinkFailTooMany"_q);
	case ShortLinkFailure::None:
	case ShortLinkFailure::Network:
		break;
	}
	return Tr(u"LuminaShortLinkFailNetwork"_q);
}

struct CheckedAddress {
	QUrl url; // Invalid unless an absolute http(s) address with a host.
	std::vector<QString> warnings;
};

// The checks a typed or looked-up address goes through before it may be
// opened: the inspector's own, plus the mixed-alphabet test stock applies to
// a clicked link with a Unicode host (UrlClickHandler::IsSuspicious()), here
// on the host alone so that userinfo does not trip it.
[[nodiscard]] CheckedAddress CheckAddress(const QString &text) {
	auto result = CheckedAddress();
	if (text.isEmpty()) {
		return result;
	}
	const auto parsed = QUrl(text, QUrl::TolerantMode);
	const auto scheme = parsed.scheme().toLower();
	if (!parsed.isValid()
		|| parsed.host().isEmpty()
		|| (scheme != u"http"_q && scheme != u"https"_q)) {
		return result;
	}
	result.url = parsed;
	result.warnings = Warnings(parsed, text, true);
	const auto unicodeHost = QUrl::fromAce(
		parsed.host(QUrl::FullyEncoded).toLatin1());
	if (UrlClickHandler::IsSuspicious(unicodeHost)) {
		result.warnings.push_back(Tr(u"LuminaShortLinkWarnMixed"_q));
	}
	return result;
}

struct ShortLinkBoxState {
	QString original; // What the field starts with.
	QString acknowledged; // Whose warnings one "Open" has already shown.
	std::unique_ptr<ShortLinkResolver> resolver;
	bool done = false;
};

void FillShortLinkBox(
		not_null<Ui::GenericBox*> box,
		const QUrl &original,
		const QString &openUrl,
		const std::vector<QString> &warnings,
		bool dark,
		Fn<void(QString, bool)> open) {
	const auto &st = dark ? st::groupCallBoxLabel : st::boxLabel;
	const auto skip = st.style.lineHeight - st::boxPadding.bottom();
	const auto state = box->lifetime().make_state<ShortLinkBoxState>();
	state->original = FieldUrl(original).trimmed();

	box->setTitle(TrValue(u"LuminaLinkSafetyTitle"_q));
	box->setWidth(st::boxWideWidth);

	// What is wrong with the address as written, if anything, then the
	// short link itself.
	auto intro = JoinWarnings(warnings);
	if (!intro.isEmpty()) {
		intro += u"\n\n"_q;
	}
	intro += Tr(u"LuminaShortLinkIntro"_q);
	const auto padding = st::boxPadding;
	box->addRow(
		object_ptr<Ui::FlatLabel>(box, intro, st),
		QMargins(padding.left(), 0, padding.right(), padding.bottom()));
	const auto shortUrl = box->addRow(object_ptr<Ui::FlatLabel>(
		box,
		state->original,
		st));
	shortUrl->setSelectable(true);
	shortUrl->setBreakEverywhere(true);
	shortUrl->setContextCopyText(tr::lng_context_copy_link(tr::now));
	shortUrl->setClickHandlerFilter(Inert());

	// Where it leads: the lookup's progress, then its answer. The full
	// address goes into a plain label of its own - never into marked text,
	// whose markdown parsing could change what an address reads as.
	box->addSkip(skip);
	const auto resolving = Tr(u"LuminaShortLinkResolving"_q);
	const auto status = box->addRow(object_ptr<Ui::FlatLabel>(
		box,
		resolving,
		st));
	status->setClickHandlerFilter(Inert());
	const auto target = box->addRow(object_ptr<Ui::FlatLabel>(box, st));
	target->setSelectable(true);
	target->setBreakEverywhere(true);
	target->setContextCopyText(tr::lng_context_copy_link(tr::now));
	target->setClickHandlerFilter(Inert());

	// The address that "Open" opens.
	box->addSkip(skip);
	const auto editCaption = Tr(u"LuminaShortLinkEdit"_q);
	box->addRow(object_ptr<Ui::FlatLabel>(box, editCaption, st));
	const auto field = box->addRow(object_ptr<Ui::InputField>(
		box,
		dark ? st::groupCallField : st::defaultInputField,
		tr::lng_formatting_link_url(),
		state->original));
	field->setInputMethodHints(Qt::ImhUrlCharactersOnly
		| Qt::ImhNoAutoUppercase
		| Qt::ImhNoPredictiveText);
	const auto notes = box->addRow(object_ptr<Ui::FlatLabel>(box, st));
	notes->setTextColorOverride(st::attentionButtonFg->c);

	const auto refreshNotes = [=] {
		const auto text = field->getLastText().trimmed();
		if (text.isEmpty() || text == state->original) {
			notes->setText(QString());
			return;
		}
		const auto checked = CheckAddress(text);
		if (!checked.url.isValid()) {
			notes->setText(Tr(u"LuminaShortLinkOnlyWeb"_q));
			return;
		}
		auto lines = checked.warnings;
		if (!lines.empty() && state->acknowledged == text) {
			lines.push_back(Tr(
				u"LuminaShortLinkOpenAgain"_q,
				tr::lng_open_link(tr::now)));
		}
		notes->setText(JoinWarnings(lines));
	};
	field->changes() | rpl::on_next(refreshNotes, field->lifetime());

	const auto attempt = [=] {
		if (state->done) {
			return;
		}
		const auto text = field->getLastText().trimmed();
		if (text.isEmpty()) {
			field->showError();
			return;
		}
		// Copied out first: closing the box may destroy this very lambda.
		const auto go = open;
		if (text == state->original) {
			state->done = true;
			const auto url = openUrl;
			box->closeBox();
			go(url, false);
			return;
		}
		const auto checked = CheckAddress(text);
		if (!checked.url.isValid()) {
			refreshNotes();
			field->showError();
			return;
		} else if (!checked.warnings.empty()
			&& state->acknowledged != text) {
			state->acknowledged = text;
			refreshNotes();
			return;
		}
		state->done = true;
		const auto url = QString::fromUtf8(checked.url.toEncoded());
		box->closeBox();
		go(url, true);
	};
	field->submits() | rpl::on_next([=] {
		attempt();
	}, field->lifetime());

	// Enter opens from anywhere in the box. While the field has the focus
	// its own submits() above has already taken the key.
	box->events(
	) | rpl::on_next([=](not_null<QEvent*> e) {
		if (e->type() != QEvent::KeyPress || field->hasFocus()) {
			return;
		}
		const auto k = static_cast<QKeyEvent*>(e.get());
		if (!k->isAutoRepeat()
			&& (k->key() == Qt::Key_Enter || k->key() == Qt::Key_Return)) {
			attempt();
		}
	}, box->lifetime());

	box->addButton(tr::lng_open_link(), attempt);
	box->addButton(tr::lng_cancel(), [=] {
		box->closeBox();
	});
	box->setFocusCallback([=] {
		field->setFocusFast();
	});

	// Last, so that every widget the answer goes into exists. Dropped the
	// moment the box starts closing - Cancel, Escape, "Open" or any other
	// way - and not only once its hide animation is over and it is
	// destroyed: no request goes out after that, and `done` never runs.
	const auto showResult = [=](const ShortLinkResult &result) {
		if (result.resolved()) {
			const auto caption = Tr(
				u"LuminaShortLinkResolved"_q,
				QString::number(result.redirects));
			const auto domain = RegistrableDomain(result.target);
			const auto text = caption + u"\n"_q + domain;
			if (IsPlainHost(domain)) {
				auto marked = TextWithEntities{ .text = text };
				marked.entities.push_back(EntityInText(
					EntityType::Bold,
					int(caption.size()) + 1,
					int(domain.size())));
				status->setMarkedText(marked);
			} else {
				status->setText(text);
			}
			const auto resolved = FieldUrl(result.target).trimmed();
			target->setText(resolved);
			if (field->getLastText().trimmed() == state->original) {
				field->setText(resolved);
				field->setCursorPosition(int(resolved.size()));
			}
		} else {
			status->setTextColorOverride(st::attentionButtonFg->c);
			status->setText(Tr(u"LuminaShortLinkUnresolved"_q)
				+ u"\n"_q
				+ FailureReason(result));
		}
		refreshNotes();
	};
	state->resolver = std::make_unique<ShortLinkResolver>(
		original,
		[=](ShortLinkResult result) { showResult(result); });
	box->boxClosing() | rpl::on_next([=] {
		state->resolver = nullptr;
	}, box->lifetime());
}

} // namespace

bool LinkSafetyEnabled() {
	return Settings::Instance().getBool(kKeyEnabled, false);
}

void SetLinkSafetyEnabled(bool value) {
	// The store is named rather than defaulted. Settings::set() defaults to
	// Store::Prefs and silently RELOCATES a key written through that default
	// (lumina_settings.h:62-67), so a bare set() here is a trap waiting for
	// the day this key moves - every other setter in this batch names it.
	Settings::Instance().set(kKeyEnabled, value, Store::Prefs);
}

rpl::producer<> LinkSafetyChanges() {
	return Settings::Instance().changesFor(kKeyEnabled);
}

bool IsKnownUrlShortener(const QString &host) {
	const auto bare = RegistrableHost(host);
	return !bare.isEmpty() && Shorteners().contains(bare);
}

std::vector<QString> LinkSafetyWarnings(const QString &url) {
	const auto parsed = ParseExternalUrl(url);
	return parsed.isValid()
		? Warnings(parsed, url, true)
		: std::vector<QString>();
}

bool InterceptExternalUrl(const QString &url, const QVariant &context) {
	return InterceptExternalUrl(url, url, context);
}

QVariant LinkSafetyInspected(const QVariant &context) {
	if (!LinkSafetyEnabled()) {
		return context;
	}
	auto my = context.value<ClickHandlerContext>();
	my.luminaLinkSafetyApproved = true;
	return QVariant::fromValue(my);
}

bool InterceptExternalUrl(
		const QString &url,
		const QString &openUrl,
		const QVariant &context) {
	if (!LinkSafetyEnabled()) {
		return false;
	}
	const auto my = context.value<ClickHandlerContext>();
	if (my.luminaLinkSafetyApproved) {
		return false;
	}
	const auto parsed = ParseExternalUrl(url);
	if (!parsed.isValid()) {
		return false;
	}
	const auto warnings = LinkSafetyWarnings(url);
	if (warnings.empty()) {
		return false;
	}
	const auto show = (my.show && my.show->valid())
		? my.show
		: std::shared_ptr<Ui::Show>();
	const auto controller = my.sessionWindow.get();
	const auto window = controller
		? &controller->window()
		: Core::App().activeWindow();
	if (!show && !window) {
		return false;
	}

	const auto open = [=] {
		auto approved = my;
		approved.luminaLinkSafetyApproved = true;
		UrlClickHandler::Open(openUrl, QVariant::fromValue(approved));
	};
	// A known shortener gets the short-link box, which looks the
	// destination up; every other warned address gets the box as before.
	const auto openShort = [=](QString target, bool changed) {
		auto approved = my;
		approved.luminaLinkSafetyApproved = true;
		if (changed) {
			// Only for the address the server handed out; this one was
			// looked up or typed in the box.
			approved.keepWebAuthTokens = false;
		}
		UrlClickHandler::Open(target, QVariant::fromValue(approved));
	};
	auto box = IsKnownUrlShortener(parsed.host())
		? Box(
			FillShortLinkBox,
			parsed,
			openUrl,
			Warnings(parsed, url, false),
			my.dark,
			Fn<void(QString, bool)>(openShort))
		: Box(
			FillBox,
			DisplayUrl(parsed),
			DestinationLine(parsed),
			JoinWarnings(warnings),
			my.dark,
			Fn<void()>(open));
	if (show) {
		show->showBox(std::move(box));
	} else {
		Core::App().hideMediaView();
		window->show(std::move(box));
		window->activate();
	}
	return true;
}

} // namespace Lumina
