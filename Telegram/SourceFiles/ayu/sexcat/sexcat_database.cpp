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
#include <QFileInfo>

#include <algorithm>
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
	std::string src_tag;
	int src_attach = 0;
	int64 saved_at = 0;
};

struct SowTagRow {
	int64 id = 0;
	std::string tag;
	int64 created_at = 0;
};

struct SourceTagRow {
	int64 source_id = 0;
	std::string tag;
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
	make_index(
		"idx_saved_file_path",
		column<SavedFileRow>(&SavedFileRow::saved_path)),
	make_index(
		"idx_saved_file_tag",
		column<SavedFileRow>(&SavedFileRow::src_tag)),
	make_index(
		"idx_sow_tag_tag",
		column<SowTagRow>(&SowTagRow::tag)),
	make_index(
		"idx_source_tag_tag",
		column<SourceTagRow>(&SourceTagRow::tag)),
	make_index(
		"idx_source_tag_source",
		column<SourceTagRow>(&SourceTagRow::source_id)),
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
		make_column("src_tag", &SavedFileRow::src_tag, default_value(std::string())),
		make_column("src_attach", &SavedFileRow::src_attach),
		make_column("saved_at", &SavedFileRow::saved_at)
	),
	make_table<SowTagRow>(
		"sow_tag",
		make_column("id", &SowTagRow::id, primary_key().autoincrement()),
		make_column("tag", &SowTagRow::tag),
		make_column("created_at", &SowTagRow::created_at)
	),
	make_table<SourceTagRow>(
		"source_tag",
		make_column("source_id", &SourceTagRow::source_id),
		make_column("tag", &SourceTagRow::tag)
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
	entry.src_tag = QString::fromStdString(row.src_tag);
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
	row.src_tag = entry.src_tag.toStdString();
	row.src_attach = entry.src_attach;
	row.saved_at = entry.saved_at;
	return row;
}

void BackfillSourcelessRows(const QByteArray &sha256, const Entry &source) {
	try {
		std::lock_guard lock(storageMutex);
		const auto blob = std::vector<char>(sha256.cbegin(), sha256.cend());
		storage.update_all(
			set(
				c(&SavedFileRow::src_peer_id) = source.src_peer_id,
				c(&SavedFileRow::src_msg_id) = source.src_msg_id,
				c(&SavedFileRow::src_title) = source.src_title.toStdString(),
				c(&SavedFileRow::src_tag) = source.src_tag.toStdString(),
				c(&SavedFileRow::src_attach) = source.src_attach),
			where(
				column<SavedFileRow>(&SavedFileRow::sha256) == blob
				and column<SavedFileRow>(&SavedFileRow::src_peer_id) == int64(0)
				and column<SavedFileRow>(&SavedFileRow::src_msg_id) == int64(0)));
	} catch (const std::exception &ex) {
		LogError(u"Failed to backfill source by hash: %1"_q.arg(QString::fromUtf8(ex.what())));
	}
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
		BackfillLegacySourceTags();
	} catch (const std::exception &ex) {
		LogError(u"Database initialization failed, feature disabled: %1"_q.arg(QString::fromUtf8(ex.what())));
	} catch (...) {
		LogError(u"Database initialization failed, feature disabled"_q);
	}
}

Database::~Database() = default;

// WHY: legacy rows carry only the first tag in src_tag; the tag table
// starts empty, so a one-time mirror keeps old records filterable.
void Database::BackfillLegacySourceTags() {
	if (!_enabled) {
		return;
	}
	try {
		std::lock_guard lock(storageMutex);
		if (storage.count<SourceTagRow>() > 0) {
			return;
		}
		for (const auto &row : storage.get_all<SavedFileRow>(
			where(column<SavedFileRow>(&SavedFileRow::src_tag)
				!= std::string()))) {
			storage.insert(SourceTagRow{
				.source_id = row.id,
				.tag = row.src_tag,
			});
		}
	} catch (const std::exception &ex) {
		LogError(u"Failed to backfill source tags: %1"_q.arg(QString::fromUtf8(ex.what())));
	}
}

bool Database::Enabled() const {
	return _enabled;
}

rpl::producer<Entry> Database::newSaved() const {
	return _newSaved.events();
}

