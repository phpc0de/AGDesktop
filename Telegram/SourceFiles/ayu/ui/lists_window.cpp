// This is the source code of [R:67494147] for Desktop.
//
// We do not and cannot prevent the use of our code,
// but be respectful and credit the original author.
//
// Copyright @Radolyn, 2026
#include "ayu/ui/lists_window.h"

#include "ayu/data/ayu_database.h"
#include "ayu/sexcat/sexcat_database.h"
#include "core/application.h"
#include "data/data_forum.h"
#include "data/data_peer.h"
#include "data/data_peer_id.h"
#include "data/data_session.h"
#include "lang/lang_keys.h"
#include "main/main_account.h"
#include "main/main_domain.h"
#include "main/main_session.h"
#include "styles/style_sexcat.h"
#include "ui/text/format_values.h"
#include "ui/toast/toast.h"
#include "window/window_controller.h"
#include "window/window_session_controller.h"

#include <QDateTime>
#include <QDesktopServices>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QStackedWidget>
#include <QPushButton>
#include <QTreeWidget>
#include <QUrl>
#include <QVBoxLayout>

#include <algorithm>

namespace Ayu::Ui {
namespace {

constexpr auto kMaxSourceRows = 1000;
constexpr auto kFilesPageSize = 500;
constexpr auto kMaxCheckpointRows = 1000;

constexpr auto kCheckpointColChat = 0;
constexpr auto kCheckpointColTopic = 1;
constexpr auto kCheckpointColMessage = 2;
constexpr auto kCheckpointColTime = 3;
constexpr auto kCheckpointColTotal = 4;

constexpr auto kSavedColSource = 0;
constexpr auto kSavedColCount = 1;
constexpr auto kSavedColSize = 2;
constexpr auto kSavedColTotal = 3;

constexpr auto kSavedFileColName = 0;
constexpr auto kSavedFileColSize = 1;
constexpr auto kSavedFileColTime = 2;
constexpr auto kSavedFileColFolder = 3;
constexpr auto kSavedFileColOpen = 4;
constexpr auto kSavedFileColTotal = 5;

[[nodiscard]] std::vector<Main::Session *> OrderedSessions() {
	auto result = std::vector<Main::Session *>();
	for (const auto &account : Core::App().domain().orderedAccounts()) {
		if (const auto session = account->maybeSession()) {
			result.push_back(session);
		}
	}
	return result;
}

// WHY: Ayu dialog ids are masked bare ids negated for chats and
// channels alike, so the chat/channel split must be probed by load state.
[[nodiscard]] PeerData *ResolveDialogPeer(Main::Session *session, ID dialogId) {
	if (dialogId > 0) {
		return session->data().userLoaded(UserId(BareId(dialogId)));
	}
	const auto bare = BareId(uint64(-dialogId));
	if (const auto channel = session->data().channelLoaded(ChannelId(bare))) {
		return channel;
	}
	return session->data().chatLoaded(ChatId(bare));
}

[[nodiscard]] PeerData *FindDialogPeer(ID dialogId) {
	for (const auto session : OrderedSessions()) {
		if (const auto peer = ResolveDialogPeer(session, dialogId)) {
			return peer;
		}
	}
	return nullptr;
}

[[nodiscard]] Window::SessionController *WindowControllerFor(PeerData *peer) {
	const auto separate = Core::App().separateWindowFor(peer);
	return separate
		? separate->sessionController()
		: peer->session().tryResolveWindow(peer);
}

[[nodiscard]] Window::SectionShow ForwardShow() {
	auto params = Window::SectionShow{ Window::SectionShow::Way::Forward };
	params.allowDuplicateInStack = true;
	return params;
}

void JumpToPeerMessage(PeerData *peer, ID topicId, ID messageId) {
	const auto controller = WindowControllerFor(peer);
	if (!controller) {
		return;
	}
	const auto params = ForwardShow();
	const auto forum = peer->forum();
	const auto topic = (forum && topicId) ? forum->topicFor(MsgId(topicId)) : nullptr;
	if (topic) {
		controller->showTopic(topic, MsgId(messageId), params);
		return;
	}
	if (const auto item = peer->owner().message(peer->id, MsgId(messageId))) {
		controller->showMessage(item, params);
	} else {
		controller->showPeerHistory(peer, params, MsgId(messageId));
	}
}

struct CheckpointRow {
	ID dialogId = 0;
	ID topicId = 0;
	ID messageId = 0;
};

class CheckpointListWindow final : public QWidget {
public:
	CheckpointListWindow() {
		setAttribute(Qt::WA_DeleteOnClose);
		setWindowTitle(u"All checkpoints"_q);
		resize(
			st::sexcatHistoryWindowSize.width(),
			st::sexcatHistoryWindowSize.height());

		auto *root = new QVBoxLayout(this);
		auto *top = new QHBoxLayout();
		_reload = new QPushButton(u"Reload"_q, this);
		top->addStretch(1);
		top->addWidget(_reload);
		root->addLayout(top);

		_tree = new QTreeWidget(this);
		_tree->setColumnCount(kCheckpointColTotal);
		_tree->setHeaderLabels({
			u"Chat"_q,
			u"Topic"_q,
			u"Message"_q,
			u"Time"_q,
		});
		_tree->setRootIsDecorated(false);
		_tree->setUniformRowHeights(true);
		_tree->setAllColumnsShowFocus(true);
		_tree->setSelectionBehavior(QAbstractItemView::SelectRows);
		_tree->setSelectionMode(QAbstractItemView::SingleSelection);
		auto *header = _tree->header();
		header->setSectionResizeMode(kCheckpointColChat, QHeaderView::Stretch);
		header->setSectionResizeMode(kCheckpointColTopic, QHeaderView::ResizeToContents);
		header->setSectionResizeMode(kCheckpointColMessage, QHeaderView::ResizeToContents);
		header->setSectionResizeMode(kCheckpointColTime, QHeaderView::ResizeToContents);
		root->addWidget(_tree, 1);

		connect(this, &QObject::destroyed, [] {
			Instance() = nullptr;
		});
		connect(_reload, &QPushButton::clicked, this, [=] {
			reload();
		});
		connect(_tree, &QTreeWidget::itemClicked, this, [=](
				QTreeWidgetItem *item,
				int) { jump(_tree->indexOfTopLevelItem(item)); });

		reload();
	}

