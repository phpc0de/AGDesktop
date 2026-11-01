// This is the source code of AyuGram for Desktop.
//
// We do not and cannot prevent the use of our code,
// but be respectful and credit the original author.
//
// Copyright @Radolyn, 2026
#include "ayu/sexcat/sexcat_history_window.h"

#include "ayu/sexcat/sexcat_database.h"
#include "ayu/sexcat/sexcat_hash_store.h"
#include "base/unixtime.h"
#include "core/application.h"
#include "core/file_utilities.h"
#include "data/data_peer.h"
#include "data/data_session.h"
#include "lang/lang_keys.h"
#include "main/main_account.h"
#include "main/main_domain.h"
#include "main/main_session.h"
#include "styles/style_boxes.h"
#include "styles/style_sexcat.h"
#include "styles/style_windows.h"
#include "ui/text/format_values.h"
#include "ui/toast/toast.h"
#include "window/window_controller.h"
#include "window/window_session_controller.h"

#include <QComboBox>
#include <QCryptographicHash>
#include <QDateTime>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLineEdit>
#include <QMenu>
#include <QSet>
#include <QPushButton>
#include <QSignalBlocker>
#include <QTreeWidget>
#include <QVBoxLayout>

namespace SexCat {
namespace {

constexpr auto kRecentLimit = 2000;
constexpr auto kHashReadChunkBytes = qint64(1024 * 1024);

constexpr auto kColTime = 0;
constexpr auto kColName = 1;
constexpr auto kColSource = 2;
constexpr auto kColPath = 3;
constexpr auto kColCount = 4;

[[nodiscard]] QByteArray ComputeFileSha256(const QString &path) {
	QFile file(path);
	if (!file.open(QIODevice::ReadOnly)) {
		return {};
	}
	QCryptographicHash hash(QCryptographicHash::Sha256);
	while (!file.atEnd()) {
		hash.addData(file.read(kHashReadChunkBytes));
	}
	return hash.result();
}

struct Row {
	uint64 id = 0;
	QString fileName;
	QString srcTitle;
	PeerId srcPeerId = 0;
	MsgId srcMsgId = 0;
	int srcAttach = 0;
	QString savedPath;
	qint64 fileSize = 0;
	QByteArray sha256;
	TimeId savedAt = 0;
	bool lost = false;
	bool hashMismatch = false;
	bool verifying = false;
};

[[nodiscard]] QString RowTimeText(const Row &row) {
	return langDateTime(QDateTime::fromSecsSinceEpoch(row.savedAt));
}

[[nodiscard]] QString RowPathText(const Row &row) {
	auto text = row.savedPath;
	if (row.lost) {
		text += u" — "_q + tr::lng_sexcat_lost(tr::now);
	}
	return text;
}

class HistoryWindow final : public QWidget {
public:
	HistoryWindow() {
		setAttribute(Qt::WA_DeleteOnClose);
		setWindowTitle(tr::lng_sexcat_history_title(tr::now));
		resize(
			st::sexcatHistoryWindowSize.width(),
			st::sexcatHistoryWindowSize.height());

		auto *root = new QVBoxLayout(this);
		auto *top = new QHBoxLayout();
		_filter = new QComboBox(this);
		_search = new QLineEdit(this);
		_search->setPlaceholderText(tr::lng_dlg_filter(tr::now));
		auto *verify = new QPushButton(
			tr::lng_sexcat_verify(tr::now),
			this);
		top->addWidget(_filter, 1);
		top->addWidget(_search, 2);
		top->addWidget(verify);
		root->addLayout(top);

		_tree = new QTreeWidget(this);
		_tree->setColumnCount(kColCount);
		_tree->setHeaderLabels({
			tr::lng_sexcat_col_time(tr::now),
			tr::lng_sexcat_col_name(tr::now),
			tr::lng_sexcat_col_source(tr::now),
			tr::lng_sexcat_col_path(tr::now),
		});
		_tree->setRootIsDecorated(false);
		_tree->setUniformRowHeights(true);
		_tree->setAllColumnsShowFocus(true);
		_tree->setSelectionBehavior(QAbstractItemView::SelectRows);
		_tree->setSelectionMode(QAbstractItemView::ExtendedSelection);
		_tree->setContextMenuPolicy(Qt::CustomContextMenu);
		auto *header = _tree->header();
		header->setSectionResizeMode(kColTime, QHeaderView::ResizeToContents);
		header->setSectionResizeMode(kColName, QHeaderView::ResizeToContents);
		header->setSectionResizeMode(
			kColSource,
			QHeaderView::ResizeToContents);
		header->setSectionResizeMode(kColPath, QHeaderView::Stretch);
		root->addWidget(_tree, 1);

		connect(this, &QObject::destroyed, [] {
			Instance() = nullptr;
		});
		connect(_search, &QLineEdit::textChanged, this, [=] {
			applyFilter();
		});
		connect(
			_filter,
			&QComboBox::currentIndexChanged,
			this,
			[=](int) { applyFilter(); });
		connect(verify, &QPushButton::clicked, this, [=] {
			verifySelected();
		});
		connect(_tree, &QTreeWidget::itemDoubleClicked, this, [=](
				QTreeWidgetItem *item,
				int) { openFolder(RowIdOf(item)); });
		connect(_tree, &QTreeWidget::itemClicked, this, [=](
				QTreeWidgetItem *item,
				int column) {
			if (column == kColSource) {
				jumpToSource(RowIdOf(item));
			}
		});
		connect(_tree, &QWidget::customContextMenuRequested, this, [=](
				const QPoint &pos) { showContextMenu(pos); });

		reload();
	}