int64 Database::RecordSaved(Entry entry) {
	if (!_enabled) {
		return 0;
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
			return 0;
		}
	}
	if (!entry.sha256.isEmpty()
		&& entry.src_peer_id != 0
		&& entry.src_msg_id != 0) {
		BackfillSourcelessRows(entry.sha256, entry);
	}
	auto saved = EntryFromRow(std::move(row));
	crl::on_main([saved = std::move(saved)]() mutable {
		instance()._newSaved.fire_copy(std::move(saved));
	});
	return row.id;
}

bool Database::UpdateRowSha(int64 id, const QByteArray &sha256) {
	if (!_enabled || id <= 0 || sha256.size() != 32) {
		return false;
	}
	try {
		std::lock_guard lock(storageMutex);
		storage.update_all(
			set(c(&SavedFileRow::sha256)
				= std::vector<char>(sha256.cbegin(), sha256.cend())),
			where(
				column<SavedFileRow>(&SavedFileRow::id) == id
				and column<SavedFileRow>(&SavedFileRow::sha256)
					== std::vector<char>()));
		return true;
	} catch (const std::exception &ex) {
		LogError(u"Failed to backfill sha256: %1"_q.arg(QString::fromUtf8(ex.what())));
		return false;
	}
}

bool Database::DropUnhashedRecord(int64 id) {
	if (!_enabled || id <= 0) {
		return false;
	}
	try {
		std::lock_guard lock(storageMutex);
		storage.remove_all<SavedFileRow>(
			where(
				column<SavedFileRow>(&SavedFileRow::id) == id
				and column<SavedFileRow>(&SavedFileRow::sha256)
					== std::vector<char>()));
		return true;
	} catch (const std::exception &ex) {
		LogError(u"Failed to drop unhashed record: %1"_q.arg(QString::fromUtf8(ex.what())));
		return false;
	}
}

std::vector<Entry> Database::LookupUnhashed(int limit) {
	if (!_enabled || limit <= 0) {
		return {};
	}
	try {
		std::lock_guard lock(storageMutex);
		auto rows = storage.get_all<SavedFileRow>(
			where(column<SavedFileRow>(&SavedFileRow::sha256)
				== std::vector<char>()),
			order_by(column<SavedFileRow>(&SavedFileRow::id)).asc(),
			sqlite_orm::limit(limit));
		auto result = std::vector<Entry>();
		result.reserve(rows.size());
		for (auto &row : rows) {
			result.push_back(EntryFromRow(std::move(row)));
		}
		return result;
	} catch (const std::exception &ex) {
		LogError(u"Failed to look up unhashed records: %1"_q.arg(QString::fromUtf8(ex.what())));
		return {};
	}
}