	static void ShowExisting() {
		if (!Instance()) {
			Instance() = new CheckpointListWindow();
		}
		Instance()->show();
		Instance()->raise();
		Instance()->activateWindow();
	}

private:
	static CheckpointListWindow *&Instance() {
		static CheckpointListWindow *value = nullptr;
		return value;
	}

	void reload() {
		_rows.clear();
		_tree->clear();
		for (const auto session : OrderedSessions()) {
			const auto userId = ID(session->userId().bare & PeerId::kChatTypeMask);
			const auto remaining = kMaxCheckpointRows - int(_rows.size());
			if (remaining <= 0) {
				break;
			}
			for (const auto &checkpoint
				: AyuDatabase::listCheckpoints(userId, remaining)) {
				const auto peer = ResolveDialogPeer(session, checkpoint.dialogId);
				auto *item = new QTreeWidgetItem(_tree);
				item->setText(
					kCheckpointColChat,
					peer ? peer->name() : QString::number(checkpoint.dialogId));
				item->setText(
					kCheckpointColTopic,
					checkpoint.topicId
						? QString::number(checkpoint.topicId)
						: u"—"_q);
				item->setText(
					kCheckpointColMessage,
					QString::number(checkpoint.messageId));
				item->setText(
					kCheckpointColTime,
					langDateTime(QDateTime::fromSecsSinceEpoch(checkpoint.date)));
				_rows.push_back(CheckpointRow{
					.dialogId = checkpoint.dialogId,
					.topicId = checkpoint.topicId,
					.messageId = checkpoint.messageId,
				});
			}
		}
	}

	void jump(int row) {
		if (row < 0 || row >= int(_rows.size())) {
			return;
		}
		const auto &entry = _rows[row];
		const auto peer = FindDialogPeer(entry.dialogId);
		if (!peer) {
			Ui::Toast::Show(u"Chat is not available in any active account"_q);
			return;
		}
		JumpToPeerMessage(peer, entry.topicId, entry.messageId);
	}

