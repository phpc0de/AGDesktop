// This is the source code of AyuGram for Desktop.
//
// We do not and cannot prevent the use of our code,
// but be respectful and credit the original author.
//
// Copyright @Radolyn, 2026
#include "ayu/ayu_settings.h"
#include "ayu/sexcat/sexcat_badge.h"

#include "ayu/sexcat/sexcat_database.h"
#include "ayu/sexcat/sexcat_history_window.h"
#include "crl/crl_async.h"
#include "crl/crl_on_main.h"
#include "data/data_msg_id.h"
#include "history/history_item.h"
#include "lang/lang_keys.h"
#include "rpl/rpl.h"
#include "ui/painter.h"
#include "ui/rect.h"
#include "ui/widgets/popup_menu.h"
#include "styles/style_menu_icons.h"
#include "styles/style_sexcat.h"

#include <QtGui/QDesktopServices>
#include <QtCore/QFileInfo>
#include <QtCore/QUrl>

#include <list>
#include <optional>
#include <unordered_map>
#include <unordered_set>

namespace SexCat::Badge {
namespace {

constexpr auto kBadgeCacheSize = 512;

struct BadgeKey {
	uint64 peer = 0;
	int64 msg = 0;

	friend inline bool operator==(BadgeKey a, BadgeKey b) {
		return (a.peer == b.peer) && (a.msg == b.msg);
	}
};

struct BadgeKeyHash {
	[[nodiscard]] std::size_t operator()(BadgeKey key) const {
		const auto h1 = std::hash<uint64>()(key.peer);
		const auto h2 = std::hash<int64>()(key.msg);
		return h1 ^ (h2 + 0x9e3779b97f4a7c15ULL + (h1 << 6) + (h1 >> 2));
	}
};

struct BadgeEntry {
	bool saved = false;
	QString lastPath;
};

using BadgeList = std::list<std::pair<BadgeKey, BadgeEntry>>;

BadgeList &Cache() {
	static BadgeList result;
	return result;
}

std::unordered_map<BadgeKey, BadgeList::iterator, BadgeKeyHash> &CacheIndex() {
	static std::unordered_map<BadgeKey, BadgeList::iterator, BadgeKeyHash> result;
	return result;
}

std::unordered_set<BadgeKey, BadgeKeyHash> &Pending() {
	static std::unordered_set<BadgeKey, BadgeKeyHash> result;
	return result;
}

std::optional<BadgeKey> &Hovered() {
	static std::optional<BadgeKey> result;
	return result;
}

bool gFeatureEnabled = true;

[[nodiscard]] BadgeKey MakeKey(PeerId peerId, MsgId msgId) {
	return { peerId.value, msgId.bare };
}

void Promote(BadgeList::iterator it) {
	if (it != Cache().begin()) {
		Cache().splice(Cache().begin(), Cache(), it);
	}
}

void TrimCache() {
	auto &list = Cache();
	auto &index = CacheIndex();
	while (int(list.size()) > kBadgeCacheSize) {
		index.erase(list.back().first);
		list.pop_back();
	}
}

void StoreEntry(PeerId peerId, MsgId msgId, BadgeEntry entry) {
	auto &list = Cache();
	auto &index = CacheIndex();
	const auto key = MakeKey(peerId, msgId);
	const auto it = index.find(key);
	if (it != index.end()) {
		it->second->second = entry;
		Promote(it->second);
	} else {
		list.emplace_front(key, entry);
		index.emplace(key, list.begin());
	}
	TrimCache();
}

void ApplyLookup(
		PeerId peerId,
		MsgId msgId,
		std::vector<Entry> records) {
	Pending().erase(MakeKey(peerId, msgId));
	auto best = BadgeEntry();
	auto bestAt = int64(0);
	for (const auto &record : records) {
		if (record.saved_at >= bestAt) {
			bestAt = record.saved_at;
			best.saved = true;
			best.lastPath = record.saved_path;
		}
	}
	StoreEntry(peerId, msgId, best);
}

void EnsureSubscribed() {
	static bool subscribed = false;
	static rpl::lifetime lifetime;
	if (subscribed) {
		return;
	}
	subscribed = true;
	Database::instance().newSaved(
	) | rpl::on_next([=](const Entry &record) {
		const auto key = BadgeKey{
			uint64(record.src_peer_id),
			record.src_msg_id,
		};
		Pending().erase(key);
		StoreEntry(
			PeerId(uint64(record.src_peer_id)),
			MsgId(record.src_msg_id),
			BadgeEntry{ true, record.saved_path });
	}, lifetime);
}

} // namespace

void SetFeatureEnabled(bool enabled) {
	gFeatureEnabled = enabled;
}

bool FeatureEnabled() {
	return gFeatureEnabled
		&& AyuSettings::getInstance().sexcatEnabled()
		&& Database::instance().Enabled();
}

bool ShouldShowBadge(PeerId peerId, MsgId msgId) {
	if (!FeatureEnabled()) {
		return false;
	}
	const auto &index = CacheIndex();
	const auto it = index.find(MakeKey(peerId, msgId));
	return (it != index.end()) && it->second->second.saved;
}

QString LastSavedPath(PeerId peerId, MsgId msgId) {
	const auto &index = CacheIndex();
	const auto it = index.find(MakeKey(peerId, msgId));
	return ((it != index.end()) && it->second->second.saved)
		? it->second->second.lastPath
		: QString();
}

rpl::lifetime &Lifetime() {
	static rpl::lifetime result;
	return result;
}

void Query(PeerId peerId, MsgId msgId, rpl::lifetime &alive, Fn<void()> refresh) {
	if (!FeatureEnabled()) {
		return;
	}
	EnsureSubscribed();
	const auto key = MakeKey(peerId, msgId);
	if (CacheIndex().contains(key) || !Pending().insert(key).second) {
		return;
	}
	if (&alive != &Lifetime()) {
		// The module lifetime never dies, so it needs no cleanup entries.
		alive.add([=] { Pending().erase(key); });
	}
	crl::async([=, refresh = std::move(refresh)]() mutable {
		auto records = Database::instance().LookupBySource(peerId, msgId);
		crl::on_main([
			=,
			records = std::move(records),
			refresh = std::move(refresh)
		]() mutable {
			ApplyLookup(peerId, msgId, std::move(records));
			if (refresh) {
				refresh();
			}
		});
	});
}

bool SetHovered(PeerId peerId, MsgId msgId, bool hovered) {
	const auto key = MakeKey(peerId, msgId);
	auto &current = Hovered();
	if (hovered) {
		if (current && *current == key) {
			return false;
		}
		current = key;
		return true;
	}
	if (current && *current == key) {
		current.reset();
		return true;
	}
	return false;
}

bool IsHovered(PeerId peerId, MsgId msgId) {
	const auto &current = Hovered();
	return current && (*current == MakeKey(peerId, msgId));
}

QRect BadgeRect(const QRect &mediaRect) {
	const auto size = st::sexcatBadgeSize;
	const auto shift = st::sexcatBadgeShift;
	return style::rtlrect(
		mediaRect.x() + mediaRect.width() - shift.right() - size,
		mediaRect.y() + shift.top(),
		size,
		size,
		mediaRect.width());
}

void PaintBadge(
		Painter &p,
		PeerId peerId,
		MsgId msgId,
		const QRect &mediaRect,
		bool selected) {
	if (!FeatureEnabled()) {
		return;
	}
	if (!ShouldShowBadge(peerId, msgId)) {
		Query(peerId, msgId, Lifetime());
		return;
	}
	const auto rect = BadgeRect(mediaRect);
	const auto &icon = selected
		? st::sexcatBadgeIconSelected
		: IsHovered(peerId, msgId)
		? st::sexcatBadgeIconHover
		: st::sexcatBadgeIcon;
	icon.paint(p, rect);
}

void AddContextMenuActions(
		not_null<Ui::PopupMenu*> menu,
		not_null<HistoryItem*> item) {
	if (!FeatureEnabled()) {
		return;
	}
	const auto fullId = item->fullId();
	if (!ShouldShowBadge(fullId.peer, fullId.msg)) {
		return;
	}
	const auto path = LastSavedPath(fullId.peer, fullId.msg);
	if (path.isEmpty()) {
		return;
	}
	const auto folder = QFileInfo(path).absolutePath();
	menu->addAction(
		tr::lng_sexcat_open_folder(tr::now),
		[=] { QDesktopServices::openUrl(QUrl::fromLocalFile(folder)); },
		&st::menuIconShowInFolder);
	menu->addAction(
		tr::lng_sexcat_view_history(tr::now),
		[=] { OpenHistoryWindow(); },
		&st::menuIconShowInChat);
	menu->addAction(
		tr::lng_sexcat_reverify(tr::now),
		[=] { RequestReverify(path); },
		&st::menuIconRefresh);
}

} // namespace SexCat::Badge