bool Database::BackfillSourceByHash(
		const QByteArray &sha256,
		const Entry &source) {
	if (sha256.size() != 32) {
		return false;
	}
	BackfillSourcelessRows(sha256, source);
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

std::optional<Entry> Database::LookupByPath(const QString &savedPath) {
	if (!_enabled || savedPath.isEmpty()) {
		return std::nullopt;
	}
	try {
		std::lock_guard lock(storageMutex);
		auto rows = storage.get_all<SavedFileRow>(
			where(
				column<SavedFileRow>(&SavedFileRow::saved_path)
					== savedPath.toStdString()
			),
			order_by(column<SavedFileRow>(&SavedFileRow::id)).desc(),
			sqlite_orm::limit(1)
		);
		return rows.empty()
			? std::nullopt
			: std::optional<Entry>(EntryFromRow(std::move(rows.front())));
	} catch (const std::exception &ex) {
		LogError(u"Failed to look up latest by path: %1"_q.arg(QString::fromUtf8(ex.what())));
		return std::nullopt;
	}
}

std::vector<Entry> Database::LookupByPathPrefix(const QString &prefix) {
	if (!_enabled || prefix.isEmpty()) {
		return {};
	}
	try {
		std::lock_guard lock(storageMutex);
		auto rows = storage.get_all<SavedFileRow>(
			where(
				like(
					column<SavedFileRow>(&SavedFileRow::saved_path),
					prefix.toStdString() + '%')),
			order_by(column<SavedFileRow>(&SavedFileRow::id)).desc());
		auto result = std::vector<Entry>();
		result.reserve(rows.size());
		for (auto &row : rows) {
			result.push_back(EntryFromRow(std::move(row)));
		}
		return result;
	} catch (const std::exception &ex) {
		LogError(u"Failed to look up by path prefix: %1"_q.arg(QString::fromUtf8(ex.what())));
		return {};
	}
}

bool Database::Relink(int64 id, const QString &newPath) {
	if (!_enabled || id <= 0 || newPath.isEmpty()) {
		return false;
	}
	try {
		std::lock_guard lock(storageMutex);
		storage.update_all(
			set(
				c(&SavedFileRow::saved_path) = newPath.toStdString(),
				c(&SavedFileRow::file_name)
					= QFileInfo(newPath).fileName().toStdString()),
			where(column<SavedFileRow>(&SavedFileRow::id) == id));
		try {
			auto rows = storage.get_all<SavedFileRow>(
				where(column<SavedFileRow>(&SavedFileRow::id) == id));
			if (!rows.empty()) {
				auto relinked = EntryFromRow(std::move(rows.front()));
				crl::on_main([relinked = std::move(relinked)]() mutable {
					instance()._newSaved.fire_copy(std::move(relinked));
				});
			}
		} catch (...) {
		}
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

std::vector<Entry> Database::Recent(int limit, Filter filter, int offset) {
	if (!_enabled || limit <= 0 || offset < 0) {
		return {};
	}
	try {
		std::lock_guard lock(storageMutex);
		const auto hasTag = !filter.src_tag.isEmpty();
		const auto hasPeer = filter.src_peer_id.has_value();
		const auto peerLower = hasPeer ? *filter.src_peer_id : std::numeric_limits<int64>::min();
		const auto peerUpper = hasPeer ? *filter.src_peer_id : std::numeric_limits<int64>::max();
		const auto pattern = LikePattern(filter.search);
		auto rows = hasTag
			? storage.get_all<SavedFileRow>(
				join<SourceTagRow>(
					on(c(&SourceTagRow::source_id)
						== c(&SavedFileRow::id))),
				where(
					column<SourceTagRow>(&SourceTagRow::tag)
						== filter.src_tag.toStdString()
					and (like(column<SavedFileRow>(&SavedFileRow::file_name), pattern, "\\")
						or like(column<SavedFileRow>(&SavedFileRow::src_title), pattern, "\\"))
				),
				order_by(column<SavedFileRow>(&SavedFileRow::id)).desc(),
				sqlite_orm::limit(limit, sqlite_orm::offset(offset))
			)
			: storage.get_all<SavedFileRow>(
				where(
					column<SavedFileRow>(&SavedFileRow::src_peer_id) >= peerLower
					and column<SavedFileRow>(&SavedFileRow::src_peer_id) <= peerUpper
					and (like(column<SavedFileRow>(&SavedFileRow::file_name), pattern, "\\")
						or like(column<SavedFileRow>(&SavedFileRow::src_title), pattern, "\\"))
				),
				order_by(column<SavedFileRow>(&SavedFileRow::id)).desc(),
				sqlite_orm::limit(limit, sqlite_orm::offset(offset))
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

std::optional<Entry> Database::FindSavedSource(
		PeerId peer,
		MsgId msg,
		int attach,
		int64 fileSize) {
	if (!_enabled || !peer || !msg) {
		return std::nullopt;
	}
	try {
		std::lock_guard lock(storageMutex);
		auto rows = storage.get_all<SavedFileRow>(
			where(
				column<SavedFileRow>(&SavedFileRow::src_peer_id) == int64(peer.value)
				and column<SavedFileRow>(&SavedFileRow::src_msg_id) == msg.bare
				and column<SavedFileRow>(&SavedFileRow::src_attach) == attach
				and column<SavedFileRow>(&SavedFileRow::file_size) == fileSize
			),
			order_by(column<SavedFileRow>(&SavedFileRow::id)).desc(),
			sqlite_orm::limit(1)
		);
		return rows.empty()
			? std::nullopt
			: std::optional<Entry>(EntryFromRow(std::move(rows.front())));
	} catch (const std::exception &ex) {
		LogError(u"Failed to find saved file by source: %1"_q.arg(QString::fromUtf8(ex.what())));
		return std::nullopt;
	}
}

int64 Database::RecordPendingPosition(const Entry &source) {
	if (!_enabled || source.src_peer_id == 0 || source.src_msg_id == 0) {
		return 0;
	}
	try {
		std::lock_guard lock(storageMutex);
		auto rows = storage.get_all<SavedFileRow>(
			where(
				column<SavedFileRow>(&SavedFileRow::src_peer_id) == source.src_peer_id
				and column<SavedFileRow>(&SavedFileRow::src_msg_id) == source.src_msg_id
				and column<SavedFileRow>(&SavedFileRow::src_attach) == source.src_attach
				and column<SavedFileRow>(&SavedFileRow::file_size) == source.file_size
			),
			sqlite_orm::limit(1)
		);
		if (!rows.empty()) {
			return rows.front().id;
		}
		auto row = RowFromEntry(source);
		row.id = 0;
		row.sha256 = std::vector<char>();
		row.saved_path.clear();
		if (row.saved_at == 0) {
			row.saved_at = base::unixtime::now();
		}
		return storage.insert(row);
	} catch (const std::exception &ex) {
		LogError(u"Failed to record pending position: %1"_q.arg(QString::fromUtf8(ex.what())));
		return 0;
	}
}

void Database::DropPendingPosition(const Entry &source) {
	if (!_enabled || source.src_peer_id == 0 || source.src_msg_id == 0) {
		return;
	}
	try {
		std::lock_guard lock(storageMutex);
		storage.remove_all<SavedFileRow>(
			where(
				column<SavedFileRow>(&SavedFileRow::src_peer_id) == source.src_peer_id
				and column<SavedFileRow>(&SavedFileRow::src_msg_id) == source.src_msg_id
				and column<SavedFileRow>(&SavedFileRow::src_attach) == source.src_attach
				and column<SavedFileRow>(&SavedFileRow::saved_path)
					== std::string()));
	} catch (const std::exception &ex) {
		LogError(u"Failed to drop pending position: %1"_q.arg(QString::fromUtf8(ex.what())));
	}
}

int64 Database::ConsumePendingPosition(
		const Entry &source,
		const QString &savedPath) {
	if (!_enabled || source.src_peer_id == 0 || source.src_msg_id == 0) {
		return 0;
	}
	try {
		std::lock_guard lock(storageMutex);
		const auto emptySha = std::vector<char>();
		auto rows = storage.get_all<SavedFileRow>(
			where(
				column<SavedFileRow>(&SavedFileRow::src_peer_id) == source.src_peer_id
				and column<SavedFileRow>(&SavedFileRow::src_msg_id) == source.src_msg_id
				and column<SavedFileRow>(&SavedFileRow::src_attach) == source.src_attach
				and column<SavedFileRow>(&SavedFileRow::sha256) == emptySha
			),
			order_by(column<SavedFileRow>(&SavedFileRow::id)).desc(),
			sqlite_orm::limit(1)
		);
		if (rows.empty()) {
			return 0;
		}
		const auto id = rows.front().id;
		storage.update_all(
			set(c(&SavedFileRow::saved_path) = savedPath.toStdString()),
			where(column<SavedFileRow>(&SavedFileRow::id) == id));
		return id;
	} catch (const std::exception &ex) {
		LogError(u"Failed to consume pending position: %1"_q.arg(QString::fromUtf8(ex.what())));
		return 0;
	}
}

bool Database::RecordTag(const QString &tag) {
	if (!_enabled || tag.isEmpty()) {
		return false;
	}
	try {
		std::lock_guard lock(storageMutex);
		const auto raw = tag.toStdString();
		auto rows = storage.get_all<SowTagRow>(
			where(column<SowTagRow>(&SowTagRow::tag) == raw),
			sqlite_orm::limit(1));
		if (!rows.empty()) {
			return true;
		}
		storage.insert(SowTagRow{
			.id = 0,
			.tag = raw,
			.created_at = base::unixtime::now(),
		});
		return true;
	} catch (const std::exception &ex) {
		LogError(u"Failed to record sow tag: %1"_q.arg(QString::fromUtf8(ex.what())));
		return false;
	}
}

void Database::ReplaceSourceTags(int64 sourceId, const QStringList &tags) {
	if (!_enabled || sourceId <= 0) {
		return;
	}
	auto cleaned = QStringList();
	for (const auto &tag : tags) {
		if (!tag.isEmpty() && !cleaned.contains(tag)) {
			cleaned.push_back(tag);
		}
	}
	try {
		std::lock_guard lock(storageMutex);
		storage.begin_transaction();
		storage.remove_all<SourceTagRow>(
			where(column<SourceTagRow>(&SourceTagRow::source_id) == sourceId));
		for (const auto &tag : cleaned) {
			storage.insert(SourceTagRow{
				.source_id = sourceId,
				.tag = tag.toStdString(),
			});
		}
		storage.commit();
	} catch (const std::exception &ex) {
		try {
			storage.rollback();
		} catch (...) {
		}
		LogError(u"Failed to replace source tags: %1"_q.arg(QString::fromUtf8(ex.what())));
	}
}

QStringList Database::SourceTags(int64 sourceId) {
	if (!_enabled || sourceId <= 0) {
		return {};
	}
	try {
		std::lock_guard lock(storageMutex);
		auto rows = storage.select(
			column<SourceTagRow>(&SourceTagRow::tag),
			where(column<SourceTagRow>(&SourceTagRow::source_id) == sourceId));
		auto result = QStringList();
		result.reserve(int(rows.size()));
		for (auto &tag : rows) {
			result.push_back(QString::fromStdString(std::move(tag)));
		}
		return result;
	} catch (const std::exception &ex) {
		LogError(u"Failed to read source tags: %1"_q.arg(QString::fromUtf8(ex.what())));
		return {};
	}
}

std::vector<TagSummary> Database::TagSummaries(int limit) {
	if (!_enabled || limit <= 0) {
		return {};
	}
	try {
		std::lock_guard lock(storageMutex);
		auto rows = storage.select(
			columns(
				column<SourceTagRow>(&SourceTagRow::tag),
				count(column<SavedFileRow>(&SavedFileRow::id)),
				sum(column<SavedFileRow>(&SavedFileRow::file_size))),
			join<SavedFileRow>(
				on(c(&SavedFileRow::id) == c(&SourceTagRow::source_id))),
			group_by(column<SourceTagRow>(&SourceTagRow::tag)),
			order_by(count(column<SavedFileRow>(&SavedFileRow::id))).desc());
		auto result = std::vector<TagSummary>();
		result.reserve(rows.size());
		for (auto &[tag, counted, totalSize] : rows) {
			result.push_back(TagSummary{
				.tag = QString::fromStdString(tag),
				.count = counted,
				.total_size = totalSize ? int64(*totalSize) : 0,
			});
		}
		for (const auto &registered : storage.select(
				column<SowTagRow>(&SowTagRow::tag))) {
			const auto name = QString::fromStdString(registered);
			const auto known = std::any_of(
				result.begin(),
				result.end(),
				[&](const TagSummary &summary) { return summary.tag == name; });
			if (!known) {
				result.push_back(TagSummary{ .tag = name, .count = 0 });
			}
		}
		std::stable_sort(
			result.begin(),
			result.end(),
			[](const TagSummary &a, const TagSummary &b) {
				return a.count > b.count;
			});
		if (int(result.size()) > limit) {
			result.resize(limit);
		}
		return result;
	} catch (const std::exception &ex) {
		LogError(u"Failed to read sow tag summaries: %1"_q.arg(QString::fromUtf8(ex.what())));
		return {};
	}
}

bool Database::RenameTag(const QString &from, const QString &to) {
	if (!_enabled || from.isEmpty() || to.isEmpty() || from == to) {
		return false;
	}
	try {
		std::lock_guard lock(storageMutex);
		storage.begin_transaction();
		storage.update_all(
			set(c(&SavedFileRow::src_tag) = to.toStdString()),
			where(column<SavedFileRow>(&SavedFileRow::src_tag)
				== from.toStdString()));
		const auto taggedIds = storage.select(
			column<SourceTagRow>(&SourceTagRow::source_id),
			where(column<SourceTagRow>(&SourceTagRow::tag)
				== from.toStdString()));
		for (const auto &id : taggedIds) {
			const auto taken = !storage.get_all<SourceTagRow>(
				where(column<SourceTagRow>(&SourceTagRow::source_id) == id
					and column<SourceTagRow>(&SourceTagRow::tag)
						== to.toStdString()),
				sqlite_orm::limit(1)).empty();
			if (taken) {
				storage.remove_all<SourceTagRow>(
					where(column<SourceTagRow>(&SourceTagRow::source_id) == id
						and column<SourceTagRow>(&SourceTagRow::tag)
							== from.toStdString()));
			} else {
				storage.update_all(
					set(c(&SourceTagRow::tag) = to.toStdString()),
					where(column<SourceTagRow>(&SourceTagRow::source_id) == id
						and column<SourceTagRow>(&SourceTagRow::tag)
							== from.toStdString()));
			}
		}
		const auto taken = !storage.get_all<SowTagRow>(
			where(column<SowTagRow>(&SowTagRow::tag) == to.toStdString()),
			sqlite_orm::limit(1)).empty();
		if (taken) {
			storage.remove_all<SowTagRow>(
				where(column<SowTagRow>(&SowTagRow::tag)
					== from.toStdString()));
		} else {
			storage.update_all(
				set(c(&SowTagRow::tag) = to.toStdString()),
				where(column<SowTagRow>(&SowTagRow::tag)
					== from.toStdString()));
		}
		storage.commit();
		return true;
	} catch (const std::exception &ex) {
		try {
			storage.rollback();
		} catch (...) {
		}
		LogError(u"Failed to rename tag: %1"_q.arg(QString::fromUtf8(ex.what())));
		return false;
	}
}

bool Database::DeleteTag(const QString &tag) {
	if (!_enabled || tag.isEmpty()) {
		return false;
	}
	try {
		std::lock_guard lock(storageMutex);
		storage.begin_transaction();
		storage.update_all(
			set(c(&SavedFileRow::src_tag) = std::string()),
			where(column<SavedFileRow>(&SavedFileRow::src_tag)
				== tag.toStdString()));
		storage.remove_all<SourceTagRow>(
			where(column<SourceTagRow>(&SourceTagRow::tag)
				== tag.toStdString()));
		storage.remove_all<SowTagRow>(
			where(column<SowTagRow>(&SowTagRow::tag) == tag.toStdString()));
		storage.commit();
		return true;
	} catch (const std::exception &ex) {
		try {
			storage.rollback();
		} catch (...) {
		}
		LogError(u"Failed to delete tag: %1"_q.arg(QString::fromUtf8(ex.what())));
		return false;
	}
}

int64 Database::DropRecordByPath(const QString &savedPath) {
	if (!_enabled || savedPath.isEmpty()) {
		return 0;
	}
	try {
		std::lock_guard lock(storageMutex);
		const auto matches = int64(storage.count<SavedFileRow>(
			where(column<SavedFileRow>(&SavedFileRow::saved_path)
				== savedPath.toStdString())));
		if (matches > 0) {
			storage.remove_all<SavedFileRow>(
				where(column<SavedFileRow>(&SavedFileRow::saved_path)
					== savedPath.toStdString()));
		}
		return matches;
	} catch (const std::exception &ex) {
		LogError(u"Failed to drop record by path: %1"_q.arg(QString::fromUtf8(ex.what())));
		return 0;
	}
}

bool Database::DropRecordById(int64 id) {
	if (!_enabled || id <= 0) {
		return false;
	}
	try {
		std::lock_guard lock(storageMutex);
		storage.remove_all<SavedFileRow>(
			where(column<SavedFileRow>(&SavedFileRow::id) == id));
		return true;
	} catch (const std::exception &ex) {
		LogError(u"Failed to drop record by id: %1"_q.arg(QString::fromUtf8(ex.what())));
		return false;
	}
}

int Database::RetagSource(PeerId peer, MsgId msg, const QString &tag) {
	if (!_enabled || !peer || !msg || tag.isEmpty()) {
		return 0;
	}
	try {
		std::lock_guard lock(storageMutex);
		const auto matches = storage.count<SavedFileRow>(
			where(
				column<SavedFileRow>(&SavedFileRow::src_peer_id)
					== int64(peer.value)
				and column<SavedFileRow>(&SavedFileRow::src_msg_id)
					== msg.bare));
		if (matches > 0) {
			const auto ids = storage.select(
				column<SavedFileRow>(&SavedFileRow::id),
				where(
					column<SavedFileRow>(&SavedFileRow::src_peer_id)
						== int64(peer.value)
					and column<SavedFileRow>(&SavedFileRow::src_msg_id)
						== msg.bare));
			storage.begin_transaction();
			storage.update_all(
				set(c(&SavedFileRow::src_tag) = tag.toStdString()),
				where(
					column<SavedFileRow>(&SavedFileRow::src_peer_id)
						== int64(peer.value)
					and column<SavedFileRow>(&SavedFileRow::src_msg_id)
						== msg.bare));
			for (const auto &id : ids) {
				storage.remove_all<SourceTagRow>(
					where(column<SourceTagRow>(&SourceTagRow::source_id) == id));
				storage.insert(SourceTagRow{
					.source_id = id,
					.tag = tag.toStdString(),
				});
			}
			storage.commit();
		}
		return matches;
	} catch (const std::exception &ex) {
		LogError(u"Failed to retag source: %1"_q.arg(QString::fromUtf8(ex.what())));
		return 0;
	}
}

} // namespace SexCat