	QPushButton *_reload = nullptr;
	QTreeWidget *_tree = nullptr;
	std::vector<CheckpointRow> _rows;

};

struct SavedSourceRow {
	int64 srcPeerId = 0;
	QString name;
	int count = 0;
	int64 totalSize = 0;
};

class SavedBySourceWindow final : public QWidget {
public:
	SavedBySourceWindow() {
		setAttribute(Qt::WA_DeleteOnClose);
		setWindowTitle(u"Saved files by source"_q);
		resize(
			st::sexcatHistoryWindowSize.width(),
			st::sexcatHistoryWindowSize.height());

		auto *root = new QVBoxLayout(this);
		auto *top = new QHBoxLayout();
		_back = new QPushButton(u"Back"_q, this);
		_back->setVisible(false);
		_loadMore = new QPushButton(u"Load more"_q, this);
		_loadMore->setVisible(false);
		_reload = new QPushButton(u"Reload"_q, this);
		top->addWidget(_back);
		top->addStretch(1);
		top->addWidget(_loadMore);
		top->addWidget(_reload);
		root->addLayout(top);

		_stack = new QStackedWidget(this);
		_sources = MakeSourceTree();
		_stack->addWidget(_sources);
		_files = MakeFileTree();
		_stack->addWidget(_files);
		root->addWidget(_stack, 1);

		connect(this, &QObject::destroyed, [] {
			Instance() = nullptr;
		});
		connect(_reload, &QPushButton::clicked, this, [=] {
			reload();
		});
		connect(_loadMore, &QPushButton::clicked, this, [=] {
			appendFilesPage();
		});
		connect(_back, &QPushButton::clicked, this, [=] {
			showSources();
		});
		connect(_sources, &QTreeWidget::itemClicked, this, [=](
				QTreeWidgetItem *item,
				int) { openSource(_sources->indexOfTopLevelItem(item)); });

		reload();
	}

	static void ShowExisting() {
		if (!Instance()) {
			Instance() = new SavedBySourceWindow();
		}
		Instance()->show();
		Instance()->raise();
		Instance()->activateWindow();
	}

private:
	static SavedBySourceWindow *&Instance() {
		static SavedBySourceWindow *value = nullptr;
		return value;
	}

	[[nodiscard]] QTreeWidget *MakeSourceTree() {
		auto *tree = new QTreeWidget(this);
		tree->setColumnCount(kSavedColTotal);
		tree->setHeaderLabels({
			u"Source"_q,
			u"Files"_q,
			u"Size"_q,
		});
		tree->setRootIsDecorated(false);
		tree->setUniformRowHeights(true);
		tree->setAllColumnsShowFocus(true);
		tree->setSelectionBehavior(QAbstractItemView::SelectRows);
		tree->setSelectionMode(QAbstractItemView::SingleSelection);
		auto *header = tree->header();
		header->setSectionResizeMode(kSavedColSource, QHeaderView::Stretch);
		header->setSectionResizeMode(kSavedColCount, QHeaderView::ResizeToContents);
		header->setSectionResizeMode(kSavedColSize, QHeaderView::ResizeToContents);
		return tree;
	}

	[[nodiscard]] QTreeWidget *MakeFileTree() {
		auto *tree = new QTreeWidget(this);
		tree->setColumnCount(kSavedFileColTotal);
		tree->setHeaderLabels({
			u"File"_q,
			u"Size"_q,
			u"Time"_q,
			QString(),
			QString(),
		});
		tree->setRootIsDecorated(false);
		tree->setUniformRowHeights(true);
		tree->setAllColumnsShowFocus(true);
		tree->setSelectionBehavior(QAbstractItemView::SelectRows);
		tree->setSelectionMode(QAbstractItemView::SingleSelection);
		auto *header = tree->header();
		header->setSectionResizeMode(kSavedFileColName, QHeaderView::Stretch);
		header->setSectionResizeMode(kSavedFileColSize, QHeaderView::ResizeToContents);
		header->setSectionResizeMode(kSavedFileColTime, QHeaderView::ResizeToContents);
		header->setSectionResizeMode(kSavedFileColFolder, QHeaderView::Fixed);
		header->setSectionResizeMode(kSavedFileColOpen, QHeaderView::Fixed);
		const auto actionWidth = QPushButton(u"Folder"_q).sizeHint().width();
		header->resizeSection(kSavedFileColFolder, actionWidth);
		header->resizeSection(kSavedFileColOpen, actionWidth);
		return tree;
	}

	void reload() {
		_rows.clear();
		_rows.reserve(kMaxSourceRows);
		_sources->clear();
		for (const auto &summary
			: SexCat::Database::instance().SourceSummaries(kMaxSourceRows)) {
			_rows.push_back(SavedSourceRow{
				.srcPeerId = summary.src_peer_id,
				.name = SourceName(summary.src_peer_id, QString()),
				.count = summary.count,
				.totalSize = summary.total_size,
			});
		}
		for (const auto &row : _rows) {
			auto *item = new QTreeWidgetItem(_sources);
			item->setText(kSavedColSource, row.name);
			item->setText(kSavedColCount, QString::number(row.count));
			item->setText(kSavedColSize, Ui::FormatSizeText(row.totalSize));
		}

		const auto stillThere = _currentSource
			&& std::any_of(
				_rows.begin(),
				_rows.end(),
				[&](const SavedSourceRow &row) {
					return row.srcPeerId == _currentSource;
				});
		if (stillThere) {
			fillFiles();
		} else {
			showSources();
		}
	}

