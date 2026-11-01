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

// Master switch seam; the SexCat settings section binds SetFeatureEnabled.
void SetFeatureEnabled(bool enabled);
[[nodiscard]] bool FeatureEnabled();

// True only on a cache hit, so the badge appears progressively.
[[nodiscard]] bool ShouldShowBadge(PeerId peerId, MsgId msgId);

// Most recent known save path for the message, empty when unknown.
[[nodiscard]] QString LastSavedPath(PeerId peerId, MsgId msgId);

// Async cache fill via Database::LookupBySource; `alive` tracks pending
// bookkeeping, `refresh` repaints the calling bubble when the answer lands.
void Query(
	PeerId peerId,
	MsgId msgId,
	rpl::lifetime &alive,
	Fn<void()> refresh = nullptr);

// Process-scoped lifetime for callers without one of their own.
[[nodiscard]] rpl::lifetime &Lifetime();

// Returns true when the hover state actually changed (caller repaints).
bool SetHovered(PeerId peerId, MsgId msgId, bool hovered);
[[nodiscard]] bool IsHovered(PeerId peerId, MsgId msgId);

// Top-right inner corner of the given media area.
[[nodiscard]] QRect BadgeRect(const QRect &mediaRect);

// Draws the badge; fires Query on a cache miss, never reserves space.
void PaintBadge(
	Painter &p,
	PeerId peerId,
	MsgId msgId,
	const QRect &mediaRect,
	bool selected);

// "Open folder / view history / re-verify" for a message with a badge.
void AddContextMenuActions(
	not_null<Ui::PopupMenu*> menu,
	not_null<HistoryItem*> item);

} // namespace SexCat::Badge
