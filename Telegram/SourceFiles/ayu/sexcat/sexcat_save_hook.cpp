// This is the source code of AyuGram for Desktop.
//
// We do not and cannot prevent the use of our code,
// but be respectful and credit the original author.
//
// Copyright @Radolyn, 2026
#include "ayu/sexcat/sexcat_save_hook.h"

#include "ayu/ayu_settings.h"
#include "ayu/sexcat/sexcat_database.h"
#include "ayu/sexcat/sexcat_hash_store.h"
#include "ayu/sexcat/sexcat_shell_watch.h"
#include "ayu/utils/ayu_log.h"
#include "base/unixtime.h"
#include "core/application.h"
#include "data/data_document.h"
#include "data/data_download_manager.h"
#include "data/data_peer.h"
#include "data/data_photo.h"
#include "data/data_session.h"
#include "history/history.h"
#include "history/history_item.h"
#include "lang_auto.h"
#include "rpl/rpl.h"
#include "ui/text/text_entity.h"
#include "ui/toast/toast.h"
#include "window/window_controller.h"
#include "window/window_session_controller.h"

#include <QFileInfo>
#include <map>
#include <optional>

namespace SexCat {
namespace {

constexpr auto kSexCatToastDuration = crl::time(3000);

struct PendingSaveAs {
	FullMsgId contextId;
	crl::time when = 0;
};

constexpr auto kPendingSaveAsTtl = 10 * crl::time(60'000);
constexpr auto kPendingSaveAsMax = 32;

std::map<DocumentData*, PendingSaveAs> &PendingMap() {
	static std::map<DocumentData*, PendingSaveAs> result;
	return result;
}

// WHY: a failed save can die without its Forget callback and
// DocumentData is not a QObject, so entries age out and the map
// stays bounded instead of tracking document lifetimes.
void PruneStalePending() {
	auto &map = PendingMap();
	const auto now = crl::now();
	for (auto i = map.begin(); i != map.end();) {
		if (now - i->second.when > kPendingSaveAsTtl) {
			i = map.erase(i);
		} else {
			++i;
		}
	}
}

[[nodiscard]] bool Enabled() {
	return AyuSettings::getInstance().sexcatEnabled();
}

void LogFailure(const QString &message) {
	Ayu::Log::Write(u"sexcat"_q, message);
}

void ShowSavedToast() {
	auto text = tr::lng_sexcat_saved_toast(tr::now);
	auto config = Ui::Toast::Config{
		.text = TextWithEntities{ std::move(text) },
		.duration = kSexCatToastDuration,
	};
	if (const auto window = Core::App().activeWindow()) {
		if (const auto controller = window->sessionController()) {
			controller->showToast(std::move(config));
			return;
		}
	}
	Ui::Toast::Show(std::move(config));
}

[[nodiscard]] int ResolveAttachIndex(HistoryItem *item) {
	if (!item) {
		return 0;
	}
	const auto group = item->history()->owner().groups().find(item);
	if (!group) {
		return 0;
	}
	auto index = 0;
	for (const auto &groupItem : group->items) {
		if (&*groupItem == item) {
			return index;
		}
		++index;
	}
	return 0;
}

void Persist(
		QString savedPath,
		int64 fileSize,
		int64 srcPeerId,
		int64 srcMsgId,
		QString srcTitle,
		int srcAttach,
		QByteArray sha256,
		bool announce) {
	auto entry = Entry{
		.sha256 = std::move(sha256),
		.file_name = QFileInfo(savedPath).fileName(),
		.saved_path = savedPath,
		.file_size = fileSize,
		.src_peer_id = srcPeerId,
		.src_msg_id = srcMsgId,
		.src_title = std::move(srcTitle),
		.src_attach = srcAttach,
		.saved_at = base::unixtime::now(),
	};
	const auto sha = entry.sha256;
	if (!Database::instance().RecordSaved(std::move(entry))) {
		return;
	}
	HashStore::WriteAds(
		savedPath,
		sha,
		PeerId(uint64(srcPeerId)),
		MsgId(srcMsgId));
	if (announce) {
		ShowSavedToast();
	}
}

void ResolveHashAndPersist(
		QString savedPath,
		int64 fileSize,
		int64 srcPeerId,
		int64 srcMsgId,
		QString srcTitle,
		int srcAttach,
		bool announce) {
	if (const auto hit = HashStore::ReadAds(savedPath)) {
		if (hit->sha256.size() == 32) {
			Persist(
				std::move(savedPath),
				fileSize,
				srcPeerId,
				srcMsgId,
				std::move(srcTitle),
				srcAttach,
				std::move(hit->sha256),
				announce);
			return;
		}
	}
	HashStore::HashFileAsync(
		savedPath,
		[=, path = savedPath, title = srcTitle](QByteArray sha256) mutable {
			if (sha256.isEmpty()) {
				LogFailure(u"Failed to hash %1, save not recorded"_q.arg(path));
				return;
			}
			Persist(
				std::move(path),
				fileSize,
				srcPeerId,
				srcMsgId,
				std::move(title),
				srcAttach,
				std::move(sha256),
				announce);
		});
}

void HandleLoadedDownload(not_null<const Data::DownloadedId*> id) {
	if (!Enabled()) {
		return;
	}
	const auto info = QFileInfo(id->path);
	if (id->path.isEmpty() || !info.exists() || info.isDir()) {
		return;
	}
	if (!Database::instance().Enabled()) {
		LogFailure(u"database unavailable, download not recorded: "_q + id->path);
		return;
	}
	if (Database::instance().FindByPath(id->path, id->size)) {
		return;
	}
	auto srcPeerId = int64(0);
	auto srcMsgId = int64(0);
	auto srcTitle = QString();
	auto attachIndex = 0;
	if (const auto item = id->object ? id->object->item.get() : nullptr) {
		// Restored downloads carry generated items, not a real source.
		if (item->isRegular()) {
			if (const auto peer = item->history()->peer) {
				srcPeerId = int64(peer->id.value);
				srcTitle = peer->name();
			}
			srcMsgId = int64(item->id.bare);
			attachIndex = ResolveAttachIndex(item);
		}
	}
	ResolveHashAndPersist(
		id->path,
		id->size,
		srcPeerId,
		srcMsgId,
		std::move(srcTitle),
		attachIndex,
		false);
}

} // namespace

void RecordSaveAttempt(
		const QString &savedPath,
		not_null<DocumentData*> document,
		FullMsgId contextId,
		int attachIndex) {
	EnsureDownloadListener();
	if (!Enabled()) {
		return;
	}
	const auto info = QFileInfo(savedPath);
	if (savedPath.isEmpty() || !info.exists() || info.isDir()) {
		return;
	}
	auto srcPeerId = int64(0);
	auto srcMsgId = int64(0);
	auto srcTitle = QString();
	const auto item = contextId
		? document->owner().message(contextId)
		: nullptr;
	if (item) {
		if (const auto peer = item->history()->peer) {
			srcPeerId = int64(peer->id.value);
			srcTitle = peer->name();
		}
		srcMsgId = int64(item->id.bare);
	}
	if (!Database::instance().Enabled()) {
		LogFailure(u"database unavailable, save not recorded: "_q + savedPath);
		return;
	}
	ResolveHashAndPersist(
		savedPath,
		info.size(),
		srcPeerId,
		srcMsgId,
		std::move(srcTitle),
		attachIndex ? attachIndex : ResolveAttachIndex(item),
		true);
}

void RecordPhotoSaveAttempt(
		const QString &savedPath,
		not_null<PhotoData*> photo,
		FullMsgId contextId,
		int attachIndex) {
	EnsureDownloadListener();
	if (!Enabled()) {
		return;
	}
	const auto info = QFileInfo(savedPath);
	if (savedPath.isEmpty() || !info.exists() || info.isDir()) {
		return;
	}
	auto srcPeerId = int64(0);
	auto srcMsgId = int64(0);
	auto srcTitle = QString();
	const auto item = contextId
		? photo->owner().message(contextId)
		: nullptr;
	if (item) {
		if (const auto peer = item->history()->peer) {
			srcPeerId = int64(peer->id.value);
			srcTitle = peer->name();
		}
		srcMsgId = int64(item->id.bare);
	}
	if (!Database::instance().Enabled()) {
		LogFailure(u"database unavailable, photo save not recorded: "_q + savedPath);
		return;
	}
	ResolveHashAndPersist(
		savedPath,
		info.size(),
		srcPeerId,
		srcMsgId,
		std::move(srcTitle),
		attachIndex ? attachIndex : ResolveAttachIndex(item),
		true);
}

void MarkDocumentSaveAs(
		not_null<DocumentData*> document,
		FullMsgId contextId) {
	PruneStalePending();
	auto &map = PendingMap();
	if (map.size() >= kPendingSaveAsMax) {
		map.clear();
	}
	map[document.get()] = PendingSaveAs{ contextId, crl::now() };
}

void NotifyDocumentSaved(
		const QString &savedPath,
		not_null<DocumentData*> document) {
	PruneStalePending();
	const auto i = PendingMap().find(document.get());
	if (i == end(PendingMap())) {
		return;
	}
	const auto contextId = i->second.contextId;
	PendingMap().erase(i);
	if (savedPath.isEmpty()) {
		return;
	}
	RecordSaveAttempt(savedPath, document, contextId);
}

void ForgetDoctrineSaveAs(not_null<DocumentData*> document) {
	PendingMap().erase(document.get());
}

void EnsureDownloadListener() {
	static bool subscribed = false;
	static rpl::lifetime lifetime;
	if (subscribed) {
		return;
	}
	subscribed = true;
	Core::App().downloadManager().loadedAdded(
	) | rpl::on_next([=](not_null<const Data::DownloadedId*> id) {
		HandleLoadedDownload(id);
	}, lifetime);
	StartShellWatch();
}

} // namespace SexCat
