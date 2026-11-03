// This is the source code of [R:67494147] for Desktop.
//
// We do not and cannot prevent the use of our code,
// but be respectful and credit the original author.
//
// Copyright @Radolyn, 2026
#include "ayu/sexcat/sexcat_shell_watch.h"

#include "ayu/sexcat/sexcat_database.h"
#include "ayu/sexcat/sexcat_hash_store.h"
#include "ayu/utils/ayu_log.h"
#include "data/data_msg_id.h"
#include "data/data_peer_id.h"

#include <QtWidgets/QWidget>

#include <QDir>
#include <QFileInfo>

#include <array>
#include <exception>

#ifdef Q_OS_WIN
#include <windows.h>
#include <shlobj.h>
#endif // Q_OS_WIN

namespace SexCat {
namespace {

#ifdef Q_OS_WIN

void LogFailure(const QString &message) {
	Ayu::Log::Write(u"sexcat"_q, message);
}

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

void HandleUpdate(const QString &path) {
	if (!QFileInfo(path).isFile()) {
		return;
	}
	const auto hit = HashStore::ReadAds(path);
	if (hit && hit->sha256.size() == 32) {
		RelinkLostByAds(path, *hit);
	}
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
	} else if (event == SHCNE_UPDATEITEM) {
		if (!path.isEmpty()) {
			HandleUpdate(path);
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
			} else if (event == SHCNE_UPDATEITEM) {
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
	auto pidl = static_cast<PIDLIST_ABSOLUTE>(nullptr);
	if (FAILED(SHGetKnownFolderIDList(FOLDERID_Desktop, 0, nullptr, &pidl))) {
		LogFailure(u"shell watch failed to resolve the desktop pidl"_q);
		return;
	}
	auto entry = SHChangeNotifyEntry();
	entry.pidl = pidl;
	entry.fRecursive = TRUE;
	const auto registered = SHChangeNotifyRegister(
		reinterpret_cast<HWND>(window->winId()),
		SHCNRF_NewDelivery | SHCNRF_InterruptLevel | SHCNRF_ShellLevel,
		SHCNE_RENAMEITEM | SHCNE_RENAMEFOLDER | SHCNE_UPDATEITEM,
		kShellChangeMessage,
		1,
		&entry);
	ILFree(pidl);
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
