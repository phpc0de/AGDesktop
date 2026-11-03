// This is the source code of AyuGram for Desktop.
//
// We do not and cannot prevent the use of our code,
// but be respectful and credit the original author.
//
// Copyright @Radolyn, 2026
#include "ayu/sexcat/sexcat_database.h"

#include "ayu/libs/sqlite/sqlite_orm.h"
#include "ayu/utils/ayu_log.h"
#include "base/unixtime.h"

#include <crl/crl.h>

#include <QDir>

#include <limits>
#include <mutex>
#include <utility>

namespace SexCat {

using namespace sqlite_orm;

namespace {

struct SavedFileRow {
	int64 id = 0;
	std::vector<char> sha256;
	std::string file_name;
	std::string saved_path;
	int64 file_size = 0;
	int64 src_peer_id = 0;
	int64 src_msg_id = 0;
	std::string src_title;
	int src_attach = 0;
	int64 saved_at = 0;
};

// WHY: schema objects are iterated in reverse, so an index must precede
// its table or sync_schema creates it while the table is still missing.
auto storage = make_storage(
	"./tdata/sexcat/sexcat.db",
	make_index(
		"idx_saved_file_sha",
		column<SavedFileRow>(&SavedFileRow::sha256)),
	make_index(
		"idx_saved_file_src",
		column<SavedFileRow>(&SavedFileRow::src_peer_id),
		column<SavedFileRow>(&SavedFileRow::src_msg_id)),
	make_table<SavedFileRow>(
		"saved_file",
		make_column("id", &SavedFileRow::id, primary_key().autoincrement()),
		make_column("sha256", &SavedFileRow::sha256),
		make_column("file_name", &SavedFileRow::file_name),
		make_column("saved_path", &SavedFileRow::saved_path),
		make_column("file_size", &SavedFileRow::file_size),
		make_column("src_peer_id", &SavedFileRow::src_peer_id),
		make_column("src_msg_id", &SavedFileRow::src_msg_id),
		make_column("src_title", &SavedFileRow::src_title),
		make_column("src_attach", &SavedFileRow::src_attach),
		make_column("saved_at", &SavedFileRow::saved_at)
	)
);

std::mutex storageMutex;

void LogError(const QString &message) {
	Ayu::Log::Write(u"sexcat"_q, message);
}

Entry EntryFromRow(SavedFileRow row) {
	auto entry = Entry();
	entry.id = row.id;
	entry.sha256 = QByteArray(
		row.sha256.data(),
		int(row.sha256.size()));
	entry.file_name = QString::fromStdString(row.file_name);
	entry.saved_path = QString::fromStdString(row.saved_path);
	entry.file_size = row.file_size;
	entry.src_peer_id = row.src_peer_id;
	entry.src_msg_id = row.src_msg_id;
	entry.src_title = QString::fromStdString(row.src_title);
	entry.src_attach = row.src_attach;
	entry.saved_at = row.saved_at;
	return entry;
}

SavedFileRow RowFromEntry(const Entry &entry) {
	auto row = SavedFileRow();
	row.id = entry.id;
	row.sha256.assign(entry.sha256.cbegin(), entry.sha256.cend());
	row.file_name = entry.file_name.toStdString();
	row.saved_path = entry.saved_path.toStdString();
	row.file_size = entry.file_size;
	row.src_peer_id = entry.src_peer_id;
	row.src_msg_id = entry.src_msg_id;
	row.src_title = entry.src_title.toStdString();
	row.src_attach = entry.src_attach;
	row.saved_at = entry.saved_at;
	return row;
}

std::string LikePattern(const QString &search) {
	const auto raw = search.toStdString();
	auto escaped = std::string();
	escaped.reserve(raw.size() * 2 + 2);
	escaped += '%';
	for (const auto ch : raw) {
		if (ch == '%' || ch == '_' || ch == '\\') {
			escaped += '\\';
		}
		escaped += ch;
	}
	escaped += '%';
	return escaped;
}

} // namespace

Database &Database::instance() {
	static auto result = Database();
	return result;
}

Database::Database() {
	QDir().mkpath(u"./tdata/sexcat"_q);
	try {
		storage.sync_schema(true);
		storage.open_forever();
		_enabled = true;
	} catch (const std::exception &ex) {
		LogError(u"Database initialization failed, feature disabled: %1"_q.arg(QString::fromUtf8(ex.what())));
	} catch (...) {
		LogError(u"Database initialization failed, feature disabled"_q);
	}
}

Database::~Database() = default;

bool Database::Enabled() const {
	return _enabled;
}

rpl::producer<Entry> Database::newSaved() const {
	return _newSaved.events();
}

bool Database::RecordSaved(Entry entry) {
	if (!_enabled || entry.sha256.isEmpty()) {
		return false;
	}
	if (entry.saved_at == 0) {
		entry.saved_at = base::unixtime::now();
	}
	auto row = RowFromEntry(entry);
	row.id = 0;
	{
		std::lock_guard lock(storageMutex);
		try {
			storage.begin_transaction();
			row.id = storage.insert(row);
			storage.commit();
		} catch (const std::exception &ex) {
			try {
				storage.rollback();
			} catch (...) {
			}
			LogError(u"Failed to record saved file: %1"_q.arg(QString::fromUtf8(ex.what())));
			return false;
		}
	}
	auto saved = EntryFromRow(std::move(row));
	crl::on_main([saved = std::move(saved)]() mutable {
		instance()._newSaved.fire_copy(std::move(saved));
	});
	return true;
}

std::vector<Entry> Database::LookupByHash(QByteArray sha256) {
	if (!_enabled || sha256.isEmpty()) {
		return {};
	}
	try {
		std::lock_guard lock(storageMutex);
		const auto blob = std::vector<char>(sha256.cbegin(), sha256.cend());
		auto rows = storage.get_all<SavedFileRow>(
			where(column<SavedFileRow>(&SavedFileRow::sha256) == blob),
			order_by(column<SavedFileRow>(&SavedFileRow::id)).desc()
		);
		auto result = std::vector<Entry>();
		result.reserve(rows.size());
		for (auto &row : rows) {
			result.push_back(EntryFromRow(std::move(row)));
		}
		return result;
	} catch (const std::exception &ex) {
		LogError(u"Failed to look up by hash: %1"_q.arg(QString::fromUtf8(ex.what())));
		return {};
	}
}

std::optional<Entry> Database::FindByPath(
		const QString &savedPath,
		int64 fileSize) {
	if (!_enabled || savedPath.isEmpty()) {
		return std::nullopt;
	}
	try {
		std::lock_guard lock(storageMutex);
		auto rows = storage.get_all<SavedFileRow>(
			where(
				column<SavedFileRow>(&SavedFileRow::saved_path)
					== savedPath.toStdString()
				and column<SavedFileRow>(&SavedFileRow::file_size) == fileSize
			),
			order_by(column<SavedFileRow>(&SavedFileRow::id)).desc(),
			sqlite_orm::limit(1)
		);
		return rows.empty()
			? std::nullopt
			: std::optional<Entry>(EntryFromRow(std::move(rows.front())));
	} catch (const std::exception &ex) {
		LogError(u"Failed to look up by path: %1"_q.arg(QString::fromUtf8(ex.what())));
		return std::nullopt;
	}
}

bool Database::Relink(int64 id, const QString &newPath) {
	if (!_enabled || id <= 0 || newPath.isEmpty()) {
		return false;
	}
	try {
		std::lock_guard lock(storageMutex);
		storage.update_all<SavedFileRow>(
			set(column<SavedFileRow>(&SavedFileRow::saved_path)
				= newPath.toStdString()),
			where(column<SavedFileRow>(&SavedFileRow::id) == id));
		return true;
	} catch (const std::exception &ex) {
		LogError(u"Failed to relink saved file: %1"_q.arg(QString::fromUtf8(ex.what())));
		return false;
	}
}

std::vector<Entry> Database::LookupBySource(PeerId peer, MsgId msg) {
	if (!_enabled || !peer || !msg) {
		return {};
	}
	try {
		std::lock_guard lock(storageMutex);
		auto rows = storage.get_all<SavedFileRow>(
			where(
				column<SavedFileRow>(&SavedFileRow::src_peer_id) == int64(peer.value)
				and column<SavedFileRow>(&SavedFileRow::src_msg_id) == msg.bare
			),
			order_by(column<SavedFileRow>(&SavedFileRow::id)).desc()
		);
		auto result = std::vector<Entry>();
		result.reserve(rows.size());
		for (auto &row : rows) {
			result.push_back(EntryFromRow(std::move(row)));
		}
		return result;
	} catch (const std::exception &ex) {
		LogError(u"Failed to look up by source: %1"_q.arg(QString::fromUtf8(ex.what())));
		return {};
	}
}

std::vector<Entry> Database::Recent(int limit, Filter filter) {
	if (!_enabled || limit <= 0) {
		return {};
	}
	try {
		std::lock_guard lock(storageMutex);
		const auto hasPeer = filter.src_peer_id.has_value();
		const auto peerLower = hasPeer ? *filter.src_peer_id : std::numeric_limits<int64>::min();
		const auto peerUpper = hasPeer ? *filter.src_peer_id : std::numeric_limits<int64>::max();
		const auto pattern = LikePattern(filter.search);
		auto rows = storage.get_all<SavedFileRow>(
			where(
				column<SavedFileRow>(&SavedFileRow::src_peer_id) >= peerLower
				and column<SavedFileRow>(&SavedFileRow::src_peer_id) <= peerUpper
				and (like(column<SavedFileRow>(&SavedFileRow::file_name), pattern, "\\")
					or like(column<SavedFileRow>(&SavedFileRow::src_title), pattern, "\\"))
			),
			order_by(column<SavedFileRow>(&SavedFileRow::id)).desc(),
			sqlite_orm::limit(limit)
		);
		auto result = std::vector<Entry>();
		result.reserve(rows.size());
		for (auto &row : rows) {
			result.push_back(EntryFromRow(std::move(row)));
		}
		return result;
	} catch (const std::exception &ex) {
		LogError(u"Failed to read recent saved files: %1"_q.arg(QString::fromUtf8(ex.what())));
		return {};
	}
}

} // namespace SexCat