	static void ShowExisting() {
		if (!Instance()) {
			Instance() = new HistoryWindow();
		}
		Instance()->show();
		Instance()->raise();
		Instance()->activateWindow();
	}

private:
	[[nodiscard]] static HistoryWindow *&Instance() {
		static HistoryWindow *single = nullptr;
		return single;
	}

	[[nodiscard]] static uint64 RowIdOf(QTreeWidgetItem *item) {
		return item ? item->data(kColTime, Qt::UserRole).toULongLong() : 0;
	}

	[[nodiscard]] Row *findRow(uint64 id) {
		for (auto &row : _rows) {
			if (row.id == id) {
				return &row;
			}
		}
		return nullptr;
	}

	void reload() {
		auto rows = std::vector<Row>();
		for (const auto &entry : Database::instance().Recent(kRecentLimit, {})) {
			auto row = Row{
				.id = uint64(entry.id),
				.fileName = entry.file_name,
				.srcTitle = entry.src_title,
				.srcPeerId = PeerId(uint64(entry.src_peer_id)),
				.srcMsgId = MsgId(entry.src_msg_id),
				.srcAttach = entry.src_attach,
				.savedPath = entry.saved_path,
				.fileSize = entry.file_size,
				.sha256 = entry.sha256,
				.savedAt = TimeId(entry.saved_at),
			};
			row.lost = !QFileInfo::exists(row.savedPath);
			rows.push_back(std::move(row));
		}
		_rows = std::move(rows);
		rebuildFilterBox();
		applyFilter();
	}

	void rebuildFilterBox() {
		_filterPeerIds.clear();
		_filterPeerIds.push_back(PeerId(0));
		QSignalBlocker blocker(_filter);
		_filter->clear();
		_filter->addItem(tr::lng_sexcat_filter_all(tr::now));
		QSet<QString> seen;
		for (const auto &row : _rows) {
			if (row.srcTitle.isEmpty() || seen.contains(row.srcTitle)) {
				continue;
			}
			seen.insert(row.srcTitle);
			_filterPeerIds.push_back(row.srcPeerId);
			_filter->addItem(row.srcTitle);
		}
	}

	[[nodiscard]] PeerId filterPeerId() const {
		const auto index = _filter->currentIndex();
		return (index >= 0 && index < _filterPeerIds.size())
			? _filterPeerIds[index]
			: PeerId(0);
	}

	void applyFilter() {
		const auto query = _search->text().trimmed().toLower();
		const auto peerId = filterPeerId();
		_tree->clear();
		for (const auto &row : _rows) {
			if (peerId != PeerId(0) && row.srcPeerId != peerId) {
				continue;
			}
			if (!query.isEmpty()
				&& !row.fileName.toLower().contains(query)
				&& !row.srcTitle.toLower().contains(query)) {
				continue;
			}
			auto *item = new QTreeWidgetItem(_tree);
			fillItem(item, row);
		}
	}

