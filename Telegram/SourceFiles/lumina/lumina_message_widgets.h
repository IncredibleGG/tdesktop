/*
This file is part of LuminaGram,
a fork of Telegram Desktop.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include <rpl/producer.h>

#include <memory>

class HistoryItem;

namespace HistoryView {
class Element;
class Media;
} // namespace HistoryView

namespace Ui {
class VerticalLayout;
} // namespace Ui

namespace Window {
class SessionController;
} // namespace Window

namespace Lumina {

// A switch for upstream 7.4's bot HTML widgets (tgw/): messages whose media
// is an interactive page the bot wrote, laid out with litehtml and scripted
// by untrusted JavaScript in a QuickJS sandbox, inside the chat.
//
// Default: on, exactly as upstream ships it. Turned off, the hook in
// Tgw::WidgetMedia::createView() (tgw/tgw_media.cpp) builds a notice card in
// place of HistoryView::HtmlWidget, so no widget host or runtime is created
// for the message and nothing of the widget runs or loads. The caption, the
// reply markup and everything else about the message are left alone, and
// forwarding, copying or deleting it work as before. Turning the switch back
// on rebuilds the widgets that were laid out in this run, in place.
//
// Preference `messageWidgetsOff`, Store::Prefs, default false.
[[nodiscard]] bool MessageWidgetsDisabled();
void SetMessageWidgetsDisabled(bool disabled);
[[nodiscard]] rpl::producer<> MessageWidgetsChanges();

// Called by the createView() hook for every widget message it lays out, so a
// later toggle can ask exactly those messages to rebuild their views.
void RememberMessageWidget(not_null<HistoryItem*> item);

// The card shown instead of a widget while they are turned off: a title, one
// line about why and a button to the setting.
[[nodiscard]] std::unique_ptr<HistoryView::Media> CreateMessageWidgetOffView(
	not_null<HistoryView::Element*> parent);

// The settings row (LuminaGram > Privacy).
void AddMessageWidgetRows(
	not_null<Ui::VerticalLayout*> container,
	not_null<Window::SessionController*> controller);

} // namespace Lumina
