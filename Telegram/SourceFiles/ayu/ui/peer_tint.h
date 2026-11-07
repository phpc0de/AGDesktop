// This is the source code of AyuGram for Desktop.
//
// We do not and cannot prevent the use of our code,
// but be respectful and credit the original author.
//
// Copyright @Radolyn, 2026
#pragma once

#include "ui/style/style_core_color.h"

class PeerData;

namespace Ayu {

enum class PeerTint {
	None,
	Protected,
	Banned,
	AyuRestricted,
	UserPeerNoForwards,
	UserSelfNoForwards,
};

[[nodiscard]] PeerTint PeerTintOf(PeerData *peer);
[[nodiscard]] const style::color &PeerTintFg(PeerTint tint);

} // namespace Ayu
