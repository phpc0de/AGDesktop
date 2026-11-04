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
#include <QHBoxLayout>
#include <QHeaderView>
#include <QPushButton>
#include <QTreeWidget>
#include <QVBoxLayout>

#include <algorithm>
#include <map>

namespace Ayu::Ui {
namespace {

constexpr auto kMaxSavedRows = 100000;

constexpr auto kCheckpointColChat = 0;
constexpr auto kCheckpointColTopic = 1;
constexpr auto kCheckpointColMessage = 2;
constexpr auto kCheckpointColTime = 3;
constexpr auto kCheckpointColTotal = 4;

constexpr auto kSavedColSource = 0;
constexpr auto kSavedColCount = 1;
constexpr auto kSavedColSize = 2;
constexpr auto kSavedColTotal = 3;

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
			for (const auto &checkpoint : AyuDatabase::listCheckpoints(userId)) {
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
		_reload = new QPushButton(u"Reload"_q, this);
		top->addStretch(1);
		top->addWidget(_reload);
		root->addLayout(top);

		_tree = new QTreeWidget(this);
		_tree->setColumnCount(kSavedColTotal);
		_tree->setHeaderLabels({
			u"Source"_q,
			u"Files"_q,
			u"Size"_q,
		});
		_tree->setRootIsDecorated(false);
		_tree->setUniformRowHeights(true);
		_tree->setAllColumnsShowFocus(true);
		_tree->setSelectionBehavior(QAbstractItemView::SelectRows);
		_tree->setSelectionMode(QAbstractItemView::SingleSelection);
		auto *header = _tree->header();
		header->setSectionResizeMode(kSavedColSource, QHeaderView::Stretch);
		header->setSectionResizeMode(kSavedColCount, QHeaderView::ResizeToContents);
		header->setSectionResizeMode(kSavedColSize, QHeaderView::ResizeToContents);
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

	void reload() {
		auto groups = std::map<int64, SavedSourceRow>();
		for (const auto &entry : SexCat::Database::instance().Recent(kMaxSavedRows)) {
			auto &row = groups[entry.src_peer_id];
			++row.count;
			row.totalSize += entry.file_size;
			if (row.name.isEmpty() && !entry.src_title.isEmpty()) {
				row.name = entry.src_title;
			}
		}

		_rows.clear();
		_rows.reserve(groups.size());
		_tree->clear();
		for (const auto &[id, group] : groups) {
			auto row = group;
			row.srcPeerId = id;
			row.name = SourceName(id, row.name);
			_rows.push_back(std::move(row));
		}
		std::sort(
			_rows.begin(),
			_rows.end(),
			[](const SavedSourceRow &a, const SavedSourceRow &b) {
				return a.count > b.count;
			});
		for (const auto &row : _rows) {
			auto *item = new QTreeWidgetItem(_tree);
			item->setText(kSavedColSource, row.name);
			item->setText(kSavedColCount, QString::number(row.count));
			item->setText(kSavedColSize, Ui::FormatSizeText(row.totalSize));
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

	void jump(int row) {
		if (row < 0 || row >= int(_rows.size())) {
			return;
		}
		const auto peerId = PeerId(uint64(_rows[row].srcPeerId));
		for (const auto session : OrderedSessions()) {
			if (session->data().peerLoaded(peerId)) {
				const auto peer = session->data().peer(peerId);
				if (const auto controller = WindowControllerFor(peer)) {
					controller->showPeerHistory(peer, ForwardShow());
				}
				return;
			}
		}
		Ui::Toast::Show(u"Chat is not available in any active account"_q);
	}

	QPushButton *_reload = nullptr;
	QTreeWidget *_tree = nullptr;
	std::vector<SavedSourceRow> _rows;

};

} // namespace

void OpenCheckpointListWindow() {
	CheckpointListWindow::ShowExisting();
}

void OpenSavedBySourceWindow() {
	SavedBySourceWindow::ShowExisting();
}

} // namespace Ayu::Ui
