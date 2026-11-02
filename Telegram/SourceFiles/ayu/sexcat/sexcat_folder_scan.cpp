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

void PumpNext(
		const std::shared_ptr<ScanSummary> &summary,
		const std::shared_ptr<QDirIterator> &iterator,
		const Progress &progress) {
	while (iterator->hasNext()) {
		const auto path = iterator->next();
		summary->scanned++;
		if (HashStore::ReadAds(path)) {
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
					summary->marked++;
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
void StartFolderScan(const QString &root, Progress progress) {
	auto summary = std::make_shared<ScanSummary>();
	auto iterator = std::make_shared<QDirIterator>(
		root,
		QDir::Files,
		QDirIterator::Subdirectories);
	PumpNext(summary, iterator, progress);
}

} // namespace SexCat
