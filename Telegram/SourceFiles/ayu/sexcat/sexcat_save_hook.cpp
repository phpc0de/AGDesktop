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
#include "base/unique_qptr.h"
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
#include "styles/style_menu_icons.h"
#include "styles/style_widgets.h"
#include "ui/text/text_entity.h"
#include "ui/toast/toast.h"
#include "ui/widgets/menu/menu_action.h"
#include "ui/widgets/menu/menu_add_action_callback.h"
#include "ui/widgets/menu/menu_add_action_callback_factory.h"
#include "ui/widgets/popup_menu.h"
#include "window/window_controller.h"
#include "window/window_session_controller.h"

#include <QFile>
#include <QFileInfo>
#include <QMenu>
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
	QString sourceTag;
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

void ShowToastText(QString text) {
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

void ShowSavedToast() {
	ShowToastText(tr::lng_sexcat_saved_toast(tr::now));
}

constexpr auto kMaxSowTagLength = 64;
constexpr auto kMaxRetroTagChoices = 32;

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

[[nodiscard]] QString CleanSowTag(const QString &raw) {
	auto text = raw;
	while (!text.isEmpty() && text.at(0) == u'#') {
		text.remove(0, 1);
	}
	text = CleanSaveNameText(text);
	return text.left(kMaxSowTagLength);
}

void ShowSowTagToast(const QString &tag) {
	ShowToastText(u"Saved to sow list: #%1"_q.arg(tag));
}

// WHY: the client's own hashtag parsing (message entities, and the
// HashtagClickHandler built from them) already accepts any script, so
// entities are the primary source; the unicode regex only covers texts
// that carry no entities at all.
[[nodiscard]] QString CleanTagToken(const QString &raw) {
	auto text = raw;
	while (!text.isEmpty() && text.at(0) == u'#') {
		text.remove(0, 1);
	}
	while (!text.isEmpty()) {
		const auto cat = text.back().category();
		if (text.back() == u'#'
			|| cat == QChar::Punctuation_Close
			|| cat == QChar::Punctuation_FinalQuote
			|| cat == QChar::Punctuation_Other) {
			text.chop(1);
		} else {
			break;
		}
	}
	return CleanSaveNameText(text).left(kMaxSowTagLength);
}

struct HashtagParts {
	QStringList tags;
	QString description;
};

// Tags in order plus the caption text with every hashtag span removed.
[[nodiscard]] HashtagParts SplitHashtags(const TextWithEntities &text) {
	auto spans = std::vector<std::pair<int, int>>();
	auto tags = QStringList();
	const auto push = [&](int offset, int length) {
		const auto token = CleanTagToken(text.text.mid(offset, length));
		if (token.isEmpty() || tags.contains(token)) {
			return;
		}
		tags.push_back(token);
		spans.emplace_back(offset, length);
	};
	for (const auto &entity : text.entities) {
		if (entity.type() == EntityType::Hashtag) {
			push(entity.offset(), entity.length());
		}
	}
	if (spans.empty()) {
		static const QRegularExpression pattern(u"#([^\\s#]{1,64})"_q);
		auto match = pattern.match(text.text);
		while (match.hasMatch()) {
			push(match.capturedStart(), match.capturedLength());
			match = pattern.match(text.text, match.capturedEnd());
		}
	}
	if (tags.isEmpty()) {
		return {};
	}
	auto raw = QString();
	auto last = 0;
	for (const auto &[offset, length] : spans) {
		raw += text.text.mid(last, offset - last);
		last = offset + length;
	}
	raw += text.text.mid(last);
	auto squeezed = QString();
	auto pendingSpace = false;
	for (const auto ch : raw) {
		if (ch.isSpace()) {
			pendingSpace = true;
		} else {
			if (pendingSpace && !squeezed.isEmpty()) {
				squeezed += u' ';
			}
			pendingSpace = false;
			squeezed += ch;
		}
	}
	return { .tags = std::move(tags), .description = CleanSaveNameText(squeezed) };
}

// Tags of the item and its album siblings; the description is the first
// caption text left after hashtag removal (album captions sit on the
// first item, videos alone carry none).
[[nodiscard]] HashtagParts AugmentNameParts(HistoryItem *item) {
	auto result = HashtagParts{};
	if (!item || !item->isRegular()) {
		return result;
	}
	const auto collect = [&](HistoryItem *groupItem) {
		auto parts = SplitHashtags(groupItem->originalText());
		for (const auto &tag : parts.tags) {
			if (!result.tags.contains(tag)) {
				result.tags.push_back(tag);
			}
		}
		if (result.description.isEmpty() && !parts.description.isEmpty()) {
			result.description = parts.description;
		}
	};
	collect(item);
	if (const auto group = item->history()->owner().groups().find(item)) {
		for (const auto &groupItem : group->items) {
			if (&*groupItem != item) {
				collect(groupItem.get());
			}
		}
	}
	return result;
}

[[nodiscard]] QString TagFromLink(const ClickHandlerPtr &link) {
	if (!link || link->getTextEntity().type != EntityType::Hashtag) {
		return {};
	}
	return CleanSowTag(link->dragText());
}

void SaveSowTag(const QString &raw) {
	const auto tag = CleanSowTag(raw);
	if (tag.isEmpty()) {
		return;
	}
	if (!Database::instance().Enabled()) {
		LogFailure(u"database unavailable, sow tag not recorded: "_q + tag);
		return;
	}
	Database::instance().RecordTag(tag);
	ShowSowTagToast(tag);
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

// Q52: a sow tag is an arbitrary string label, not a peer.
struct SourceOrigin {
	int64 peerId = 0;
	int64 msgId = 0;
	QString title;
	QString tag;
	int attach = 0;

	[[nodiscard]] bool known() const {
		return peerId != 0 && msgId != 0;
	}
};

[[nodiscard]] SourceOrigin OriginFromItem(
		HistoryItem *item,
		QString tag,
		int attachOverride = 0) {
	auto origin = SourceOrigin{ .tag = std::move(tag) };
	if (!item || !item->isRegular()) {
		return origin;
	}
	if (const auto peer = item->history()->peer) {
		origin.peerId = int64(peer->id.value);
		origin.title = peer->name();
	}
	origin.msgId = int64(item->id.bare);
	origin.attach = attachOverride ? attachOverride : ResolveAttachIndex(item);
	if (origin.tag.isEmpty()) {
		origin.tag = SourceTagForItem(item);
	}
	return origin;
}

[[nodiscard]] Entry SourceEntryForPending(
		const SourceOrigin &origin,
		int64 fileSize) {
	return Entry{
		.file_size = fileSize,
		.src_peer_id = origin.peerId,
		.src_msg_id = origin.msgId,
		.src_title = origin.title,
		.src_tag = origin.tag,
		.src_attach = origin.attach,
	};
}

constexpr auto kMaxDuplicatePathProbes = 3;
constexpr auto kMaxPendingRehashRows = 256;
constexpr auto kPendingPositionTtl = 6 * 60 * 60;

void DedupeOrRecordHashed(
		QString savedPath,
		int64 fileSize,
		SourceOrigin origin,
		QByteArray sha256,
		int64 existingRowId,
		bool announce) {
	const auto sha = sha256;
	// Q80: a pending row written at download start becomes this record.
	const auto pendingId = (existingRowId > 0)
		? existingRowId
		: (origin.known()
			? Database::instance().ConsumePendingPosition(
				SourceEntryForPending(origin, fileSize),
				savedPath)
			: 0);
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
		if (pendingId > 0) {
			Database::instance().DropUnhashedRecord(pendingId);
		}
		LogFailure(u"duplicate content, record skipped: "_q + savedPath);
	} else if (!hits.empty()) {
		Database::instance().Relink(hits.front().id, savedPath);
		if (pendingId > 0) {
			Database::instance().DropUnhashedRecord(pendingId);
		}
		LogFailure(u"duplicate content relinked to new copy: "_q + savedPath);
	} else if (pendingId > 0) {
		Database::instance().UpdateRowSha(pendingId, sha);
	} else {
		auto entry = Entry{
			.sha256 = sha,
			.file_name = QFileInfo(savedPath).fileName(),
			.saved_path = savedPath,
			.file_size = fileSize,
			.src_peer_id = origin.peerId,
			.src_msg_id = origin.msgId,
			.src_title = origin.title,
			.src_tag = origin.tag,
			.src_attach = origin.attach,
			.saved_at = base::unixtime::now(),
		};
		if (Database::instance().RecordSaved(std::move(entry)) <= 0) {
			return;
		}
	}
	Database::instance().RecordTag(origin.tag);
	HashStore::WriteAds(
		savedPath,
		sha,
		PeerId(uint64(origin.peerId)),
		MsgId(origin.msgId));
	if (announce) {
		ShowSavedToast();
	}
}

// WHY: the row is written before hashing, so a restart cannot lose
// it; the sha is backfilled once the hash queue reaches the file.
int64 PersistRecord(
		const QString &savedPath,
		int64 fileSize,
		const SourceOrigin &origin,
		bool announce) {
	auto entry = Entry{
		.file_name = QFileInfo(savedPath).fileName(),
		.saved_path = savedPath,
		.file_size = fileSize,
		.src_peer_id = origin.peerId,
		.src_msg_id = origin.msgId,
		.src_title = origin.title,
		.src_tag = origin.tag,
		.src_attach = origin.attach,
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
		SourceOrigin origin,
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
		if (origin.known()) {
			auto source = Entry{
				.sha256 = sha,
				.src_peer_id = origin.peerId,
				.src_msg_id = origin.msgId,
				.src_title = origin.title,
				.src_tag = origin.tag,
				.src_attach = origin.attach,
			};
			Database::instance().BackfillSourceByHash(sha, source);
		}
	}
	Database::instance().RecordTag(origin.tag);
	HashStore::WriteAds(
		savedPath,
		sha,
		PeerId(uint64(origin.peerId)),
		MsgId(origin.msgId));
}

void ResolveHashAndPersist(
		QString savedPath,
		int64 fileSize,
		SourceOrigin origin,
		bool announce,
		int64 existingRowId = 0) {
	if (const auto sha = TakeDownloadedSha(savedPath)) {
		DedupeOrRecordHashed(
			std::move(savedPath),
			fileSize,
			std::move(origin),
			std::move(*sha),
			existingRowId,
			announce);
		return;
	}
	if (const auto hit = HashStore::ReadAds(savedPath)) {
		if (hit->sha256.size() == 32) {
			DedupeOrRecordHashed(
				std::move(savedPath),
				fileSize,
				std::move(origin),
				std::move(hit->sha256),
				existingRowId,
				announce);
			return;
		}
	}
	// Q80: a pending download-start row carries this position already.
	const auto pendingId = (existingRowId > 0)
		? existingRowId
		: (origin.known()
			? Database::instance().ConsumePendingPosition(
				SourceEntryForPending(origin, fileSize),
				savedPath)
			: 0);
	const auto rowId = (pendingId > 0)
		? pendingId
		: PersistRecord(savedPath, fileSize, origin, announce);
	if (rowId <= 0) {
		return;
	}
	HashStore::HashFileAsync(
		savedPath,
		[=, path = savedPath, origin = origin](QByteArray sha256) mutable {
			FinishHashedRecord(
				rowId,
				path,
				std::move(origin),
				std::move(sha256));
		});
}

[[nodiscard]] std::optional<Entry> FindRecordedForDocument(
		not_null<DocumentData*> document,
		const QString &toFile) {
	const auto info = QFileInfo(toFile);
	const auto fileMatches = (document->size > 0
		&& info.isFile()
		&& info.size() == document->size);
	const auto ads = fileMatches ? HashStore::ReadAds(toFile) : std::nullopt;
	if (fileMatches) {
		if (auto hit = Database::instance().FindByPath(toFile, document->size)) {
			if (hit->sha256.size() == 32
				&& (!ads || ads->sha256 == hit->sha256)) {
				return hit;
			}
		}
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
			|| entry.file_size != document->size) {
			continue;
		}
		// Q80: nothing yet at toFile — a recorded row for this
		// (peer, msg, attach) position skips the download instantly.
		if (!fileMatches) {
			if (entry.saved_path.isEmpty()) {
				// download-start marker, trusted only while fresh
				return (base::unixtime::now() - entry.saved_at
					< kPendingPositionTtl)
					? std::optional<Entry>(entry)
					: std::nullopt;
			}
			return QFileInfo::exists(entry.saved_path)
				? std::optional<Entry>(entry)
				: std::nullopt;
		} else if (entry.sha256.size() != 32) {
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

// Q49 naming: "#tags -description-original" with absent segments and
// their separators dropped.
[[nodiscard]] QString BuildAugmentedName(
		const QString &original,
		const HashtagParts &parts) {
	auto segments = QStringList();
	if (!parts.tags.isEmpty()) {
		auto tags = QStringList();
		for (const auto &tag : parts.tags) {
			tags.push_back(u"#%1"_q.arg(tag));
		}
		segments.push_back(tags.join(u' '));
	}
	if (!parts.description.isEmpty()) {
		segments.push_back(parts.description);
	}
	if (segments.isEmpty()) {
		return {};
	}
	auto text = segments.join(u" -"_q);
	if (IsReservedWindowsName(text)) {
		text = u"_"_q + text;
	}
	auto name = text + u'-' + original;
	if (name.size() > kMaxAugmentedNameLength) {
		const auto keep = kMaxAugmentedNameLength - original.size() - 1;
		if (keep <= 0) {
			return {};
		}
		name = text.left(keep) + u'-' + original;
	}
	return name;
}

[[nodiscard]] QString UniqueAugmentedTarget(
		const QFileInfo &info,
		const QString &candidate) {
	const auto dir = info.dir();
	const auto dot = candidate.lastIndexOf(u'.');
	const auto stem = (dot > 0) ? candidate.left(dot) : candidate;
	const auto ext = (dot > 0) ? candidate.mid(dot) : QString();
	auto target = dir.filePath(candidate);
	for (auto i = 2; QFileInfo::exists(target) && i < kAugmentedNameTries; ++i) {
		target = dir.filePath(stem + u" (%1)"_q.arg(i) + ext);
	}
	return target;
}

// The server names captioned videos by bare numeric ids.
[[nodiscard]] QString MaybeAugmentedSavePath(
		const QString &savedPath,
		not_null<DocumentData*> document,
		HistoryItem *item) {
	if (!item || !item->isRegular()) {
		return savedPath;
	}
	const auto info = QFileInfo(savedPath);
	const auto original = info.fileName();
	const auto dot = original.lastIndexOf(u'.');
	const auto stem = (dot > 0) ? original.left(dot) : original;
	if (!IsAllDigitsName(stem)) {
		return savedPath;
	}
	const auto candidate = BuildAugmentedName(original, AugmentNameParts(item));
	if (candidate.isEmpty()) {
		return savedPath;
	}
	const auto target = UniqueAugmentedTarget(info, candidate);
	if (target == savedPath || !QFile::rename(savedPath, target)) {
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
	auto path = id->path;
	if (id->object && id->object->document) {
		path = MaybeAugmentedSavePath(
			path,
			id->object->document,
			id->object->item.get());
	}
	const auto info = QFileInfo(path);
	if (path.isEmpty() || !info.exists() || info.isDir()) {
		return;
	}
	if (!Database::instance().Enabled()) {
		LogFailure(u"database unavailable, download not recorded: "_q + path);
		return;
	}
	auto existingRowId = int64(0);
	if (const auto hit = Database::instance().FindByPath(path, id->size)) {
		if (hit->sha256.size() == 32) {
			return;
		}
		// an unhashed row on this path is hashed in place, not duplicated
		existingRowId = hit->id;
	}
	auto origin = SourceOrigin{};
	if (const auto item = id->object ? id->object->item.get() : nullptr) {
		// Restored downloads carry generated items, not a real source.
		if (item->isRegular()) {
			origin = OriginFromItem(item, QString());
		}
	}
	ResolveHashAndPersist(
		path,
		id->size,
		std::move(origin),
		false,
		existingRowId);
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
	if (hit) {
		LogFailure(u"skipping duplicate download, already in library: "_q + toFile);
		return true;
	}
	// Q80: record the (peer, msg, attach) position at download start,
	// so the next download of the same position dedups before any sha.
	PruneStalePending();
	if (const auto i = PendingMap().find(document.get()); i != end(PendingMap())) {
		const auto item = document->owner().message(i->second.contextId);
		if (item && item->isRegular()) {
			const auto origin = OriginFromItem(item, i->second.sourceTag);
			Database::instance().RecordPendingPosition(
				SourceEntryForPending(origin, document->size));
		}
	}
	return false;
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

bool HasRecordedSource(
		not_null<DocumentData*> document,
		FullMsgId contextId) {
	if (!Enabled() || !Database::instance().Enabled()) {
		return false;
	}
	const auto item = contextId
		? document->owner().message(contextId)
		: nullptr;
	if (!item || !item->isRegular()) {
		return false;
	}
	const auto peer = item->history()->peer;
	if (!peer) {
		return false;
	}
	return Database::instance().FindSavedSource(
		peer->id,
		item->id,
		ResolveAttachIndex(item),
		document->size).has_value();
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
		int attachIndex,
		const QString &sourceTag) {
	EnsureDownloadListener();
	Ayu::SexCat::RememberDownloadedDocument(document);
	if (!Enabled()) {
		return;
	}
	const auto info = QFileInfo(savedPath);
	if (savedPath.isEmpty() || !info.exists() || info.isDir()) {
		return;
	}
	const auto item = contextId
		? document->owner().message(contextId)
		: nullptr;
	const auto path = MaybeAugmentedSavePath(savedPath, document, item);
	auto origin = OriginFromItem(item, sourceTag, attachIndex);
	if (!Database::instance().Enabled()) {
		LogFailure(u"database unavailable, save not recorded: "_q + path);
		return;
	}
	ResolveHashAndPersist(path, info.size(), std::move(origin), true);
}

void RecordPhotoSaveAttempt(
		const QString &savedPath,
		not_null<PhotoData*> photo,
		FullMsgId contextId,
		int attachIndex,
		const QString &sourceTag) {
	EnsureDownloadListener();
	if (!Enabled()) {
		return;
	}
	const auto info = QFileInfo(savedPath);
	if (savedPath.isEmpty() || !info.exists() || info.isDir()) {
		return;
	}
	const auto item = contextId
		? photo->owner().message(contextId)
		: nullptr;
	auto origin = OriginFromItem(item, sourceTag, attachIndex);
	if (!Database::instance().Enabled()) {
		LogFailure(u"database unavailable, photo save not recorded: "_q + savedPath);
		return;
	}
	ResolveHashAndPersist(savedPath, info.size(), std::move(origin), true);
}

void MarkDocumentSaveAs(
		not_null<DocumentData*> document,
		FullMsgId contextId,
		const QString &sourceTag) {
	PruneStalePending();
	auto &map = PendingMap();
	if (map.size() >= size_t(kPendingSaveAsMax)) {
		map.clear();
	}
	map[document.get()] = PendingSaveAs{ contextId, sourceTag, crl::now() };
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
	const auto sourceTag = i->second.sourceTag;
	PendingMap().erase(i);
	if (savedPath.isEmpty()) {
		return;
	}
	RecordSaveAttempt(savedPath, document, contextId, 0, sourceTag);
}

void ForgetDoctrineSaveAs(not_null<DocumentData*> document) {
	// WHY: a cancelled download must not leave its download-start
	// marker behind, or every later download of that position is
	// deduped into nothing.
	if (const auto i = PendingMap().find(document.get());
		i != end(PendingMap())) {
		const auto item = document->owner().message(i->second.contextId);
		if (item && item->isRegular()) {
			const auto origin = OriginFromItem(item, i->second.sourceTag);
			Database::instance().DropPendingPosition(
				SourceEntryForPending(origin, document->size));
		}
	}
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
		const auto info = QFileInfo(entry.saved_path);
		if (!info.isFile()) {
			continue;
		}
		// WHY: a pending download row points at a file still being
		// written; hashing it would pin a partial sha.
		if (entry.file_size > 0 && info.size() != entry.file_size) {
			continue;
		}
		HashStore::HashFileAsync(
			entry.saved_path,
			[=, path = entry.saved_path, origin = SourceOrigin{
				.peerId = entry.src_peer_id,
				.msgId = entry.src_msg_id,
				.title = entry.src_title,
				.tag = entry.src_tag,
				.attach = entry.src_attach,
			}](QByteArray sha256) mutable {
				FinishHashedRecord(
					entry.id,
					path,
					std::move(origin),
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

QString SourceTagForItem(HistoryItem *item) {
	return AugmentNameParts(item).tags.value(0);
}

QString AdjustDocumentSaveTarget(
		const QString &targetPath,
		not_null<DocumentData*> document,
		FullMsgId contextId) {
	if (!Enabled() || targetPath.isEmpty()) {
		return targetPath;
	}
	const auto item = contextId
		? document->owner().message(contextId)
		: nullptr;
	if (!item || !item->isRegular()) {
		return targetPath;
	}
	const auto info = QFileInfo(targetPath);
	const auto original = info.fileName();
	const auto dot = original.lastIndexOf(u'.');
	const auto stem = (dot > 0) ? original.left(dot) : original;
	if (!IsAllDigitsName(stem)) {
		return targetPath;
	}
	const auto candidate = BuildAugmentedName(original, AugmentNameParts(item));
	if (candidate.isEmpty()) {
		return targetPath;
	}
	const auto target = UniqueAugmentedTarget(info, candidate);
	LogFailure(u"save target augmented: %1 -> %2"_q.arg(original, target));
	return target;
}

// Retro-tag: hang already downloaded records of this message on a tag.
void ApplyRetroTag(
		not_null<Main::Session*> session,
		FullMsgId contextId,
		const QString &tag) {
	const auto item = session->data().message(contextId);
	const auto count = (item && item->isRegular())
		? Database::instance().RetagSource(
			item->history()->peer->id,
			item->id,
			tag)
		: 0;
	if (!count) {
		ShowToastText(u"No saved records for this message"_q);
		return;
	}
	Database::instance().RecordTag(tag);
	ShowSowTagToast(tag);
}

void AddRetroTagMenuActions(
		not_null<Ui::PopupMenu*> menu,
		HistoryItem *item) {
	if (!item || !item->isRegular()) {
		return;
	}
	const auto tags = Database::instance().TagSummaries(kMaxRetroTagChoices);
	if (tags.empty()) {
		return;
	}
	const auto session = &item->history()->session();
	const auto contextId = item->fullId();
	const auto anchor = menu->addAction(
		base::make_unique_q<Ui::Menu::Action>(
			menu->menu(),
			menu->menu()->st(),
			Ui::Menu::CreateAction(
				menu->menu().get(),
				u"Add to sow list"_q,
				nullptr),
			&st::menuIconDownload,
			&st::menuIconDownload));
	anchor->setMenu(new QMenu(menu->menu().get()));
	const auto sub = menu->ensureSubmenu(anchor, st::popupMenuWithIcons);
	for (const auto &summary : tags) {
		const auto tag = summary.tag;
		sub->addAction(
			u"#%1"_q.arg(tag),
			[=] { ApplyRetroTag(session, contextId, tag); },
			&st::menuIconDownload);
	}
}

void AddSowTagMenuActions(
		not_null<Ui::PopupMenu*> menu,
		HistoryItem *item,
		const QString &selectedText,
		const ClickHandlerPtr &link) {
	if (!Enabled() || !Database::instance().Enabled()) {
		return;
	}
	if (const auto selected = CleanSowTag(selectedText); !selected.isEmpty()) {
		menu->addAction(
			u"Save selected text to sow list"_q,
			[=] { SaveSowTag(selectedText); },
			&st::menuIconDownload);
	}
	// Q52.1: the hashtag under the cursor wins over the first one.
	const auto parts = AugmentNameParts(item);
	const auto cursorTag = TagFromLink(link);
	auto tags = parts.tags;
	if (!cursorTag.isEmpty() && !tags.contains(cursorTag)) {
		tags.push_front(cursorTag);
	}
	const auto direct = cursorTag.isEmpty()
		? (tags.isEmpty() ? QString() : tags.front())
		: cursorTag;
	if (!direct.isEmpty()) {
		menu->addAction(
			u"Save #%1 to sow list"_q.arg(direct),
			[=] { SaveSowTag(direct); },
			&st::menuIconDownload);
	}
	if (tags.size() > 1) {
		const auto anchor = menu->addAction(
			base::make_unique_q<Ui::Menu::Action>(
				menu->menu(),
				menu->menu()->st(),
				Ui::Menu::CreateAction(
					menu->menu().get(),
					u"Save hashtag to sow list"_q,
					nullptr),
				&st::menuIconDownload,
				&st::menuIconDownload));
		anchor->setMenu(new QMenu(menu->menu().get()));
		const auto sub = menu->ensureSubmenu(anchor, st::popupMenuWithIcons);
		for (const auto &tag : tags) {
			if (tag == direct) {
				continue;
			}
			sub->addAction(
				u"#%1"_q.arg(tag),
				[=] { SaveSowTag(tag); },
				&st::menuIconDownload);
		}
	}
	AddRetroTagMenuActions(menu, item);
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
