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

constexpr auto kShellChangeMessage = WM_APP + 0x7351;

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

void HandleUpdate(const QString &path) {
	if (!QFileInfo(path).isFile()) {
		return;
	}
	const auto hit = HashStore::ReadAds(path);
	if (hit && hit->sha256.size() == 32) {
		RelinkLostByAds(path, *hit);
	}
}

void ProcessChangeEvent(LONG event, PIDLIST_ABSOLUTE *pidls) {
	if (event == SHCNE_RENAMEITEM) {
		auto oldPath = std::array<WCHAR, MAX_PATH>();
		auto newPath = std::array<WCHAR, MAX_PATH>();
		const auto oldOk = pidls[0]
			&& SHGetPathFromIDListW(pidls[0], oldPath.data());
		const auto newOk = pidls[1]
			&& SHGetPathFromIDListW(pidls[1], newPath.data());
		if (oldOk && newOk) {
			HandleRename(
				QString::fromWCharArray(oldPath.data()),
				QString::fromWCharArray(newPath.data()));
		}
	} else if (event == SHCNE_UPDATEITEM) {
		auto path = std::array<WCHAR, MAX_PATH>();
		if (pidls[0] && SHGetPathFromIDListW(pidls[0], path.data())) {
			HandleUpdate(QString::fromWCharArray(path.data()));
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
		if (lock) {
			try {
				ProcessChangeEvent(event, pidls);
			} catch (const std::exception &ex) {
				LogFailure(u"shell watch failed: %1"_q.arg(QString::fromUtf8(ex.what())));
			} catch (...) {
				LogFailure(u"shell watch failed: unknown error"_q);
			}
			SHChangeNotification_Unlock(lock);
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
		SHCNE_RENAMEITEM | SHCNE_UPDATEITEM,
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
