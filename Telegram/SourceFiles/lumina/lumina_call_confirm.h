/*
This file is part of LuminaGram,
a fork of Telegram Desktop.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include <rpl/producer.h>

namespace Lumina {

// Batch 4, #16. Ask for confirmation before a one-on-one voice or video call is
// actually placed, so a mis-tap on a call button does not start ringing the
// other side. Off by default, in which case Calls::Instance::startOutgoingCall()
// behaves exactly as stock Telegram Desktop's does.
//
// This is distinct from the "confirm before sending a voice / video MESSAGE"
// feature (lumina/lumina_voice_confirm.h): that one guards the recorder, this
// one guards the call.
//
// Only the 1-on-1 outgoing-call choke point is gated; group / conference calls
// are unaffected.
//
// Key `confirmStartCall`, Store::Prefs, default false.
[[nodiscard]] bool ConfirmCallStart();
[[nodiscard]] rpl::producer<bool> ConfirmCallStartValue();
void SetConfirmCallStart(bool value);

} // namespace Lumina
