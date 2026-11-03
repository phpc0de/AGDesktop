// This is the source code of AyuGram for Desktop.
//
// We do not and cannot prevent the use of our code,
// but be respectful and credit the original author.
//
// Copyright @Radolyn, 2026
#include "ayu/sexcat/sexcat_folder_scan.h"

#include "ayu/sexcat/sexcat_database.h"
#include "ayu/sexcat/sexcat_hash_store.h"
#include "base/unixtime.h"
#include "data/data_msg_id.h"
#include "data/data_peer_id.h"

#include <QDir>
#include <QDirIterator>
#include <QFileInfo>

#include <memory>

namespace SexCat {
namespace {

constexpr auto kMaxHashBytes = qint64(512 * 1024 * 1024);

using Progress = Fn<void(const ScanSummary &)>;

// WHY: NTFS moves on the same volume carry the ADS marker along, so an
// already-marked file may still be the lost copy of a known record.
[[nodiscard]] bool RelinkLostEntry(
		const QString &path,
		const HashHit &hit,
		ScanSummary &summary) {
	const auto hits = Database::instance().LookupByHash(hit.sha256);
	const Entry *lost = nullptr;
	for (const auto &entry : hits) {
		if (!QFileInfo::exists(entry.saved_path)) {
			lost = &entry;
			break;
		}
	}
	if (!lost) {
		return false;
	}
	if (!Database::instance().Relink(lost->id, path)) {
		return false;
	}
	const auto peer = PeerId(uint64(lost->src_peer_id));
	const auto msg = MsgId(lost->src_msg_id);
	if (!HashStore::WriteAds(path, hit.sha256, peer, msg)) {
		HashStore::TrySidecar(path, hit.sha256, peer, msg);
	}
	summary.linked++;
	return true;
}

// Path+size dedupe keeps re-scans from duplicating rows.
[[nodiscard]] bool ImportLooseFile(
		const QString &path,
		const QByteArray &sha256,
		int64 size) {
	if (Database::instance().FindByPath(path, size)) {
		return false;
	}
	auto entry = Entry();
	entry.sha256 = sha256;
	entry.file_name = QFileInfo(path).fileName();
	entry.saved_path = path;
	entry.file_size = size;
	const auto modified = QFileInfo(path).lastModified();
	entry.saved_at = modified.isValid()
		? modified.toSecsSinceEpoch()
		: base::unixtime::now();
	return Database::instance().RecordSaved(std::move(entry));
}

void PumpNext(
		const std::shared_ptr<ScanSummary> &summary,
		const std::shared_ptr<QDirIterator> &iterator,
		const Progress &progress) {
	while (iterator->hasNext()) {
		if (summary->canceled) {
			summary->finished = true;
			progress(*summary);
			return;
		}
		const auto path = iterator->next();
		summary->scanned++;
		if (path.endsWith(u".sexcat.json"_q)) {
			summary->skipped++;
			continue;
		}
		if (const auto hit = HashStore::ReadAds(path)) {
			if (hit->sha256.size() == 32
				&& RelinkLostEntry(path, *hit, *summary)) {
				progress(*summary);
				continue;
			}
			summary->alreadyMarked++;
			progress(*summary);
			continue;
		}
		const auto size = QFileInfo(path).size();
		if (size <= 0 || size > kMaxHashBytes) {
			summary->failed++;
			progress(*summary);
			continue;
		}
		HashStore::HashFileAsync(path, [=](QByteArray sha256) {
			if (summary->canceled) {
				return;
			}
			if (sha256.isEmpty()) {
				summary->failed++;
			} else {
				summary->hashed++;
				const auto hits = Database::instance().LookupByHash(sha256);
				auto best = Entry();
				for (const auto &hit : hits) {
					if (best.saved_at < hit.saved_at) {
						best = hit;
					}
				}
				if (best.sha256.isEmpty()) {
					const auto imported = ImportLooseFile(
							path,
							sha256,
							size);
					if (!HashStore::WriteAds(
							path,
							sha256,
							PeerId(),
							MsgId())) {
						HashStore::TrySidecar(
							path,
							sha256,
							PeerId(),
							MsgId());
					}
					if (imported) {
						summary->imported++;
					} else {
						summary->marked++;
					}
				} else {
					const auto peer = PeerId(uint64(best.src_peer_id));
					const auto msg = MsgId(best.src_msg_id);
					if (!HashStore::WriteAds(path, sha256, peer, msg)) {
						HashStore::TrySidecar(path, sha256, peer, msg);
					}
					auto entry = Entry();
					entry.sha256 = sha256;
					entry.file_name = QFileInfo(path).fileName();
					entry.saved_path = path;
					entry.file_size = QFileInfo(path).size();
					entry.src_peer_id = best.src_peer_id;
					entry.src_msg_id = best.src_msg_id;
					entry.src_title = best.src_title;
					entry.src_attach = best.src_attach;
					entry.saved_at = base::unixtime::now();
					Database::instance().RecordSaved(std::move(entry));
					summary->linked++;
				}
			}
			progress(*summary);
			PumpNext(summary, iterator, progress);
		});
		return;
	}
	summary->finished = true;
	progress(*summary);
}

} // namespace

// WHY: HashFileAsync serializes on its own queue, so this pump keeps
// exactly one file hashing at a time and every progress call lands on
// the main thread, safe for direct UI updates.
std::shared_ptr<ScanSummary> StartFolderScan(
		const QString &root,
		Progress progress) {
	auto summary = std::make_shared<ScanSummary>();
	auto iterator = std::make_shared<QDirIterator>(
		root,
		QDir::Files,
		QDirIterator::Subdirectories);
	PumpNext(summary, iterator, progress);
	return summary;
}

} // namespace SexCat
