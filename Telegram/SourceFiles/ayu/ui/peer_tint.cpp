// This is the source code of AyuGram for Desktop.
//
// We do not and cannot prevent the use of our code,
// but be respectful and credit the original author.
//
// Copyright @Radolyn, 2026
#include "ayu/ui/peer_tint.h"

#include "ayu/ayu_settings.h"
#include "data/data_channel.h"
#include "data/data_chat.h"
#include "data/data_peer.h"
#include "data/data_user.h"
#include "styles/style_dialogs.h"
#include "styles/style_info.h"

namespace Ayu {

PeerTint PeerTintOf(PeerData *peer) {
	if (!peer) {
		return PeerTint::None;
	}
	if (const auto channel = peer->asChannel()) {
		if (AyuSettings::getInstance().isSpecialRestrictedPeer(
			channel->id.value)) {
			return PeerTint::Banned;
		} else if (channel->flags() & ChannelDataFlag::AyuForbidden) {
			return PeerTint::Banned;
		} else if (channel->isForbidden()) {
			// WHY: the channelForbidden apply path zeroes date on purpose,
			// so guarding on it made the tint unreachable for exactly the
			// region-blocked channels this feature is meant to mark.
			return PeerTint::Banned;
		} else if (channel->flags() & ChannelDataFlag::AyuRestrictedContent) {
			return PeerTint::RegionRestricted;
		} else if (channel->flags() & ChannelDataFlag::NoForwards) {
			return PeerTint::Protected;
		} else if (channel->isAyuNoForwards()) {
			return PeerTint::AyuRestricted;
		}
	} else if (const auto chat = peer->asChat()) {
		if (chat->flags() & ChatDataFlag::AyuForbidden) {
			return PeerTint::Banned;
		} else if (chat->flags() & ChatDataFlag::NoForwards) {
			return PeerTint::Protected;
		} else if (chat->isAyuNoForwards()) {
			return PeerTint::AyuRestricted;
		}
	} else if (const auto user = peer->asUser()) {
		if (user->flags() & UserDataFlag::NoForwardsPeerEnabled) {
			return PeerTint::UserPeerNoForwards;
		} else if (user->flags() & UserDataFlag::NoForwardsMyEnabled) {
			return PeerTint::UserSelfNoForwards;
		}
	}
	return PeerTint::None;
}

const style::color &PeerTintFg(PeerTint tint) {
	switch (tint) {
	case PeerTint::None: break;
	case PeerTint::Banned: return st::topBarTitleAyuBanned.textFg;
	case PeerTint::RegionRestricted:
		return st::topBarTitleAyuRegionBlocked.textFg;
	case PeerTint::Protected:
		return st::topBarTitleAyuProtected.textFg;
	case PeerTint::AyuRestricted:
		return st::topBarTitleAyuRestricted.textFg;
	case PeerTint::UserPeerNoForwards:
		return st::topBarTitleAyuUserPeer.textFg;
	case PeerTint::UserSelfNoForwards:
		return st::topBarTitleAyuUserSelf.textFg;
	}
	return st::dialogsNameFg;
}

} // namespace Ayu
