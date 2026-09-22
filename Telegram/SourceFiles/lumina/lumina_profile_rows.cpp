/*
This file is part of LuminaGram,
a fork of Telegram Desktop.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "lumina/lumina_profile_rows.h"

// UserData has to be complete here, not just forward declared: passing a
// not_null<UserData*> where a not_null<PeerData*> is expected goes through
// gsl::not_null's converting constructor, and its std::is_convertible check
// needs the derived-to-base relationship to be visible.
#include "data/data_user.h"
#include "boxes/peers/edit_peer_info_box.h"
#include "data/data_changes.h"
#include "data/data_channel.h"
#include "data/data_peer.h"
#include "data/data_session.h"
#include "history/admin_log/history_admin_log_section.h"
#include "lumina/lumina_locale.h"
#include "main/main_session.h"
#include "window/window_session_controller.h"
#include "styles/style_menu_icons.h"

#include <memory>
#include "lumina/lumina_contact_notes.h"
#include "lumina/lumina_profile_chat_date.h"
#include "lumina/lumina_profile_chat_id.h"
#include "lumina/lumina_profile_dc_id.h"
#include "lumina/lumina_profile_name_warning.h"
#include "lumina/lumina_profile_user_id.h"
#include "lumina/lumina_registration_date.h"

namespace Lumina {
namespace {

// The two clickable shortcuts mirror Swiftgram's group/channel profile: a row
// into the manage/edit screen and a row into the admin event log. Unlike the
// other Lumina rows these react to a tap, so they go through
// context.addClickableRow (a MultiSlideTracker-tracked settings button), not
// addInfoOneLine.
//
// `shown` is rebuilt from peerFlagsValue(Rights | Admins) so a change of role
// shows or hides the row without reopening the profile. The click handlers
// capture context.controller (which outlives the section - see the header) and
// the peer/channel pointer; they never touch makeInfo()'s locals.

// "Group Settings" - opens the group/channel manage & edit screen, shown only
// when that screen is reachable for the current viewer.
void AddGroupSettingsRow(
		const ProfileRowsContext &context,
		not_null<PeerData*> peer,
		Data::ForumTopic *topic) {
	if (topic || !context.addClickableRow) {
		return;
	}
	// Groups and channels only (this branch is never a user/bot profile, but be
	// explicit); EditPeerInfoBox::Available() is the same gate the top-bar
	// "Manage" action uses.
	if (!peer->isChat() && !peer->isChannel()) {
		return;
	}
	const auto controller = context.controller;
	auto shown = peer->session().changes().peerFlagsValue(
		peer,
		Data::PeerUpdate::Flag::Rights
			| Data::PeerUpdate::Flag::Admins
	) | rpl::map([=] {
		return EditPeerInfoBox::Available(peer);
	});
	context.addClickableRow(
		TrValue(u"LuminaProfileGroupSettings"_q),
		std::move(shown),
		[controller, peer] {
			controller->showEditPeerBox(peer);
		},
		&st::menuIconManage);
}

// "Recent Actions" - opens the admin event log. Channels/supergroups only, and
// only for admins or the creator (matches EditPeerInfoBox's hasRecentActions).
void AddRecentActionsRow(
		const ProfileRowsContext &context,
		not_null<PeerData*> peer,
		Data::ForumTopic *topic) {
	if (topic || !context.addClickableRow) {
		return;
	}
	const auto channel = peer->asChannel();
	if (!channel) {
		return;
	}
	const auto controller = context.controller;
	auto shown = channel->session().changes().peerFlagsValue(
		channel,
		Data::PeerUpdate::Flag::Rights
			| Data::PeerUpdate::Flag::Admins
	) | rpl::map([=] {
		return channel->hasAdminRights() || channel->amCreator();
	});
	context.addClickableRow(
		TrValue(u"LuminaProfileRecentActions"_q),
		std::move(shown),
		[controller, channel] {
			controller->showSection(
				std::make_shared<AdminLog::SectionMemento>(channel));
		},
		&st::menuIconGroupLog);
}

} // namespace

void AddUserInfoRows(
		const ProfileRowsContext &context,
		not_null<UserData*> user) {
	AddNameWarningRow(context, user);
	AddRegistrationDateRow(context, user);
	AddProfileDcIdRow(context, user);
	AddUserIdRow(context, user);
	AddContactNoteRow(context, user);
}

void AddChatInfoRows(
		const ProfileRowsContext &context,
		not_null<PeerData*> peer,
		Data::ForumTopic *topic) {
	AddChatCreationDateRow(context, peer, topic);
	AddProfileDcIdRow(context, peer, topic);
	AddChatIdRow(context, peer, topic);
	AddGroupSettingsRow(context, peer, topic);
	AddRecentActionsRow(context, peer, topic);
}

} // namespace Lumina
