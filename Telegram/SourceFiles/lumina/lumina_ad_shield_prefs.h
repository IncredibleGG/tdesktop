/*
This file is part of LuminaGram,
a fork of Telegram Desktop.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "lumina/lumina_ad_shield_store.h"

#include <rpl/producer.h>

#include <QtCore/QString>

namespace Lumina::AdShield {

// "Hide repeated ads": the user's choices, kept in Lumina::Settings.
//
// Only these small preferences live in the preference store. The AdShield
// data itself (hashes, ids, dates) never does: it is in its own directory
// under tdata (lumina_ad_shield_manager.h), so it is not in a Lumina backup
// and not in any synced settings.
//
// Keys, all Store::Prefs except the per-group modes:
//   adShieldEnabled          bool, default false - the global switch
//   adShieldIntroAccepted    bool, default false - the first-enable
//                            explainer was confirmed with "Turn on"
//   adShieldSensitivity      "loose" | "standard" | "aggressive",
//                            default "standard"
//   adShieldExemptContacts   bool, default true
//   adShieldExemptBots       bool, default true
//   adShieldExemptReplies    bool, default true
//   adShieldTrial            bool, default false - mark, do not fold
//   adShieldChatModes        object { "<sessionUniqueId>_<peerId>": "on" |
//                            "off" }, Store::Private - a list of groups
//                            is more personal than an on/off switch. A
//                            group that follows the global switch has no
//                            entry. Written only by SetChatMode(),
//                            ClearChatModesFor() and ClearChatModes().
//
// The panic wipe empties all of them with every other Lumina preference;
// "Clear local data" removes adShieldChatModes (ClearChatModes()), and
// logging out removes the account's entries (ClearChatModesFor()).
//
// Main thread only, like Lumina::Settings.

enum class ChatMode {
	Default,
	On,
	Off,
};

[[nodiscard]] bool GlobalEnabled();
void SetGlobalEnabled(bool enabled);

[[nodiscard]] bool IntroAccepted();
void SetIntroAccepted(bool accepted);

[[nodiscard]] Sensitivity CurrentSensitivity();
void SetSensitivity(Sensitivity value);

[[nodiscard]] bool ExemptContacts();
void SetExemptContacts(bool value);
[[nodiscard]] bool ExemptBots();
void SetExemptBots(bool value);
[[nodiscard]] bool ExemptReplies();
void SetExemptReplies(bool value);

[[nodiscard]] bool TrialMode();
void SetTrialMode(bool value);

// The engine settings for decide(). dryRun mirrors the trial switch; it does
// not change any Decision (SPEC 6.10), only what the chat list shows.
[[nodiscard]] Settings CurrentSettings();

[[nodiscard]] ChatMode ChatModeFor(quint64 sessionUniqueId, quint64 peerId);
void SetChatMode(quint64 sessionUniqueId, quint64 peerId, ChatMode mode);

// Whether any group of this account is set to "on in this group".
[[nodiscard]] bool AnyChatOn(quint64 sessionUniqueId);

// Whether any group of any account is set to "on in this group".
[[nodiscard]] bool AnyChatOnAnywhere();

// Every per-group mode of every account goes back to "follow the default".
void ClearChatModes();

// The same for one account only (its logout).
void ClearChatModesFor(quint64 sessionUniqueId);

// The global switch or a per-group mode changed: what is observed may have
// changed.
[[nodiscard]] rpl::producer<> ActivationChanges();

// Sensitivity or an exemption changed: decisions have to be recomputed.
[[nodiscard]] rpl::producer<> DecisionSettingsChanges();

// The trial switch changed: only the display changes.
[[nodiscard]] rpl::producer<> TrialChanges();

// Any of the above, for settings rows.
[[nodiscard]] rpl::producer<> PrefsChanges();

} // namespace Lumina::AdShield
