/*
This file is part of LuminaGram,
a fork of Telegram Desktop.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "lumina/lumina_format_bar.h"

#include "base/event_filter.h"
#include "base/flat_map.h"
#include "base/qt_signal_producer.h"
#include "base/timer.h"
#include "base/unique_qptr.h"
#include "base/weak_ptr.h"
#include "dialogs/ui/dialogs_pill.h"
#include "lang/lang_keys.h"
#include "lumina/lumina_settings.h"
#include "ui/screen_reader_mode.h"
#include "ui/ui_utility.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/fields/input_field.h"
#include "ui/widgets/shadow.h"
#include "ui/widgets/tooltip.h"
#include "styles/palette.h"
#include "styles/style_lumina.h"

#include <QtGui/QCursor>
#include <QtGui/QGuiApplication>
#include <QtGui/QKeySequence>
#include <QtGui/QMouseEvent>
#include <QtGui/QPainter>
#include <QtGui/QTextCursor>
#include <QtWidgets/QTextEdit>

namespace Lumina {
namespace {

// A keyboard selection (Shift+arrows, Ctrl+A) grows one step at a time; the
// bar waits for it to settle instead of flickering after every key.
constexpr auto kKeyboardShowDelay = crl::time(200);

// The host panels the bar makes way for (emoji / sticker panel, autocomplete,
// inline results) come and go without telling the field anything, so while
// the bar is wanted it re-checks them - and its position - this often. Runs
// only while the field has focus and a selection; never while the feature is
// off.
constexpr auto kWatchInterval = crl::time(250);

[[nodiscard]] QString SettingsKey() {
	return u"formattingBar"_q;
}

enum class Action : uchar {
	Tag,
	Link,
	Clear,
};

enum class ButtonState : uchar {
	Disabled,
	Inactive,
	Active,
};

// The monospace button toggles kTagCode, which the field itself turns into
// kTagPre for a selection of whole lines. The shortcut path
// (InputField::executeMarkdownAction) requires both to be allowed; so does
// this.
[[nodiscard]] bool TagSupported(
		const Ui::MarkdownEnabledState &state,
		const QString &tag) {
	using Field = Ui::InputField;
	return state.enabledForTag(tag)
		&& ((tag != Field::kTagCode) || state.enabledForTag(Field::kTagPre));
}

[[nodiscard]] bool TagActive(
		not_null<const Ui::InputField*> field,
		const QString &tag) {
	using Field = Ui::InputField;
	if (tag == Field::kTagCode) {
		return field->isMarkdownTagActive(Field::kTagCode)
			|| field->isMarkdownTagActive(Field::kTagPre);
	} else if (tag == Field::kTagBlockquote) {
		return field->isMarkdownTagActive(Field::kTagBlockquote)
			|| field->isMarkdownTagActive(Field::kTagBlockquoteCollapsed);
	}
	return field->isMarkdownTagActive(tag);
}

[[nodiscard]] QString TooltipWithShortcut(
		const QString &name,
		const QKeySequence &shortcut) {
	const auto keys = shortcut.toString(QKeySequence::NativeText);
	return keys.isEmpty() ? name : (name + u" ("_q + keys + QChar(')'));
}

class FormatButton final
	: public Ui::IconButton
	, public Ui::AbstractTooltipShower {
public:
	FormatButton(QWidget *parent, const style::icon &icon);

	void setTooltipText(const QString &text);
	void setButtonState(ButtonState state);

	// AbstractTooltipShower interface.
	QString tooltipText() const override;
	QPoint tooltipPos() const override;
	bool tooltipWindowActive() const override;

protected:
	void mouseMoveEvent(QMouseEvent *e) override;
	void leaveEventHook(QEvent *e) override;

private:
	QString _tooltip;

};

FormatButton::FormatButton(QWidget *parent, const style::icon &icon)
: IconButton(parent, st::luminaFormatBarButton) {
	setIconOverride(&icon, &icon);
}

void FormatButton::setTooltipText(const QString &text) {
	_tooltip = text;
	setAccessibleName(text);
}

// The Instant View editor toolbar's button states (Iv::Editor::
// SetupToolbarButton), spelled out locally so that this file does not
// depend on the editor's internals.
//
// Always re-applied rather than cached: hiding a button clears its ripple
// (RippleButton::clearState) while leaving forceRippled() set, so an active
// button shown again has to be force-rippled anew.
void FormatButton::setButtonState(ButtonState state) {
	const auto disabled = (state == ButtonState::Disabled);
	const auto active = (state == ButtonState::Active);
	setAttribute(Qt::WA_TransparentForMouseEvents, disabled);
	setPointerCursor(!disabled);
	if (forceRippled()) {
		setForceRippled(false, anim::type::instant);
	}
	if (active) {
		setRippleColorOverride(&st::lightButtonBgOver);
		setForceRippled(true, anim::type::instant);
		setIconColorOverride(st::windowActiveTextFg->c);
	} else {
		setRippleColorOverride(nullptr);
		setIconColorOverride(disabled
			? std::optional<QColor>(st::windowSubTextFg->c)
			: std::nullopt);
	}
}

QString FormatButton::tooltipText() const {
	return _tooltip;
}

QPoint FormatButton::tooltipPos() const {
	return QCursor::pos();
}

bool FormatButton::tooltipWindowActive() const {
	return isVisible() && Ui::AppInFocus() && Ui::InFocusChain(window());
}

void FormatButton::mouseMoveEvent(QMouseEvent *e) {
	IconButton::mouseMoveEvent(e);
	if (rect().contains(e->pos())) {
		Ui::Tooltip::Show(1000, this);
	} else {
		Ui::Tooltip::Hide();
	}
}

void FormatButton::leaveEventHook(QEvent *e) {
	IconButton::leaveEventHook(e);
	Ui::Tooltip::Hide();
}

class FormatBar final : public Ui::RpWidget {
public:
	FormatBar(QWidget *parent, not_null<Ui::InputField*> field);

	// The controller's say on visibility. A host that shows all of its
	// children at once (SectionWidget::showChildren() after a slide) cannot
	// bring the bar up behind the controller's back.
	void setWanted(bool wanted);

	// Button visibility, states, tooltips and size, from the field as it is.
	void refresh();

	[[nodiscard]] QMargins shadowMargins() const;

protected:
	void paintEvent(QPaintEvent *e) override;
	void mousePressEvent(QMouseEvent *e) override;
	void mouseReleaseEvent(QMouseEvent *e) override;
	void mouseDoubleClickEvent(QMouseEvent *e) override;
	void setVisibleHook(bool visible) override;

private:
	struct Entry {
		not_null<FormatButton*> button;
		Action action = Action::Tag;
		QString tag;
		Fn<QString()> name;
		QKeySequence shortcut;
		bool secondGroup = false;
	};

	void add(
		Action action,
		QString tag,
		const style::icon &icon,
		Fn<QString()> name,
		QKeySequence shortcut,
		bool secondGroup);
	void apply(Action action, const QString &tag);
	void layout();

	const not_null<Ui::InputField*> _field;
	const Ui::BoxShadow _shadow;
	const QMargins _shadowMargins;
	std::vector<Entry> _entries;
	int _dividerX = -1;
	bool _wanted = false;

};

FormatBar::FormatBar(QWidget *parent, not_null<Ui::InputField*> field)
: RpWidget(parent)
, _field(field)
, _shadow(st::luminaFormatBarShadow)
, _shadowMargins(_shadow.extend()) {
	using Field = Ui::InputField;
	const auto tag = [&](
			const QString &id,
			const style::icon &icon,
			Fn<QString()> name,
			QKeySequence shortcut) {
		add(Action::Tag, id, icon, std::move(name), shortcut, false);
	};
	tag(
		Field::kTagBold,
		st::luminaFormatBarBold,
		[] { return tr::lng_menu_formatting_bold(tr::now); },
		QKeySequence(QKeySequence::Bold));
	tag(
		Field::kTagItalic,
		st::luminaFormatBarItalic,
		[] { return tr::lng_menu_formatting_italic(tr::now); },
		QKeySequence(QKeySequence::Italic));
	tag(
		Field::kTagUnderline,
		st::luminaFormatBarUnderline,
		[] { return tr::lng_menu_formatting_underline(tr::now); },
		QKeySequence(QKeySequence::Underline));
	tag(
		Field::kTagStrikeOut,
		st::luminaFormatBarStrikeOut,
		[] { return tr::lng_menu_formatting_strike_out(tr::now); },
		Ui::kStrikeOutSequence);
	tag(
		Field::kTagCode,
		st::luminaFormatBarMonospace,
		[] { return tr::lng_menu_formatting_monospace(tr::now); },
		Ui::kMonospaceSequence);
	tag(
		Field::kTagSpoiler,
		st::luminaFormatBarSpoiler,
		[] { return tr::lng_menu_formatting_spoiler(tr::now); },
		Ui::kSpoilerSequence);
	tag(
		Field::kTagBlockquote,
		st::luminaFormatBarQuote,
		[] { return tr::lng_menu_formatting_blockquote(tr::now); },
		Ui::kBlockquoteSequence);
	add(
		Action::Link,
		QString(),
		st::luminaFormatBarLink,
		[=] {
			return _field->hasCurrentMarkdownLink()
				? tr::lng_menu_formatting_link_edit(tr::now)
				: tr::lng_menu_formatting_link_create(tr::now);
		},
		Ui::kEditLinkSequence,
		true);
	add(
		Action::Clear,
		QString(),
		st::luminaFormatBarClear,
		[] { return tr::lng_menu_formatting_clear(tr::now); },
		Ui::kClearFormatSequence,
		true);

	hide();
	refresh();
}

void FormatBar::add(
		Action action,
		QString tag,
		const style::icon &icon,
		Fn<QString()> name,
		QKeySequence shortcut,
		bool secondGroup) {
	const auto button = Ui::CreateChild<FormatButton>(this, icon);

	// Hidden explicitly: refresh() shows the supported ones. A child that
	// was never hidden explicitly would come up with the bar even when its
	// tag is not allowed in this field.
	button->hide();
	button->setClickedCallback([=] {
		Ui::Tooltip::Hide();
		apply(action, tag);
	});
	_entries.push_back({
		.button = button,
		.action = action,
		.tag = std::move(tag),
		.name = std::move(name),
		.shortcut = std::move(shortcut),
		.secondGroup = secondGroup,
	});
}

void FormatBar::apply(Action action, const QString &tag) {
	// The bar is only up with a selection, but a click can race the
	// selection going away. Without one these calls would change the typing
	// format instead (the Instant View editor's behaviour), which the
	// shortcuts never do - so do nothing at all.
	if (!_field->textCursor().hasSelection()
		|| _field->markdownEnabledState().disabled()) {
		return;
	}
	switch (action) {
	case Action::Tag:
		if (TagSupported(_field->markdownEnabledState(), tag)) {
			_field->toggleCurrentMarkdownTag(tag);
		}
		break;
	case Action::Link:
		// Opens the stock link box (DefaultEditLinkCallback). The box takes
		// the focus, which hides the bar until the field gets it back.
		_field->editCurrentMarkdownLink();
		return;
	case Action::Clear:
		_field->clearCurrentMarkdown();
		break;
	}
	refresh();
}

void FormatBar::refresh() {
	const auto state = _field->markdownEnabledState();
	const auto cursor = _field->textCursor();
	const auto from = cursor.selectionStart();
	const auto till = cursor.selectionEnd();
	const auto hasTags = (till > from)
		&& !_field->getTextWithTagsPart(from, till).tags.isEmpty();
	const auto hasLink = _field->hasCurrentMarkdownLink();

	auto relayout = false;
	for (const auto &entry : _entries) {
		auto visible = true;
		auto buttonState = ButtonState::Inactive;
		switch (entry.action) {
		case Action::Tag:
			visible = TagSupported(state, entry.tag);
			if (visible && TagActive(_field, entry.tag)) {
				buttonState = ButtonState::Active;
			}
			break;
		case Action::Link:
			if (hasLink) {
				buttonState = ButtonState::Active;
			}
			break;
		case Action::Clear:
			// The context menu's rule: nothing to clear, nothing to press.
			if (!hasTags) {
				buttonState = ButtonState::Disabled;
			}
			break;
		}
		if (entry.button->isHidden() == visible) {
			entry.button->setVisible(visible);
			relayout = true;
		}
		if (visible) {
			entry.button->setButtonState(buttonState);
			entry.button->setTooltipText(
				TooltipWithShortcut(entry.name(), entry.shortcut));
		}
	}
	if (relayout || width() <= 0) {
		layout();
	}
}

// Laid out from the left in reading order and mirrored for a right-to-left
// interface (moveToLeft), as upstream's editor pill does.
void FormatBar::layout() {
	const auto size = st::luminaFormatBarButtonSize;
	const auto padding = st::luminaFormatBarPadding;
	const auto top = _shadowMargins.top() + padding;
	const auto walk = [&](Fn<void(not_null<FormatButton*>, int)> place) {
		auto left = _shadowMargins.left() + padding;
		auto firstGroup = false;
		auto secondGroupStarted = false;
		_dividerX = -1;
		for (const auto &entry : _entries) {
			if (entry.button->isHidden()) {
				continue;
			}
			if (entry.secondGroup && !secondGroupStarted) {
				secondGroupStarted = true;
				if (firstGroup) {
					const auto skip = st::luminaFormatBarGroupSkip;
					_dividerX = left + (skip - st::lineWidth) / 2;
					left += skip;
				}
			} else if (!entry.secondGroup) {
				firstGroup = true;
			}
			if (place) {
				place(entry.button, left);
			}
			left += size;
		}
		return left;
	};
	const auto outerWidth = walk(nullptr) + padding + _shadowMargins.right();
	resize(outerWidth, top + size + padding + _shadowMargins.bottom());
	walk([&](not_null<FormatButton*> button, int left) {
		button->moveToLeft(left, top, outerWidth);
	});
	update();
}

QMargins FormatBar::shadowMargins() const {
	return _shadowMargins;
}

void FormatBar::setWanted(bool wanted) {
	_wanted = wanted;
	setVisible(wanted);
}

void FormatBar::setVisibleHook(bool visible) {
	const auto was = !isHidden();
	RpWidget::setVisibleHook(visible && _wanted);
	if (!was && !isHidden()) {
		refresh();
	}
}

void FormatBar::paintEvent(QPaintEvent *e) {
	auto p = QPainter(this);
	const auto pill = rect() - _shadowMargins;
	if (pill.isEmpty()) {
		return;
	}
	Dialogs::PaintPillBackground(p, _shadow, pill, pill.height() / 2);
	if (_dividerX >= 0) {
		const auto margin = st::luminaFormatBarDividerMargin;
		p.fillRect(
			style::rtlrect(
				_dividerX,
				pill.y() + margin,
				st::lineWidth,
				pill.height() - 2 * margin,
				width()),
			st::menuSeparatorFg);
	}
}

// The pill's own surface (padding, divider) swallows clicks, so a near miss
// never reaches the host underneath (HistoryWidget handles presses on its
// own surface, the reply bar for one).
void FormatBar::mousePressEvent(QMouseEvent *e) {
	e->accept();
}

void FormatBar::mouseReleaseEvent(QMouseEvent *e) {
	e->accept();
}

void FormatBar::mouseDoubleClickEvent(QMouseEvent *e) {
	e->accept();
}

class BarController final : public base::has_weak_ptr {
public:
	explicit BarController(FormatBarHost &&host);
	~BarController();

	void hostMoved();
	void raise();
	void setReportedArea(QRect area);

private:
	void setEnabled(bool enabled);
	void startTracking();
	void update(bool immediate);
	void watch();
	void show();
	void hide();
	bool place();
	void schedulePlace();

	[[nodiscard]] bool shown() const;
	[[nodiscard]] bool wanted() const;
	[[nodiscard]] bool suppressed() const;
	[[nodiscard]] std::optional<int> selectionCenter() const;
	[[nodiscard]] QRect area() const;

	const not_null<Ui::InputField*> _field;
	const QPointer<Ui::RpWidget> _overlay;
	const Fn<int()> _composeTop;
	const Fn<bool()> _suppressed;
	const Fn<QRect()> _area;
	std::optional<QRect> _reportedArea;

	base::unique_qptr<FormatBar> _bar;
	base::Timer _showTimer;
	base::Timer _watchTimer;
	bool _enabled = false;
	bool _pressedInText = false;
	bool _placeScheduled = false;

	rpl::lifetime _tracking;
	rpl::lifetime _lifetime;

};

using BarRegistry = base::flat_map<const Ui::InputField*, BarController*>;

[[nodiscard]] BarRegistry &Registry() {
	static auto result = BarRegistry();
	return result;
}

BarController::BarController(FormatBarHost &&host)
: _field(host.field)
, _overlay(host.overlay.get())
, _composeTop(std::move(host.composeTop))
, _suppressed(std::move(host.suppressed))
, _area(std::move(host.area))
, _showTimer([=] { update(true); })
, _watchTimer([=] { watch(); }) {
	Registry().emplace(_field.get(), this);

	// The only thing alive while the feature is off. A backup restore
	// (Settings::importAll) and the panic wipe come through here as well.
	Settings::Instance().changesFor(
		SettingsKey()
	) | rpl::on_next([=] {
		setEnabled(FormatBarEnabled());
	}, _lifetime);
	setEnabled(FormatBarEnabled());
}

BarController::~BarController() {
	Registry().remove(_field.get());
}

void BarController::setEnabled(bool enabled) {
	if (_enabled == enabled) {
		return;
	}
	_enabled = enabled;
	if (enabled) {
		startTracking();
		update(false);
	} else {
		_tracking.destroy();
		_showTimer.cancel();
		_watchTimer.cancel();
		_pressedInText = false;
		_bar = nullptr;
	}
}

void BarController::startTracking() {
	const auto edit = _field->rawTextEdit();
	const auto changed = [=] { update(false); };

	rpl::merge(
		base::qt_signal_producer(
			edit.get(),
			&QTextEdit::selectionChanged),
		base::qt_signal_producer(
			edit.get(),
			&QTextEdit::cursorPositionChanged),
		_field->changes(),
		_field->focusedChanges() | rpl::to_empty,
		_field->menuShownValue() | rpl::skip(1) | rpl::to_empty,
		_field->shownValue() | rpl::skip(1) | rpl::to_empty,
		Ui::ScreenReaderModeActiveValue() | rpl::skip(1) | rpl::to_empty
	) | rpl::on_next(changed, _tracking);

	rpl::merge(
		_field->heightChanges(),
		_field->scrollTop().changes() | rpl::to_empty
	) | rpl::on_next([=] {
		hostMoved();
	}, _tracking);
	if (_overlay) {
		_overlay->sizeValue(
		) | rpl::skip(1) | rpl::on_next([=] {
			hostMoved();
		}, _tracking);
	}

	// Observe the presses on the text, never consume them: a press hides the
	// bar (a drag-select or a click into the text is starting), the release
	// brings it back once the selection has settled. Evaluated a turn later
	// because a click inside an existing selection only collapses it in the
	// text control's own release handling, after this filter ran.
	base::install_event_filter(edit->viewport(), [=](not_null<QEvent*> e) {
		const auto type = e->type();
		if (type == QEvent::MouseButtonPress
			|| type == QEvent::MouseButtonDblClick) {
			_pressedInText = true;
			update(false);
		} else if (type == QEvent::MouseButtonRelease) {
			crl::on_main(this, [=] {
				_pressedInText = false;
				update(true);
			});
		}
		return base::EventFilterResult::Continue;
	}, _tracking);
}

bool BarController::suppressed() const {
	return _suppressed && _suppressed();
}

bool BarController::wanted() const {
	if (!_enabled
		|| !_overlay
		|| !_overlay->isVisible()
		|| !_field->isVisible()
		|| !_field->hasFocus()
		|| _field->menuShown()
		|| _field->markdownEnabledState().disabled()
		|| Ui::ScreenReaderModeActive()) {
		return false;
	} else if (_pressedInText
		&& (QGuiApplication::mouseButtons() != Qt::NoButton)) {
		// Still selecting with the mouse. Asked of the application rather
		// than trusted from the flag alone: a release swallowed by a text
		// drag-and-drop must not keep the bar away for good. A press on the
		// bar's own buttons is not a press in the text and keeps it up.
		return false;
	}
	const auto cursor = _field->textCursor();
	if (!cursor.hasSelection()) {
		return false;
	}
	const auto text = cursor.selectedText();
	for (const auto &ch : text) {
		if (!ch.isSpace()) {
			return true;
		}
	}
	return false;
}

bool BarController::shown() const {
	return _bar && !_bar->isHidden();
}

void BarController::update(bool immediate) {
	if (!_enabled) {
		return;
	} else if (!wanted()) {
		_showTimer.cancel();
		_watchTimer.cancel();
		hide();
		return;
	}
	if (!_watchTimer.isActive()) {
		_watchTimer.callEach(kWatchInterval);
	}
	if (suppressed()) {
		_showTimer.cancel();
		hide();
	} else if (shown()) {
		_bar->refresh();
		if (!place()) {
			hide();
		}
	} else if (immediate) {
		_showTimer.cancel();
		show();
	} else {
		_showTimer.callOnce(kKeyboardShowDelay);
	}
}

void BarController::watch() {
	if (!wanted()) {
		update(false);
	} else if (suppressed()) {
		hide();
	} else if (shown()) {
		if (!place()) {
			hide();
		}
	} else if (!_showTimer.isActive()) {
		// What hid it has gone away (a panel closed): come back.
		show();
	}
}

void BarController::show() {
	if (!_overlay) {
		return;
	}
	if (!_bar) {
		_bar = base::make_unique_q<FormatBar>(_overlay.data(), _field);
	}
	_bar->refresh();
	if (!place()) {
		hide();
		return;
	}
	_bar->setWanted(true);
	_bar->raise();
}

void BarController::hide() {
	if (_bar) {
		_bar->setWanted(false);
	}
}

void BarController::hostMoved() {
	if (!shown()) {
		return;
	} else if (!place()) {
		hide();
		return;
	}
	// A host usually moves the field first and resizes what is above it
	// (HistoryWidget's message list, a section's compose wrap) right after,
	// in the same pass; look again once that pass is over.
	schedulePlace();
}

void BarController::schedulePlace() {
	if (_placeScheduled) {
		return;
	}
	_placeScheduled = true;
	crl::on_main(this, [=] {
		_placeScheduled = false;
		if (shown() && !place()) {
			hide();
		}
	});
}

void BarController::raise() {
	if (_bar) {
		_bar->raise();
	}
}

void BarController::setReportedArea(QRect area) {
	if (_reportedArea == area) {
		return;
	}
	_reportedArea = area;
	hostMoved();
}

// Where the pill may go, in the overlay's coordinates: the host's message
// list viewport when it told us, else the whole overlay.
QRect BarController::area() const {
	if (!_overlay) {
		return QRect();
	}
	const auto full = _overlay->rect();
	if (_area) {
		return _area().intersected(full);
	} else if (_reportedArea) {
		return _reportedArea->intersected(full);
	}
	return full;
}

// The horizontal middle of the first visible line of the selection, in the
// overlay's coordinates.
std::optional<int> BarController::selectionCenter() const {
	const auto edit = _field->rawTextEdit();
	const auto viewport = edit->viewport();
	if (!_overlay || !_overlay->isAncestorOf(viewport)) {
		return std::nullopt;
	}
	const auto cursor = _field->textCursor();
	const auto start = cursor.selectionStart();
	const auto end = cursor.selectionEnd();
	if (start >= end) {
		return std::nullopt;
	}
	auto first = cursor;
	first.setPosition(start);
	if (edit->cursorRect(first).bottom() < 0) {
		// The selection starts above the visible part of the field.
		first = edit->cursorForPosition(QPoint(0, 0));
		first.movePosition(QTextCursor::StartOfLine);
		if (first.position() < start) {
			first.setPosition(start);
		}
	}
	auto last = first;
	last.movePosition(QTextCursor::EndOfLine);
	if (last.position() > end) {
		last.setPosition(end);
	}
	const auto visible = viewport->width();
	const auto a = std::clamp(edit->cursorRect(first).x(), 0, visible);
	const auto b = std::clamp(edit->cursorRect(last).x(), 0, visible);
	return viewport->mapTo(_overlay.data(), QPoint((a + b) / 2, 0)).x();
}

bool BarController::place() {
	if (!_bar || !_overlay) {
		return false;
	}
	const auto center = selectionCenter();
	if (!center) {
		return false;
	}
	const auto bounds = area();
	if (bounds.isEmpty()) {
		return false;
	}
	const auto margins = _bar->shadowMargins();
	const auto pillWidth = _bar->width() - margins.left() - margins.right();
	const auto pillHeight = _bar->height() - margins.top() - margins.bottom();
	const auto fieldTop = _field->mapTo(_overlay.data(), QPoint()).y();
	const auto stackTop = std::min(
		_composeTop ? std::min(_composeTop(), fieldTop) : fieldTop,
		bounds.y() + bounds.height());
	const auto pillTop = stackTop - st::luminaFormatBarOffset - pillHeight;
	if (pillTop < bounds.y()) {
		return false;
	}
	// Inside the area with a small skip from its edges; centred in it when
	// the skips do not fit; centred on the whole overlay only when not even
	// the pill fits (a window narrower than any real layout).
	const auto skip = st::luminaFormatBarSkip;
	const auto minLeft = bounds.x() + skip;
	const auto maxLeft = bounds.x() + bounds.width() - skip - pillWidth;
	const auto pillLeft = (maxLeft >= minLeft)
		? std::clamp(*center - pillWidth / 2, minLeft, maxLeft)
		: (bounds.width() >= pillWidth)
		? (bounds.x() + (bounds.width() - pillWidth) / 2)
		: ((_overlay->width() - pillWidth) / 2);
	_bar->move(pillLeft - margins.left(), pillTop - margins.top());
	return true;
}

[[nodiscard]] BarController *Find(not_null<const Ui::InputField*> field) {
	const auto &registry = Registry();
	const auto i = registry.find(field.get());
	return (i != registry.end()) ? i->second : nullptr;
}

} // namespace

void AttachFormatBar(FormatBarHost host) {
	const auto field = host.field;
	if (Find(field)) {
		return;
	}
	field->lifetime().make_state<BarController>(std::move(host));
}

void MoveFormatBar(not_null<Ui::InputField*> field) {
	if (const auto controller = Find(field)) {
		controller->hostMoved();
	}
}

void RaiseFormatBar(not_null<Ui::InputField*> field) {
	if (const auto controller = Find(field)) {
		controller->raise();
	}
}

void SetFormatBarArea(not_null<Ui::InputField*> field, QRect area) {
	if (const auto controller = Find(field)) {
		controller->setReportedArea(area);
	}
}

bool FormatBarEnabled() {
	return Settings::Instance().getBool(SettingsKey(), false);
}

} // namespace Lumina
