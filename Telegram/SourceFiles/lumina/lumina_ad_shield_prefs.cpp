/*
This file is part of LuminaGram,
a fork of Telegram Desktop.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "lumina/lumina_ad_shield_prefs.h"

#include "lumina/lumina_settings.h"

#include <QtCore/QJsonObject>
#include <QtCore/QJsonValue>

namespace Lumina::AdShield {
namespace {

// Lumina::AdShield has its own Settings (the engine struct) and Store (the
// engine class), so the preference store is always spelled out in full here.
using Prefs = Lumina::Settings;
using PrefsFile = Lumina::Store;

[[nodiscard]] QString EnabledKey() {
	return u"adShieldEnabled"_q;
}

[[nodiscard]] QString IntroKey() {
	return u"adShieldIntroAccepted"_q;
}

[[nodiscard]] QString SensitivityKey() {
	return u"adShieldSensitivity"_q;
}

[[nodiscard]] QString ExemptContactsKey() {
	return u"adShieldExemptContacts"_q;
}

[[nodiscard]] QString ExemptBotsKey() {
	return u"adShieldExemptBots"_q;
}

[[nodiscard]] QString ExemptRepliesKey() {
	return u"adShieldExemptReplies"_q;
}

[[nodiscard]] QString TrialKey() {
	return u"adShieldTrial"_q;
}

[[nodiscard]] QString ChatModesKey() {
	return u"adShieldChatModes"_q;
}

[[nodiscard]] QString ModeOn() {
	return u"on"_q;
}

[[nodiscard]] QString ModeOff() {
	return u"off"_q;
}

[[nodiscard]] QString ChatKey(quint64 sessionUniqueId, quint64 peerId) {
	return QString::number(sessionUniqueId)
		+ QChar('_')
		+ QString::number(peerId);
}

[[nodiscard]] QString SessionPrefix(quint64 sessionUniqueId) {
	return QString::number(sessionUniqueId) + QChar('_');
}

[[nodiscard]] rpl::producer<> KeysChanges(std::vector<QString> keys) {
	return Prefs::Instance().changes(
	) | rpl::filter([keys = std::move(keys)](const QString &key) {
		return ranges::contains(keys, key);
	}) | rpl::to_empty;
}

} // namespace

bool GlobalEnabled() {
	return Prefs::Instance().getBool(EnabledKey(), false);
}

void SetGlobalEnabled(bool enabled) {
	Prefs::Instance().set(EnabledKey(), enabled, PrefsFile::Prefs);
}

bool IntroAccepted() {
	return Prefs::Instance().getBool(IntroKey(), false);
}

void SetIntroAccepted(bool accepted) {
	Prefs::Instance().set(IntroKey(), accepted, PrefsFile::Prefs);
}

Sensitivity CurrentSensitivity() {
	const auto value = Prefs::Instance().getString(SensitivityKey());
	if (value == u"loose"_q) {
		return Sensitivity::Loose;
	} else if (value == u"aggressive"_q) {
		return Sensitivity::Aggressive;
	}
	return Sensitivity::Standard;
}

void SetSensitivity(Sensitivity value) {
	const auto text = [&] {
		switch (value) {
		case Sensitivity::Loose: return u"loose"_q;
		case Sensitivity::Standard: return u"standard"_q;
		case Sensitivity::Aggressive: return u"aggressive"_q;
		}
		return u"standard"_q;
	}();
	Prefs::Instance().set(SensitivityKey(), text, PrefsFile::Prefs);
}

bool ExemptContacts() {
	return Prefs::Instance().getBool(ExemptContactsKey(), true);
}

void SetExemptContacts(bool value) {
	Prefs::Instance().set(ExemptContactsKey(), value, PrefsFile::Prefs);
}

bool ExemptBots() {
	return Prefs::Instance().getBool(ExemptBotsKey(), true);
}

void SetExemptBots(bool value) {
	Prefs::Instance().set(ExemptBotsKey(), value, PrefsFile::Prefs);
}

bool ExemptReplies() {
	return Prefs::Instance().getBool(ExemptRepliesKey(), true);
}

void SetExemptReplies(bool value) {
	Prefs::Instance().set(ExemptRepliesKey(), value, PrefsFile::Prefs);
}

bool TrialMode() {
	return Prefs::Instance().getBool(TrialKey(), false);
}

void SetTrialMode(bool value) {
	Prefs::Instance().set(TrialKey(), value, PrefsFile::Prefs);
}

Settings CurrentSettings() {
	auto result = Settings();
	result.sensitivity = CurrentSensitivity();
	result.exemptContacts = ExemptContacts();
	result.exemptBots = ExemptBots();
	result.exemptReplies = ExemptReplies();
	result.dryRun = TrialMode();
	return result;
}

ChatMode ChatModeFor(quint64 sessionUniqueId, quint64 peerId) {
	if (!sessionUniqueId || !peerId) {
		return ChatMode::Default;
	}
	const auto value = Prefs::Instance().getObject(
		ChatModesKey()
	).value(ChatKey(sessionUniqueId, peerId)).toString();
	if (value == ModeOn()) {
		return ChatMode::On;
	} else if (value == ModeOff()) {
		return ChatMode::Off;
	}
	return ChatMode::Default;
}

void SetChatMode(quint64 sessionUniqueId, quint64 peerId, ChatMode mode) {
	if (!sessionUniqueId || !peerId) {
		return;
	}
	const auto key = ChatKey(sessionUniqueId, peerId);
	auto object = Prefs::Instance().getObject(ChatModesKey());
	switch (mode) {
	case ChatMode::Default: object.remove(key); break;
	case ChatMode::On: object.insert(key, ModeOn()); break;
	case ChatMode::Off: object.insert(key, ModeOff()); break;
	}
	if (object.isEmpty()) {
		Prefs::Instance().remove(ChatModesKey());
	} else {
		Prefs::Instance().set(ChatModesKey(), object, PrefsFile::Private);
	}
}

bool AnyChatOn(quint64 sessionUniqueId) {
	const auto prefix = SessionPrefix(sessionUniqueId);
	const auto object = Prefs::Instance().getObject(ChatModesKey());
	for (auto i = object.constBegin(); i != object.constEnd(); ++i) {
		if (i.key().startsWith(prefix) && i.value().toString() == ModeOn()) {
			return true;
		}
	}
	return false;
}

bool AnyChatOnAnywhere() {
	const auto object = Prefs::Instance().getObject(ChatModesKey());
	for (auto i = object.constBegin(); i != object.constEnd(); ++i) {
		if (i.value().toString() == ModeOn()) {
			return true;
		}
	}
	return false;
}

void ClearChatModes() {
	Prefs::Instance().remove(ChatModesKey());
}

void ClearChatModesFor(quint64 sessionUniqueId) {
	if (!sessionUniqueId) {
		return;
	}
	const auto prefix = SessionPrefix(sessionUniqueId);
	auto object = Prefs::Instance().getObject(ChatModesKey());
	auto changed = false;
	for (auto i = object.begin(); i != object.end();) {
		if (i.key().startsWith(prefix)) {
			i = object.erase(i);
			changed = true;
		} else {
			++i;
		}
	}
	if (!changed) {
		return;
	} else if (object.isEmpty()) {
		Prefs::Instance().remove(ChatModesKey());
	} else {
		Prefs::Instance().set(ChatModesKey(), object, PrefsFile::Private);
	}
}

rpl::producer<> ActivationChanges() {
	return KeysChanges({ EnabledKey(), ChatModesKey() });
}

rpl::producer<> DecisionSettingsChanges() {
	return KeysChanges({
		SensitivityKey(),
		ExemptContactsKey(),
		ExemptBotsKey(),
		ExemptRepliesKey(),
	});
}

rpl::producer<> TrialChanges() {
	return KeysChanges({ TrialKey() });
}

rpl::producer<> PrefsChanges() {
	return KeysChanges({
		EnabledKey(),
		IntroKey(),
		SensitivityKey(),
		ExemptContactsKey(),
		ExemptBotsKey(),
		ExemptRepliesKey(),
		TrialKey(),
		ChatModesKey(),
	});
}

} // namespace Lumina::AdShield
