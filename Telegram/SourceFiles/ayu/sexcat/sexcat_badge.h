// This is the source code of AyuGram for Desktop.
//
// We do not and cannot prevent the use of our code,
// but be respectful and credit the original author.
//
// Copyright @Radolyn, 2026
#pragma once

#include "base/basic_types.h"
#include "data/data_peer_id.h"
#include "rpl/lifetime.h"

#include <QtCore/QRect>
#include <QtCore/QString>

class HistoryItem;
class Painter;

namespace Ui {
class PopupMenu;
} // namespace Ui

namespace SexCat::Badge {

[[nodiscard]] bool FeatureEnabled();

// True only on a cache hit, so the badge appears progressively.
[[nodiscard]] bool ShouldShowBadge(PeerId peerId, MsgId msgId);

[[nodiscard]] QString LastSavedPath(PeerId peerId, MsgId msgId);

void Query(
	PeerId peerId,
	MsgId msgId,
	rpl::lifetime &alive,
	Fn<void()> refresh = nullptr);

[[nodiscard]] rpl::lifetime &Lifetime();

// Returns true when the hover state actually changed (caller repaints).
bool SetHovered(PeerId peerId, MsgId msgId, bool hovered);
[[nodiscard]] bool IsHovered(PeerId peerId, MsgId msgId);

[[nodiscard]] QRect BadgeRect(const QRect &mediaRect);

void PaintBadge(
	Painter &p,
	PeerId peerId,
	MsgId msgId,
	const QRect &mediaRect,
	bool selected,
	Fn<void()> refresh = nullptr);

void AddContextMenuActions(
	not_null<Ui::PopupMenu*> menu,
	not_null<HistoryItem*> item);

} // namespace SexCat::Badge