	void fillItem(QTreeWidgetItem *item, const Row &row) {
		item->setData(
			kColTime,
			Qt::UserRole,
			QVariant::fromValue<qulonglong>(row.id));
		item->setText(kColTime, RowTimeText(row));
		item->setText(kColName, row.fileName);
		item->setToolTip(
			kColName,
			row.fileName + u" ("_q + Ui::FormatSizeText(row.fileSize)
				+ u")"_q);
		item->setText(
			kColSource,
			row.srcTitle.isEmpty() ? u"—"_q : row.srcTitle);
		item->setToolTip(
			kColSource,
			QString::number(row.srcPeerId.value));
		item->setText(kColPath, RowPathText(row));
		item->setToolTip(kColPath, row.savedPath);
		if (row.verifying) {
			item->setText(kColPath, tr::lng_sexcat_hashing(tr::now));
		}
		const auto lostFg = QBrush(st::windowSubTextFg->c);
		const auto errorFg = QBrush(st::boxTextFgError->c);
		auto fg = QBrush();
		if (row.hashMismatch) {
			fg = errorFg;
		} else if (row.lost) {
			fg = lostFg;
		}
		for (auto column = 0; column != kColCount; ++column) {
			item->setForeground(column, fg);
		}
	}

	void refreshRow(uint64 id) {
		const auto row = findRow(id);
		if (!row) {
			return;
		}
		for (auto i = 0; i != _tree->topLevelItemCount(); ++i) {
			auto *item = _tree->topLevelItem(i);
			if (RowIdOf(item) == id) {
				fillItem(item, *row);
				return;
			}
		}
	}

	void verifySelected() {
		for (auto *item : _tree->selectedItems()) {
			if (const auto row = findRow(RowIdOf(item))) {
				startVerification(row->id);
			}
		}
	}

	void startVerification(uint64 id) {
		const auto row = findRow(id);
		if (!row || row->verifying) {
			return;
		}
		if (!QFileInfo::exists(row->savedPath)) {
			row->lost = true;
			refreshRow(id);
			return;
		}
		row->verifying = true;
		refreshRow(id);
		const auto path = row->savedPath;
		crl::async([path, weak = QPointer<HistoryWindow>(this), id] {
			auto sha = ComputeFileSha256(path);
			crl::on_main([weak, id, sha = std::move(sha)] {
				if (weak) {
					weak->finishVerification(id, sha);
				}
			});
		});
	}

	void finishVerification(uint64 id, const QByteArray &sha) {
		const auto row = findRow(id);
		if (!row) {
			return;
		}
		row->verifying = false;
		row->hashMismatch = (sha.isEmpty() || row->sha256 != sha);
		row->lost = row->hashMismatch ? row->lost : false;
		refreshRow(id);
		Ui::Toast::Show(row->hashMismatch
			? tr::lng_sexcat_verify_failed(tr::now)
			: tr::lng_sexcat_verify_ok(tr::now));
	}

	void relocateRow(uint64 id) {
		const auto row = findRow(id);
		if (!row) {
			return;
		}
		const auto oldDir = QFileInfo(row->savedPath).absolutePath();
		const auto file = QFileDialog::getOpenFileName(
			this,
			tr::lng_sexcat_relocate(tr::now),
			QFileInfo::exists(oldDir) ? oldDir : QString());
		if (file.isEmpty() || file == row->savedPath) {
			return;
		}
		row->verifying = true;
		refreshRow(id);
		crl::async([file, weak = QPointer<HistoryWindow>(this), id] {
			auto sha = ComputeFileSha256(file);
			crl::on_main([weak, id, file, sha = std::move(sha)] {
				if (weak) {
					weak->finishRelocation(id, file, sha);
				}
			});
		});
	}

