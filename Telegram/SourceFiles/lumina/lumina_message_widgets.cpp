/*
This file is part of LuminaGram,
a fork of Telegram Desktop.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "lumina/lumina_message_widgets.h"

#include "base/weak_ptr.h"
#include "core/click_handler_types.h"
#include "data/data_session.h"
#include "history/history.h"
#include "history/history_item.h"
#include "history/view/history_view_cursor_state.h"
#include "history/view/history_view_element.h"
#include "history/view/media/history_view_media.h"
#include "lumina/lumina_locale.h"
#include "lumina/lumina_settings.h"
#include "main/main_session.h"
#include "settings/sections/settings_lumina_privacy.h"
#include "ui/chat/chat_style.h"
#include "ui/chat/unsupported_notice.h"
#include "ui/effects/ripple_animation.h"
#include "ui/painter.h"
#include "ui/vertical_list.h"
#include "ui/widgets/buttons.h"
#include "ui/wrap/vertical_layout.h"
#include "window/window_session_controller.h"
#include "styles/style_chat.h"
#include "styles/style_settings.h"

namespace Lumina {
namespace {

using HistoryView::Element;
using HistoryView::Media;
using HistoryView::PaintContext;
using HistoryView::StateRequest;
using HistoryView::TextState;

const auto kKeyOff = u"messageWidgetsOff"_q;

// Old entries are pruned on the way in; past this many live ones the oldest
// are forgotten (they then switch on their next layout instead of at once).
constexpr auto kRememberedLimit = 4096;

struct Remembered {
	base::weak_ptr<Main::Session> session;
	FullMsgId id;
};

[[nodiscard]] std::vector<Remembered> &RememberedList() {
	static auto result = std::vector<Remembered>();
	return result;
}

[[nodiscard]] HistoryItem *Lookup(const Remembered &entry) {
	const auto session = entry.session.get();
	return session ? session->data().message(entry.id) : nullptr;
}

void RefreshRememberedWidgets() {
	auto &list = RememberedList();
	auto sessions = std::vector<Main::Session*>();
	for (auto i = begin(list); i != end(list);) {
		const auto item = Lookup(*i);
		if (!item) {
			i = list.erase(i);
			continue;
		}
		const auto session = &item->history()->session();
		session->data().requestItemViewRefresh(item);
		if (ranges::find(sessions, session) == end(sessions)) {
			sessions.push_back(session);
		}
		++i;
	}
	for (const auto session : sessions) {
		// Same reason as Tgw::Runtime::WalletAvailabilityChanged(): nothing
		// else flushes a refresh requested from outside applyUpdates.
		session->data().sendHistoryChangeNotifications();
	}
}

[[nodiscard]] rpl::producer<bool> ShownValue() {
	return rpl::single(
		rpl::empty
	) | rpl::then(
		MessageWidgetsChanges()
	) | rpl::map([] {
		return !MessageWidgetsDisabled();
	});
}

// Upstream's HistoryView::UnsupportedWidget card with LuminaGram's texts and
// a button to the setting instead of "Update".
class WidgetOff final : public Media {
public:
	explicit WidgetOff(not_null<Element*> parent);

	QSize countOptimalSize() override;
	QSize countCurrentSize(int newWidth) override;
	void draw(Painter &p, const PaintContext &context) const override;
	TextState textState(QPoint point, StateRequest request) const override;

	[[nodiscard]] bool toggleSelectionByHandlerClick(
		const ClickHandlerPtr &p) const override;
	[[nodiscard]] bool dragItemByHandler(
		const ClickHandlerPtr &p) const override;
	void clickHandlerPressedChanged(
		const ClickHandlerPtr &handler,
		bool pressed) override;

	[[nodiscard]] bool hideMessageText() const override;
	[[nodiscard]] bool needsBubble() const override;
	[[nodiscard]] bool customInfoLayout() const override;

private:
	[[nodiscard]] int topSkip() const;
	[[nodiscard]] int bottomSkip() const;
	[[nodiscard]] QRect cardRect() const;

	Ui::UnsupportedNoticeCard _card;
	std::unique_ptr<Ui::RippleAnimation> _ripple;
	ClickHandlerPtr _link;
	mutable QPoint _lastPoint;

};

WidgetOff::WidgetOff(not_null<Element*> parent)
: Media(parent)
, _link(std::make_shared<LambdaClickHandler>([](ClickContext context) {
	const auto my = context.other.value<ClickHandlerContext>();
	if (const auto controller = my.sessionWindow.get()) {
		controller->showSettings(::Settings::LuminaPrivacyId());
	}
})) {
	_card.setTexts(
		Tr(u"LuminaMessageWidgetOffTitle"_q),
		Tr(u"LuminaMessageWidgetOffText"_q),
		Tr(u"LuminaMessageWidgetOffButton"_q));
}

QSize WidgetOff::countOptimalSize() {
	const auto height = _card.resizeGetHeight(st::unsupportedNoticeMaxWidth);
	return {
		st::msgPadding.left() + _card.width() + st::msgPadding.right(),
		topSkip() + height + bottomSkip(),
	};
}

QSize WidgetOff::countCurrentSize(int newWidth) {
	const auto available = std::max(
		newWidth - st::msgPadding.left() - st::msgPadding.right(),
		0);
	const auto height = _card.resizeGetHeight(available);
	return { newWidth, topSkip() + height + bottomSkip() };
}

void WidgetOff::draw(Painter &p, const PaintContext &context) const {
	_card.paintInBubble(p, context, cardRect(), _ripple.get());
}

TextState WidgetOff::textState(QPoint point, StateRequest request) const {
	auto result = TextState(_parent);
	const auto button = _card.buttonRect().translated(cardRect().topLeft());
	if (button.contains(point)) {
		result.link = _link;
		_lastPoint = point - button.topLeft();
	}
	return result;
}

bool WidgetOff::toggleSelectionByHandlerClick(
		const ClickHandlerPtr &p) const {
	return true;
}

bool WidgetOff::dragItemByHandler(const ClickHandlerPtr &p) const {
	return true;
}

void WidgetOff::clickHandlerPressedChanged(
		const ClickHandlerPtr &handler,
		bool pressed) {
	if (handler == _link) {
		_card.buttonPressedChanged(
			_ripple,
			pressed,
			_lastPoint,
			[=] { repaint(); });
	}
}

bool WidgetOff::hideMessageText() const {
	return false;
}

bool WidgetOff::needsBubble() const {
	return true;
}

bool WidgetOff::customInfoLayout() const {
	return false;
}

int WidgetOff::topSkip() const {
	return isBubbleTop() ? st::msgPadding.left() : 0;
}

int WidgetOff::bottomSkip() const {
	if (!isBubbleBottom()) {
		return 0;
	}
	const auto info = _parent->hidesBottomInfo()
		? 0
		: (st::mediaInBubbleSkip + _parent->skipBlockHeight());
	return st::msgPadding.bottom() + info;
}

QRect WidgetOff::cardRect() const {
	return QRect(
		st::msgPadding.left(),
		topSkip(),
		_card.width(),
		_card.height());
}

} // namespace

bool MessageWidgetsDisabled() {
	return Settings::Instance().getBool(kKeyOff, false);
}

void SetMessageWidgetsDisabled(bool disabled) {
	if (MessageWidgetsDisabled() == disabled) {
		return;
	}
	Settings::Instance().set(kKeyOff, disabled, Store::Prefs);
	RefreshRememberedWidgets();
}

rpl::producer<> MessageWidgetsChanges() {
	return Settings::Instance().changesFor(kKeyOff);
}

void RememberMessageWidget(not_null<HistoryItem*> item) {
	const auto session = &item->history()->session();
	const auto id = item->fullId();
	auto &list = RememberedList();
	for (const auto &entry : list) {
		if (entry.id == id && entry.session.get() == session) {
			return;
		}
	}
	if (int(list.size()) >= kRememberedLimit) {
		list.erase(ranges::remove_if(list, [](const Remembered &entry) {
			return !Lookup(entry);
		}), end(list));
		if (int(list.size()) >= kRememberedLimit) {
			list.erase(begin(list), begin(list) + (kRememberedLimit / 2));
		}
	}
	list.push_back({ base::make_weak(session), id });
}

std::unique_ptr<HistoryView::Media> CreateMessageWidgetOffView(
		not_null<HistoryView::Element*> parent) {
	return std::make_unique<WidgetOff>(parent);
}

void AddMessageWidgetRows(
		not_null<Ui::VerticalLayout*> container,
		not_null<Window::SessionController*>) {
	Ui::AddSkip(container);
	const auto button = container->add(object_ptr<Ui::SettingsButton>(
		container,
		TrValue(u"LuminaMessageWidgetsRow"_q),
		st::settingsButtonNoIcon
	))->toggleOn(ShownValue());
	button->toggledChanges(
	) | rpl::on_next([](bool shown) {
		SetMessageWidgetsDisabled(!shown);
	}, button->lifetime());
	Ui::AddSkip(container);
	Ui::AddDividerText(container, TrValue(u"LuminaMessageWidgetsInfo"_q));
}

} // namespace Lumina
