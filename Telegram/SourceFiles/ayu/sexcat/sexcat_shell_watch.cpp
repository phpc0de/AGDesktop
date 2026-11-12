// This is the source code of [R:67494147] for Desktop.
//
// We do not and cannot prevent the use of our code,
// but be respectful and credit the original author.
//
// Copyright @Radolyn, 2026
#include "ayu/sexcat/sexcat_shell_watch.h"

#include "ayu/sexcat/sexcat_database.h"
#include "ayu/sexcat/sexcat_hash_store.h"
#include "ayu/ui/toasts.h"
#include "ayu/utils/ayu_log.h"
#include "core/application.h"
#include "core/core_settings.h"
#include "data/data_msg_id.h"
#include "data/data_peer_id.h"
#include "lang/lang_keys.h"

#include <QtWidgets/QWidget>

#include <QtCore/QStandardPaths>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>

#include <crl/crl.h>

#include <array>
#include <exception>
#include <map>
#include <optional>
#include <set>
#include <vector>

#ifdef Q_OS_WIN
#include <windows.h>
#include <shlobj.h>
#endif // Q_OS_WIN

namespace SexCat {
namespace {

void LogFailure(const QString &message) {
	Ayu::Log::Write(u"sexcat"_q, message);
}

constexpr auto kRelocateFiles = 20000;
constexpr auto kRelocateDepth = 4;
constexpr auto kRelocateScanLimit = 2000;
constexpr auto kRelocateMs = crl::time(2000);
constexpr auto kManualRelocateFiles = 20000;
constexpr auto kManualRelocateDepth = 4;
constexpr auto kManualRelocateMs = crl::time(10'000);
constexpr auto kRescueHashCapBytes = qint64(512 * 1024 * 1024);
constexpr auto kRescueCooldownMs = crl::time(5000);

struct RelocateBudget {
	int filesLeft = 0;
	int depthLeft = 0;
	crl::time deadline = 0;
};

[[nodiscard]] QString DriveRoot(const QString &path) {
	const auto absolute = QFileInfo(path).absoluteFilePath();
	const auto index = absolute.indexOf('/');
	return (index > 0) ? absolute.left(index + 1) : QString();
}

[[nodiscard]] bool SameDrive(const QString &path, const QString &drive) {
	return !drive.isEmpty()
		&& DriveRoot(path).compare(drive, Qt::CaseInsensitive) == 0;
}

void ScanForMatch(
		const QString &root,
		const QString &name,
		int64 size,
		RelocateBudget &budget,
		QString &found) {
	if (!found.isEmpty()
		|| budget.filesLeft <= 0
		|| budget.depthLeft <= 0
		|| crl::now() >= budget.deadline) {
		return;
	}
	auto files = QDirIterator(root, QDir::Files | QDir::NoDotAndDotDot);
	while (files.hasNext()) {
		const auto path = files.next();
		--budget.filesLeft;
		const auto info = QFileInfo(path);
		if (info.fileName().compare(name, Qt::CaseInsensitive) == 0
			&& info.size() == size) {
			found = path;
			return;
		}
		if (budget.filesLeft <= 0 || crl::now() >= budget.deadline) {
			return;
		}
	}
	auto folders = QDirIterator(
		root,
		QDir::Dirs | QDir::NoDotAndDotDot | QDir::NoSymLinks);
	while (folders.hasNext()) {
		const auto path = folders.next();
		--budget.depthLeft;
		ScanForMatch(path, name, size, budget, found);
		++budget.depthLeft;
		if (!found.isEmpty()
			|| budget.filesLeft <= 0
			|| crl::now() >= budget.deadline) {
			return;
		}
	}
}

void AppendCandidate(
		std::vector<QString> &roots,
		const QString &drive,
		const QString &path) {
	const auto absolute = QFileInfo(path).absoluteFilePath();
	if (absolute.isEmpty()
		|| !SameDrive(absolute, drive)
		|| !QFileInfo(absolute).isDir()) {
		return;
	}
	for (const auto &root : roots) {
		if (root.compare(absolute, Qt::CaseInsensitive) == 0) {
			return;
		}
	}
	roots.push_back(absolute);
}

std::vector<QString> RelocateCandidates(const QString &oldPath) {
	auto roots = std::vector<QString>();
	const auto drive = DriveRoot(oldPath);
	const auto oldDir = QFileInfo(oldPath).absolutePath();
	AppendCandidate(roots, drive, QFileInfo(oldDir).absolutePath());
	const auto desktop = QStandardPaths::writableLocation(
		QStandardPaths::DesktopLocation);
	if (!desktop.isEmpty()) {
		const auto profile = QFileInfo(desktop).absolutePath();
		for (const auto &info
			: QDir(profile).entryInfoList(
				QDir::Dirs | QDir::NoDotAndDotDot | QDir::NoSymLinks)) {
			AppendCandidate(roots, drive, info.absoluteFilePath());
		}
		AppendCandidate(roots, drive, desktop);
	}
	AppendCandidate(
		roots,
		drive,
		QStandardPaths::writableLocation(QStandardPaths::DownloadLocation));
	AppendCandidate(
		roots,
		drive,
		QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation));
	for (const auto &entry : Database::instance().Recent(kRelocateScanLimit, {})) {
		AppendCandidate(
			roots,
			drive,
			QFileInfo(entry.saved_path).absolutePath());
	}
	return roots;
}

struct MissingIndex {
	std::map<int64, std::vector<int64>> bySize;
	std::map<int64, Entry> entries;
};

// WHY: the ADS marker rides along with same-volume moves, so a stream
// read identifies a relocated file without hashing; unmarked files of a
// matching size fall back to a bounded full hash.
void ScanForMissing(
		const QString &root,
		const MissingIndex &index,
		RelocateBudget &budget,
		std::map<int64, QString> &hits) {
	if (budget.filesLeft <= 0
		|| budget.depthLeft <= 0
		|| crl::now() >= budget.deadline) {
		return;
	}
	auto files = QDirIterator(root, QDir::Files | QDir::NoDotAndDotDot);
	while (files.hasNext()) {
		const auto path = files.next();
		--budget.filesLeft;
		const auto info = QFileInfo(path);
		const auto it = index.bySize.find(info.size());
		if (it != end(index.bySize)) {
			for (const auto id : it->second) {
				if (hits.contains(id)) {
					continue;
				}
				const auto &entry = index.entries.at(id);
				auto matched = info.fileName().compare(
					entry.file_name,
					Qt::CaseInsensitive) == 0;
				if (!matched && entry.sha256.size() == 32) {
					const auto marker = HashStore::ReadAds(path);
					matched = marker && marker->sha256 == entry.sha256;
					if (!matched && info.size() <= kRescueHashCapBytes) {
						matched = HashStore::ComputeFileSha256(path)
							== entry.sha256;
					}
				}
				if (matched) {
					hits.emplace(id, path);
					break;
				}
			}
		}
		if (budget.filesLeft <= 0 || crl::now() >= budget.deadline) {
			return;
		}
	}
	auto folders = QDirIterator(
		root,
		QDir::Dirs | QDir::NoDotAndDotDot | QDir::NoSymLinks);
	while (folders.hasNext()) {
		const auto path = folders.next();
		--budget.depthLeft;
		ScanForMissing(path, index, budget, hits);
		++budget.depthLeft;
		if (budget.filesLeft <= 0
			|| budget.depthLeft <= 0
			|| crl::now() >= budget.deadline) {
			return;
		}
	}
}

} // namespace

void RelinkMovedEntry(const Entry &entry, const QString &newPath) {
	if (newPath.isEmpty() || !Database::instance().Relink(entry.id, newPath)) {
		return;
	}
	if (entry.sha256.size() == 32) {
		const auto peer = PeerId(uint64(entry.src_peer_id));
		const auto msg = MsgId(entry.src_msg_id);
		if (!HashStore::WriteAds(newPath, entry.sha256, peer, msg)) {
			HashStore::TrySidecar(newPath, entry.sha256, peer, msg);
		}
	}
	if (QFileInfo::exists(entry.saved_path + u".sexcat.json"_q)) {
		QFile::remove(newPath + u".sexcat.json"_q);
		QFile::rename(
			entry.saved_path + u".sexcat.json"_q,
			newPath + u".sexcat.json"_q);
	}
	LogFailure(
		u"shell watch relocated %1 -> %2"_q.arg(entry.saved_path, newPath));
}

void RelocateMissingEntries() {
	auto missing = std::vector<Entry>();
	for (const auto &entry
		: Database::instance().Recent(kRelocateScanLimit, {})) {
		if (!entry.saved_path.isEmpty()
			&& !QFileInfo::exists(entry.saved_path)) {
			missing.push_back(entry);
		}
	}
	if (missing.empty()) {
		return;
	}
	auto index = MissingIndex();
	for (const auto &entry : missing) {
		index.entries.emplace(entry.id, entry);
		index.bySize[entry.file_size].push_back(entry.id);
	}
	auto roots = std::vector<QString>();
	auto seenRoots = std::set<QString>();
	const auto addRoot = [&](const QString &path) {
		const auto absolute = QFileInfo(path).absoluteFilePath();
		if (absolute.isEmpty() || !QFileInfo(absolute).isDir()) {
			return;
		}
		if (seenRoots.insert(absolute.toLower()).second) {
			roots.push_back(absolute);
		}
	};
	for (const auto &entry : missing) {
		for (const auto &root : RelocateCandidates(entry.saved_path)) {
			addRoot(root);
		}
		// last resort: a bounded walk from the drive root catches moves
		// to folders no other entry has ever touched
		addRoot(DriveRoot(entry.saved_path));
	}
	crl::async([index = std::move(index), roots = std::move(roots)] {
		auto budget = RelocateBudget{
			.filesLeft = kManualRelocateFiles,
			.depthLeft = kManualRelocateDepth,
			.deadline = crl::now() + kManualRelocateMs,
		};
		auto hits = std::map<int64, QString>();
		for (const auto &root : roots) {
			ScanForMissing(root, index, budget, hits);
			if (budget.filesLeft <= 0 || crl::now() >= budget.deadline) {
				break;
			}
		}
		crl::on_main([hits = std::move(hits)] {
			auto relinked = 0;
			for (const auto &[id, path] : hits) {
				if (Database::instance().Relink(id, path)) {
					++relinked;
				}
			}
			if (relinked > 0) {
				Ui::Toast::Show(tr::lng_sexcat_autorelocate_done(
					tr::now,
					lt_count,
					QString::number(relinked)));
			}
		});
	});
}

std::optional<QString> LocateByNameSize(
		const QString &root,
		const QString &fileName,
		int64 fileSize) {
	if (root.isEmpty() || fileName.isEmpty() || fileSize <= 0) {
		return std::nullopt;
	}
	auto found = QString();
	auto budget = RelocateBudget{
		.filesLeft = kManualRelocateFiles,
		.depthLeft = kManualRelocateDepth,
		.deadline = crl::now() + kManualRelocateMs,
	};
	ScanForMatch(root, fileName, fileSize, budget, found);
	if (found.isEmpty()) {
		return std::nullopt;
	}
	return found;
}

namespace {

#ifdef Q_OS_WIN

constexpr auto kShellChangeMessage = WM_APP + 0x0351;

bool RelinkLostByAds(const QString &path, const HashHit &hit) {
	const auto hits = Database::instance().LookupByHash(hit.sha256);
	for (const auto &entry : hits) {
		if (QFileInfo::exists(entry.saved_path)) {
			continue;
		}
		if (!Database::instance().Relink(entry.id, path)) {
			return false;
		}
		HashStore::WriteAds(
			path,
			hit.sha256,
			PeerId(uint64(entry.src_peer_id)),
			MsgId(entry.src_msg_id));
		LogFailure(u"shell watch relinked lost copy: %1"_q.arg(path));
		return true;
	}
	return false;
}

void HandleRename(const QString &oldPath, const QString &newPath) {
	if (auto entry = Database::instance().LookupByPath(oldPath)) {
		if (Database::instance().Relink(entry->id, newPath)) {
			HashStore::WriteAds(
				newPath,
				entry->sha256,
				PeerId(uint64(entry->src_peer_id)),
				MsgId(entry->src_msg_id));
				QFile::remove(newPath + u".sexcat.json"_q);
			QFile::rename(
				oldPath + u".sexcat.json"_q,
				newPath + u".sexcat.json"_q);
			LogFailure(u"shell watch relinked %1 -> %2"_q.arg(oldPath, newPath));
		}
		return;
	}
	if (QFileInfo(newPath).isFile()) {
		const auto hit = HashStore::ReadAds(newPath);
		if (hit && hit->sha256.size() == 32) {
			RelinkLostByAds(newPath, *hit);
		}
	}
}

void HandleRenameFolder(const QString &oldDir, const QString &newDir) {
	auto oldPrefix = oldDir;
	if (!oldPrefix.endsWith('/')) {
		oldPrefix += '/';
	}
	auto newPrefix = newDir;
	if (!newPrefix.endsWith('/')) {
		newPrefix += '/';
	}
	auto relinked = 0;
	for (const auto &entry : Database::instance().LookupByPathPrefix(oldPrefix)) {
		if (!entry.saved_path.startsWith(oldPrefix)) {
			continue;
		}
		if (Database::instance().Relink(
				entry.id,
				newPrefix + entry.saved_path.mid(oldPrefix.size()))) {
			relinked++;
		}
	}
	if (relinked > 0) {
		LogFailure(u"shell watch relinked folder %1 -> %2 (%3)"_q
			.arg(oldDir, newDir)
			.arg(relinked));
	}
}

crl::time &LastRescueAt() {
	static crl::time value = 0;
	return value;
}

void HandleDeleted(const QString &path) {
	if (path.isEmpty() || QFileInfo::exists(path)) {
		return;
	}
	if (!Database::instance().LookupByPath(path)) {
		return;
	}
	// WHY: a move out of the watched tree arrives as a bare delete, so
	// likely destinations are searched before the row is left missing.
	auto &last = LastRescueAt();
	const auto now = crl::now();
	if (now - last < kRescueCooldownMs) {
		return;
	}
	last = now;
	RelocateMissingEntries();
}

void ProcessChangeEvent(
		LONG event,
		const QString &oldPath,
		const QString &newPath,
		const QString &path) {
	if (event == SHCNE_RENAMEITEM) {
		if (!oldPath.isEmpty() && !newPath.isEmpty()) {
			HandleRename(oldPath, newPath);
		}
	} else if (event == SHCNE_RENAMEFOLDER) {
		if (!oldPath.isEmpty() && !newPath.isEmpty()) {
			HandleRenameFolder(oldPath, newPath);
		}
	} else if (event == SHCNE_DELETE) {
		if (!path.isEmpty()) {
			HandleDeleted(path);
		}
	}
}

class ShellWatchWindow final : public QWidget {
public:
	ShellWatchWindow() {
		winId(); // forces the native handle before registration
	}

protected:
	bool nativeEvent(
		const QByteArray &eventType,
		void *message,
#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
		long *result) override {
#else // Qt < 6
		qintptr *result) override {
#endif // Qt < 6
		const auto msg = static_cast<MSG*>(message);
		if (!msg || msg->message != kShellChangeMessage) {
			return QWidget::nativeEvent(eventType, message, result);
		}
		auto pidls = static_cast<PIDLIST_ABSOLUTE *>(nullptr);
		auto event = LONG();
		const auto lock = SHChangeNotification_Lock(
			reinterpret_cast<HANDLE>(msg->wParam),
			static_cast<DWORD>(msg->lParam),
			&pidls,
			&event);
		auto oldPath = QString();
		auto newPath = QString();
		auto path = QString();
		if (lock) {
			// WHY: only cheap PIDL decoding may run under the shell lock,
			// database and file work must happen after it is released.
			const auto read = [&](PIDLIST_ABSOLUTE pidl) {
				auto buffer = std::array<WCHAR, MAX_PATH>();
				return (pidl && SHGetPathFromIDListW(pidl, buffer.data()))
					? QDir::fromNativeSeparators(
						QString::fromWCharArray(buffer.data()))
					: QString();
			};
			if (event == SHCNE_RENAMEITEM || event == SHCNE_RENAMEFOLDER) {
				oldPath = read(pidls ? pidls[0] : nullptr);
				newPath = read(pidls ? pidls[1] : nullptr);
			} else if (event == SHCNE_DELETE) {
				path = read(pidls ? pidls[0] : nullptr);
			}
			SHChangeNotification_Unlock(lock);
		}
		try {
			ProcessChangeEvent(event, oldPath, newPath, path);
		} catch (const std::exception &ex) {
			LogFailure(u"shell watch failed: %1"_q.arg(QString::fromUtf8(ex.what())));
		} catch (...) {
			LogFailure(u"shell watch failed: unknown error"_q);
		}
		*result = 0;
		return true;
	}
};

void RegisterShellWatch() {
	auto window = new ShellWatchWindow();
	// WHY: CSIDL_DESKTOP is the shell namespace root, so one recursive
	// registration delivers renames, moves and deletes for every drive,
	// not just the few folders the old entries covered.
	auto desktopPidl = static_cast<PIDLIST_ABSOLUTE>(nullptr);
	if (FAILED(
			SHGetFolderLocation(
				nullptr,
				CSIDL_DESKTOP,
				nullptr,
				0,
				&desktopPidl))) {
		LogFailure(u"shell watch failed to resolve the desktop pidl"_q);
		return;
	}
	auto entry = SHChangeNotifyEntry();
	entry.pidl = desktopPidl;
	entry.fRecursive = TRUE;
	const auto registered = SHChangeNotifyRegister(
		reinterpret_cast<HWND>(window->winId()),
		SHCNRF_NewDelivery | SHCNRF_InterruptLevel | SHCNRF_ShellLevel,
		SHCNE_RENAMEITEM | SHCNE_RENAMEFOLDER | SHCNE_DELETE,
		kShellChangeMessage,
		1,
		&entry);
	ILFree(desktopPidl);
	if (!registered) {
		LogFailure(u"shell watch failed to register change notifications"_q);
	}
}

#endif // Q_OS_WIN

} // namespace

void StartShellWatch() {
	static bool started = false;
	if (started) {
		return;
	}
	started = true;
#ifdef Q_OS_WIN
	RegisterShellWatch();
#endif // Q_OS_WIN
}

} // namespace SexCat