	void finishRelocation(uint64 id, const QString &file, const QByteArray &sha) {
		const auto row = findRow(id);
		if (!row) {
			return;
		}
		row->verifying = false;
		if (sha.isEmpty() || row->sha256 != sha) {
			row->hashMismatch = true;
			refreshRow(id);
			Ui::Toast::Show(tr::lng_sexcat_verify_failed(tr::now));
			return;
		}
		// WHY: the ADS marker is the zero-cost lookup anchor after a move;
		// the sidecar covers filesystems where xattrs streams don't exist,
		// and the newest record carries the current "last valid path".
		if (!HashStore::WriteAds(file, row->sha256, row->srcPeerId, row->srcMsgId)) {
			HashStore::TrySidecar(file, row->sha256, row->srcPeerId, row->srcMsgId);
		}
		auto updated = SexCat::Entry{
			.id = 0,
			.sha256 = row->sha256,
			.file_name = row->fileName,
			.saved_path = file,
			.file_size = row->fileSize,
			.src_peer_id = int64(row->srcPeerId.value),
			.src_msg_id = row->srcMsgId.bare,
			.src_title = row->srcTitle,
			.src_attach = row->srcAttach,
			.saved_at = base::unixtime::now(),
		};
		if (!Database::instance().RecordSaved(std::move(updated))) {
			Ui::Toast::Show(tr::lng_sexcat_verify_failed(tr::now));
			return;
		}
		row->savedPath = file;
		row->lost = false;
		row->hashMismatch = false;
		reload();
		Ui::Toast::Show(tr::lng_sexcat_verify_ok(tr::now));
	}

	void openFolder(uint64 id) {
		const auto row = findRow(id);
		if (!row) {
			return;
		}
		if (!QFileInfo::exists(row->savedPath)) {
			row->lost = true;
			refreshRow(id);
			Ui::Toast::Show(tr::lng_sexcat_lost(tr::now));
			return;
		}
		File::ShowInFolder(row->savedPath);
	}

	void jumpToSource(uint64 id) {
		const auto row = findRow(id);
		if (!row) {
			return;
		}
		Main::Session *found = nullptr;
		for (const auto &account : Core::App().domain().orderedAccounts()) {
			const auto session = account->maybeSession();
			if (session && session->data().peerLoaded(row->srcPeerId)) {
				found = session;
				break;
			}
		}
		if (!found) {
			Ui::Toast::Show(tr::lng_sexcat_source_gone(tr::now));
			return;
		}
		const auto peer = found->data().peer(row->srcPeerId);
		if (!peer) {
			return;
		}
		const auto separate = Core::App().separateWindowFor(peer);
		const auto controller = separate
			? separate->sessionController()
			: peer->session().tryResolveWindow(peer);
		if (!controller) {
			return;
		}
		auto params = Window::SectionShow{
			Window::SectionShow::Way::Forward,
		};
		params.allowDuplicateInStack = true;
		if (const auto item = found->data().message(
				row->srcPeerId,
				row->srcMsgId)) {
			controller->showMessage(item, params);
		} else {
			controller->showPeerHistory(peer, params, row->srcMsgId);
		}
	}

	void showContextMenu(const QPoint &pos) {
		auto *item = _tree->itemAt(pos);
		const auto row = item ? findRow(RowIdOf(item)) : nullptr;
		if (!row) {
			return;
		}
		const auto id = row->id;
		auto menu = new QMenu(this);
		menu->setAttribute(Qt::WA_DeleteOnClose);
		menu->addAction(tr::lng_sexcat_open_folder(tr::now), [=] {
			openFolder(id);
		});
		menu->addAction(tr::lng_sexcat_relocate(tr::now), [=] {
			relocateRow(id);
		});
		menu->addAction(tr::lng_sexcat_verify(tr::now), [=] {
			startVerification(id);
		});
		menu->popup(_tree->viewport()->mapToGlobal(pos));
	}

	QComboBox *_filter = nullptr;
	QLineEdit *_search = nullptr;
	QTreeWidget *_tree = nullptr;
	std::vector<Row> _rows;
	std::vector<PeerId> _filterPeerIds;

};

} // namespace

void OpenHistoryWindow() {
	HistoryWindow::ShowExisting();
}

void RequestReverify(const QString &path) {
	if (path.isEmpty()) {
		return;
	}
	crl::async([path] {
		auto sha = ComputeFileSha256(path);
		crl::on_main([path, sha = std::move(sha)] {
			if (!QFileInfo::exists(path) || sha.isEmpty()) {
				Ui::Toast::Show(tr::lng_sexcat_lost(tr::now));
				return;
			}
			Ui::Toast::Show(Database::instance().LookupByHash(sha).empty()
				? tr::lng_sexcat_not_recorded(tr::now)
				: tr::lng_sexcat_verify_ok(tr::now));
		});
	});
}

} // namespace SexCat
