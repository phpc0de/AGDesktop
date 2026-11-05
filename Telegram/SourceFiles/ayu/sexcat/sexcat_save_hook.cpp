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
#include "ayu/utils/telegram_helpers.h"
#include "base/unixtime.h"
#include "core/application.h"
#include "core/file_location.h"
#include "data/data_document.h"
#include "data/data_download_manager.h"
#include "data/data_peer.h"
#include "data/data_photo.h"
#include "data/data_session.h"
#include "history/history.h"
#include "history/history_item.h"
#include "lang_auto.h"
#include "main/main_session.h"
#include "rpl/rpl.h"
#include "storage/storage_account.h"
#include "ui/text/text_entity.h"
#include "ui/toast/toast.h"
#include "window/window_controller.h"
#include "window/window_session_controller.h"

#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <map>
#include <optional>
#include <set>
#include <vector>

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

constexpr auto kMaxDownloadedShas = 64;

std::map<QString, QByteArray> &DownloadedShaMap() {
	static std::map<QString, QByteArray> result;
	return result;
}

[[nodiscard]] std::optional<QByteArray> TakeDownloadedSha(
		const QString &path) {
	auto &map = DownloadedShaMap();
	const auto i = map.find(path);
	if (i == end(map)) {
		return std::nullopt;
	}
	auto sha = std::move(i->second);
	map.erase(i);
	return sha;
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

constexpr auto kMaxDuplicatePathProbes = 3;
constexpr auto kMaxPendingRehashRows = 256;

void DedupeOrRecordHashed(
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
	// WHY: same-sha content must not grow the library, but a fresh
	// copy still becomes the live carrier when every older one is gone.
	const auto hits = Database::instance().LookupByHash(sha);
	auto live = false;
	// probing every duplicate would walk the disk on each save
	for (auto i = 0;
		i < kMaxDuplicatePathProbes && i < int(hits.size());
		++i) {
		if (QFileInfo::exists(hits[i].saved_path)) {
			live = true;
			break;
		}
	}
	if (live) {
		LogFailure(u"duplicate content, record skipped: "_q + savedPath);
	} else if (!hits.empty()) {
		Database::instance().Relink(hits.front().id, savedPath);
		LogFailure(u"duplicate content relinked to new copy: "_q + savedPath);
	} else if (Database::instance().RecordSaved(std::move(entry)) <= 0) {
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

// WHY: the row is written before hashing, so a restart cannot lose
// it; the sha is backfilled once the hash queue reaches the file.
int64 PersistRecord(
		const QString &savedPath,
		int64 fileSize,
		int64 srcPeerId,
		int64 srcMsgId,
		const QString &srcTitle,
		int srcAttach,
		bool announce) {
	auto entry = Entry{
		.file_name = QFileInfo(savedPath).fileName(),
		.saved_path = savedPath,
		.file_size = fileSize,
		.src_peer_id = srcPeerId,
		.src_msg_id = srcMsgId,
		.src_title = srcTitle,
		.src_attach = srcAttach,
		.saved_at = base::unixtime::now(),
	};
	const auto id = Database::instance().RecordSaved(std::move(entry));
	if (id > 0 && announce) {
		ShowSavedToast();
	}
	return id;
}

void FinishHashedRecord(
		int64 rowId,
		const QString &savedPath,
		int64 srcPeerId,
		int64 srcMsgId,
		QString srcTitle,
		int srcAttach,
		QByteArray sha256) {
	if (rowId <= 0) {
		return;
	}
	if (sha256.isEmpty()) {
		// the row stays unhashed and is requeued on the next launch
		LogFailure(u"Failed to hash %1, record left pending rehash"_q.arg(savedPath));
		return;
	}
	const auto sha = sha256;
	// WHY: same-sha content must not grow the library, but a fresh
	// copy still becomes the live carrier when every older one is gone.
	const auto hits = Database::instance().LookupByHash(sha);
	auto live = false;
	for (auto i = 0;
		i < kMaxDuplicatePathProbes && i < int(hits.size());
		++i) {
		if (QFileInfo::exists(hits[i].saved_path)) {
			live = true;
			break;
		}
	}
	if (live) {
		Database::instance().DropUnhashedRecord(rowId);
		LogFailure(u"duplicate content, pending record dropped: "_q + savedPath);
	} else if (!hits.empty()) {
		Database::instance().Relink(hits.front().id, savedPath);
		Database::instance().DropUnhashedRecord(rowId);
		LogFailure(u"duplicate content relinked to new copy: "_q + savedPath);
	} else if (Database::instance().UpdateRowSha(rowId, sha)) {
		if (srcPeerId != 0 && srcMsgId != 0) {
			auto source = Entry{
				.sha256 = sha,
				.src_peer_id = srcPeerId,
				.src_msg_id = srcMsgId,
				.src_title = std::move(srcTitle),
				.src_attach = srcAttach,
			};
			Database::instance().BackfillSourceByHash(sha, source);
		}
	}
	HashStore::WriteAds(
		savedPath,
		sha,
		PeerId(uint64(srcPeerId)),
		MsgId(srcMsgId));
}

void ResolveHashAndPersist(
		QString savedPath,
		int64 fileSize,
		int64 srcPeerId,
		int64 srcMsgId,
		QString srcTitle,
		int srcAttach,
		bool announce) {
	if (const auto sha = TakeDownloadedSha(savedPath)) {
		DedupeOrRecordHashed(
			std::move(savedPath),
			fileSize,
			srcPeerId,
			srcMsgId,
			std::move(srcTitle),
			srcAttach,
			std::move(*sha),
			announce);
		return;
	}
	if (const auto hit = HashStore::ReadAds(savedPath)) {
		if (hit->sha256.size() == 32) {
			DedupeOrRecordHashed(
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
	const auto rowId = PersistRecord(
		savedPath,
		fileSize,
		srcPeerId,
		srcMsgId,
		srcTitle,
		srcAttach,
		announce);
	if (rowId <= 0) {
		return;
	}
	HashStore::HashFileAsync(
		savedPath,
		[=, path = savedPath, title = std::move(srcTitle)](QByteArray sha256) mutable {
			FinishHashedRecord(
				rowId,
				path,
				srcPeerId,
				srcMsgId,
				std::move(title),
				srcAttach,
				std::move(sha256));
		});
}

[[nodiscard]] std::optional<Entry> FindRecordedForDocument(
		not_null<DocumentData*> document,
		const QString &toFile) {
	const auto info = QFileInfo(toFile);
	if (document->size <= 0
		|| !info.isFile()
		|| info.size() != document->size) {
		return std::nullopt;
	}
	const auto ads = HashStore::ReadAds(toFile);
	if (auto hit = Database::instance().FindByPath(toFile, document->size)) {
		if (hit->sha256.size() == 32
			&& (!ads || ads->sha256 == hit->sha256)) {
			return hit;
		}
		return std::nullopt;
	}
	PruneStalePending();
	const auto i = PendingMap().find(document.get());
	if (i == end(PendingMap())) {
		return std::nullopt;
	}
	const auto item = document->owner().message(i->second.contextId);
	if (!item || !item->isRegular()) {
		return std::nullopt;
	}
	const auto peer = item->history()->peer;
	// Sidecar sha proves toFile really holds the pinned attachment.
	const auto attach = ResolveAttachIndex(item);
	for (const auto &entry
		: Database::instance().LookupBySource(peer->id, item->id)) {
		if (entry.src_attach != attach
			|| entry.file_size != document->size
			|| entry.sha256.size() != 32) {
			continue;
		}
		return (ads && ads->sha256 == entry.sha256)
			? std::optional<Entry>(entry)
			: std::nullopt;
	}
	return std::nullopt;
}

constexpr auto kMaxAugmentedNameLength = 240;
constexpr auto kAugmentedNameTries = 100;

[[nodiscard]] bool IsAllDigitsName(const QString &stem) {
	static const QRegularExpression digits(u"^[0-9]+$"_q);
	return !stem.isEmpty() && digits.match(stem).hasMatch();
}

[[nodiscard]] bool IsReservedWindowsName(const QString &name) {
	static const auto reserved = [] {
		auto names = std::vector<QString>{
			u"CON"_q, u"PRN"_q, u"AUX"_q, u"NUL"_q,
		};
		for (auto i = 1; i != 10; ++i) {
			const auto digit = QString::number(i);
			names.push_back(u"COM"_q + digit);
			names.push_back(u"LPT"_q + digit);
		}
		return names;
	}();
	const auto dot = name.indexOf(u'.');
	const auto stem = (dot >= 0) ? name.left(dot) : name;
	for (const auto &reservedName : reserved) {
		if (stem.compare(reservedName, Qt::CaseInsensitive) == 0) {
			return true;
		}
	}
	return false;
}

[[nodiscard]] QString CleanSaveNameText(const QString &raw) {
	static const auto illegal = u"\\/:*?\"<>|"_q;
	auto cleaned = QString();
	cleaned.reserve(raw.size());
	for (const auto ch : raw) {
		if (ch.category() == QChar::Other_Control || illegal.contains(ch)) {
			cleaned += u' ';
		} else {
			cleaned += ch;
		}
	}
	const auto isEdgeJunk = [](QChar ch) {
		return ch == u' ' || ch == u'.';
	};
	auto start = 0;
	auto end = cleaned.size();
	while (start < end && isEdgeJunk(cleaned.at(start))) {
		++start;
	}
	while (end > start && isEdgeJunk(cleaned.at(end - 1))) {
		--end;
	}
	return cleaned.mid(start, end - start);
}

[[nodiscard]] QString BuildAugmentedName(
		const QString &original,
		const QString &text) {
	auto cleaned = CleanSaveNameText(text);
	if (cleaned.isEmpty()) {
		return {};
	}
	if (IsReservedWindowsName(cleaned)) {
		cleaned = u"_"_q + cleaned;
	}
	auto name = cleaned + u'-' + original;
	if (name.size() > kMaxAugmentedNameLength) {
		const auto keep = kMaxAugmentedNameLength - original.size() - 1;
		if (keep <= 0) {
			return {};
		}
		name = cleaned.left(keep) + u'-' + original;
	}
	return name;
}

// The server names captioned videos by bare numeric ids.
[[nodiscard]] QString MaybeAugmentedSavePath(
		const QString &savedPath,
		not_null<DocumentData*> document,
		HistoryItem *item) {
	if (!item) {
		return savedPath;
	}
	const auto info = QFileInfo(savedPath);
	const auto original = info.fileName();
	const auto dot = original.lastIndexOf(u'.');
	const auto stem = (dot > 0) ? original.left(dot) : original;
	if (!IsAllDigitsName(stem)) {
		return savedPath;
	}
	if (item->emptyText()) {
		return savedPath;
	}
	const auto candidate = BuildAugmentedName(original, extractText(item).text);
	if (candidate.isEmpty()) {
		return savedPath;
	}
	const auto dir = info.dir();
	auto target = dir.filePath(candidate);
	const auto dotCandidate = candidate.lastIndexOf(u'.');
	const auto candidateStem = (dotCandidate > 0)
		? candidate.left(dotCandidate)
		: candidate;
	const auto candidateExt = (dotCandidate > 0)
		? candidate.mid(dotCandidate)
		: QString();
	auto available = !QFileInfo::exists(target);
	for (auto i = 2; !available && i < kAugmentedNameTries; ++i) {
		target = dir.filePath(candidateStem + u" (%1)"_q.arg(i) + candidateExt);
		available = !QFileInfo::exists(target);
	}
	if (!available || !QFile::rename(savedPath, target)) {
		LogFailure(u"failed to rename saved file to %1"_q.arg(target));
		return savedPath;
	}
	if (document->filepath() == savedPath) {
		document->setLocation(Core::FileLocation(target));
		document->session().local().writeFileLocation(
			document->mediaKey(),
			Core::FileLocation(target));
	}
	LogFailure(u"save name augmented: %1 -> %2"_q.arg(original, target));
	return target;
}

void HandleLoadedDownload(not_null<const Data::DownloadedId*> id) {
	if (id->object && id->object->document) {
		Ayu::SexCat::RememberDownloadedDocument(id->object->document);
	}
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

bool ShouldSkipDuplicateDownload(
		not_null<DocumentData*> document,
		const QString &toFile) {
	EnsureDownloadListener();
	if (!Enabled()
		|| !Database::instance().Enabled()
		|| toFile.isEmpty()) {
		return false;
	}
	const auto hit = FindRecordedForDocument(document, toFile);
	if (!hit) {
		return false;
	}
	LogFailure(u"skipping duplicate download, already in library: "_q + toFile);
	return true;
}

QString RecordedDocumentPath(
		not_null<DocumentData*> document,
		FullMsgId contextId) {
	if (!Enabled() || !Database::instance().Enabled()) {
		return {};
	}
	const auto item = contextId
		? document->owner().message(contextId)
		: nullptr;
	if (!item || !item->isRegular()) {
		return {};
	}
	const auto peer = item->history()->peer;
	if (!peer) {
		return {};
	}
	const auto hit = Database::instance().FindSavedSource(
		peer->id,
		item->id,
		ResolveAttachIndex(item),
		document->size);
	if (!hit || !QFileInfo::exists(hit->saved_path)) {
		return {};
	}
	return hit->saved_path;
}

bool ShouldTrackDownloadHash() {
	return Enabled() && Database::instance().Enabled();
}

void NoteDownloadedSha(const QString &path, const QByteArray &sha256) {
	if (!Enabled() || path.isEmpty() || sha256.size() != 32) {
		return;
	}
	auto &map = DownloadedShaMap();
	while (map.size() >= size_t(kMaxDownloadedShas)) {
		map.erase(map.begin());
	}
	map.insert_or_assign(path, sha256);
}

void RecordSaveAttempt(
		const QString &savedPath,
		not_null<DocumentData*> document,
		FullMsgId contextId,
		int attachIndex) {
	EnsureDownloadListener();
	Ayu::SexCat::RememberDownloadedDocument(document);
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
	const auto path = MaybeAugmentedSavePath(savedPath, document, item);
	if (item) {
		if (const auto peer = item->history()->peer) {
			srcPeerId = int64(peer->id.value);
			srcTitle = peer->name();
		}
		srcMsgId = int64(item->id.bare);
	}
	if (!Database::instance().Enabled()) {
		LogFailure(u"database unavailable, save not recorded: "_q + path);
		return;
	}
	ResolveHashAndPersist(
		path,
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
	if (map.size() >= size_t(kPendingSaveAsMax)) {
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

// WHY: unhashed rows are the pending-hash marker; rehashing them on
// the first save/download event recovers a queue lost to an exit.
void RehashPendingRows() {
	static bool done = false;
	if (done) {
		return;
	}
	done = true;
	for (const auto &entry
		: Database::instance().LookupUnhashed(kMaxPendingRehashRows)) {
		if (!QFileInfo(entry.saved_path).isFile()) {
			continue;
		}
		HashStore::HashFileAsync(
			entry.saved_path,
			[=, path = entry.saved_path, title = entry.src_title](
				QByteArray sha256) mutable {
				FinishHashedRecord(
					entry.id,
					path,
					entry.src_peer_id,
					entry.src_msg_id,
					std::move(title),
					entry.src_attach,
					std::move(sha256));
			});
	}
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
		RehashPendingRows();
		HandleLoadedDownload(id);
	}, lifetime);
	StartShellWatch();
	RehashPendingRows();
}

} // namespace SexCat

namespace Ayu::SexCat {

namespace {

constexpr auto kMaxDownloadedDocumentIds = 8192;

std::set<DocumentId> &DownloadedDocumentIds() {
	static std::set<DocumentId> result;
	return result;
}

} // namespace

// WHY: DocumentData objects can be recreated mid-session while the
// file stays on disk, so ids are tracked instead of object pointers.
void RememberDownloadedDocument(not_null<const DocumentData*> document) {
	auto &ids = DownloadedDocumentIds();
	while (ids.size() >= size_t(kMaxDownloadedDocumentIds)) {
		ids.erase(ids.begin());
	}
	ids.insert(document->id);
}

bool IsDownloadedDocument(not_null<const DocumentData*> document) {
	return DownloadedDocumentIds().contains(document->id);
}

} // namespace Ayu::SexCat