	[[nodiscard]] QString SourceName(int64 srcPeerId, const QString &fallback) {
		const auto peerId = PeerId(uint64(srcPeerId));
		for (const auto session : OrderedSessions()) {
			if (session->data().peerLoaded(peerId)) {
				return session->data().peer(peerId)->name();
			}
		}
		return fallback.isEmpty() ? QString::number(srcPeerId) : fallback;
	}

	void showSources() {
		_currentSource = 0;
		_back->setVisible(false);
		_loadMore->setVisible(false);
		setWindowTitle(u"Saved files by source"_q);
		_stack->setCurrentWidget(_sources);
	}

	void openSource(int row) {
		if (row < 0 || row >= int(_rows.size())) {
			return;
		}
		_currentSource = _rows[row].srcPeerId;
		fillFiles();
		_back->setVisible(true);
		setWindowTitle(
			u"Saved files by source — %1"_q.arg(_rows[row].name));
		_stack->setCurrentWidget(_files);
	}

	void fillFiles() {
		_fileRows.clear();
		_files->clear();
		appendFilesPage();
	}

	void appendFilesPage() {
		const auto loaded = int(_fileRows.size());
		const auto filter = SexCat::Filter{ .src_peer_id = _currentSource };
		const auto rows = SexCat::Database::instance().Recent(
			kFilesPageSize,
			filter,
			loaded);
		_loadMore->setVisible(int(rows.size()) == kFilesPageSize);
		if (rows.empty()) {
			return;
		}
		_fileRows.insert(_fileRows.end(), rows.begin(), rows.end());
		const auto rowHeight = QPushButton(u"Folder"_q).sizeHint().height();
		for (auto i = loaded; i < int(_fileRows.size()); ++i) {
			const auto &entry = _fileRows[i];
			auto *item = new QTreeWidgetItem(_files);
			item->setText(
				kSavedFileColName,
				entry.file_name.isEmpty()
					? QFileInfo(entry.saved_path).fileName()
					: entry.file_name);
			item->setText(kSavedFileColSize, Ui::FormatSizeText(entry.file_size));
			item->setText(
				kSavedFileColTime,
				langDateTime(QDateTime::fromSecsSinceEpoch(entry.saved_at)));
			item->setSizeHint(kSavedFileColName, QSize(0, rowHeight));
			item->setSizeHint(kSavedFileColFolder, QSize(0, rowHeight));
			item->setSizeHint(kSavedFileColOpen, QSize(0, rowHeight));

			const auto index = i;
			auto *folder = new QPushButton(u"Folder"_q, _files);
			connect(folder, &QPushButton::clicked, this, [=] {
				openFolderOf(index);
			});
			_files->setItemWidget(item, kSavedFileColFolder, folder);
			auto *open = new QPushButton(u"Open"_q, _files);
			connect(open, &QPushButton::clicked, this, [=] {
				openFileAt(index);
			});
			_files->setItemWidget(item, kSavedFileColOpen, open);
		}
	}

	void openFileAt(int row) {
		if (row < 0 || row >= int(_fileRows.size())) {
			return;
		}
		const auto &entry = _fileRows[row];
		if (entry.saved_path.isEmpty()) {
			Ui::Toast::Show(u"File path is not recorded"_q);
			return;
		}
		QDesktopServices::openUrl(QUrl::fromLocalFile(entry.saved_path));
	}

	void openFolderOf(int row) {
		if (row < 0 || row >= int(_fileRows.size())) {
			return;
		}
		const auto &entry = _fileRows[row];
		const auto info = QFileInfo(entry.saved_path);
		if (!info.exists()) {
			Ui::Toast::Show(u"File is missing on disk"_q);
			return;
		}
		QDesktopServices::openUrl(QUrl::fromLocalFile(info.absolutePath()));
	}

	QPushButton *_back = nullptr;
	QPushButton *_loadMore = nullptr;
	QPushButton *_reload = nullptr;
	QStackedWidget *_stack = nullptr;
	QTreeWidget *_sources = nullptr;
	QTreeWidget *_files = nullptr;
	std::vector<SavedSourceRow> _rows;
	std::vector<SexCat::Entry> _fileRows;
	int64 _currentSource = 0;

};

} // namespace

void OpenCheckpointListWindow() {
	CheckpointListWindow::ShowExisting();
}

void OpenSavedBySourceWindow() {
	SavedBySourceWindow::ShowExisting();
}

} // namespace Ayu::Ui
