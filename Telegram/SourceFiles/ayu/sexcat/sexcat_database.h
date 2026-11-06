// This is the source code of AyuGram for Desktop.
//
// We do not and cannot prevent the use of our code,
// but be respectful and credit the original author.
//
// Copyright @Radolyn, 2026
#pragma once

#include "data/data_msg_id.h"

#include <QByteArray>
#include <QString>

#include <optional>
#include <vector>

#include <rpl/event_stream.h>

namespace SexCat {

struct Entry {
	int64 id = 0;
	QByteArray sha256;
	QString file_name;
	QString saved_path;
	int64 file_size = 0;
	int64 src_peer_id = 0; // PeerId bits packed like tdesktop PeerId::value
	int64 src_msg_id = 0;
	QString src_title;
	QString src_tag;
	int src_attach = 0;
	int64 saved_at = 0;
};

struct Filter {
	std::optional<int64> src_peer_id;
	QString src_tag;
	QString search;
};

struct TagSummary {
	QString tag;
	int count = 0;
	int64 total_size = 0;
};

class Database final {
public:
	static Database &instance();

	Database(const Database &other) = delete;
	Database &operator=(const Database &other) = delete;

	[[nodiscard]] int64 RecordSaved(Entry entry);
	[[nodiscard]] bool UpdateRowSha(int64 id, const QByteArray &sha256);
	[[nodiscard]] bool DropUnhashedRecord(int64 id);
	[[nodiscard]] std::vector<Entry> LookupUnhashed(int limit);
	[[nodiscard]] std::vector<Entry> LookupByHash(QByteArray sha256);
	[[nodiscard]] bool BackfillSourceByHash(
		const QByteArray &sha256,
		const Entry &source);
	[[nodiscard]] std::vector<Entry> LookupBySource(PeerId peer, MsgId msg);
	[[nodiscard]] std::optional<Entry> FindByPath(
		const QString &savedPath,
		int64 fileSize);
	[[nodiscard]] std::optional<Entry> LookupByPath(const QString &savedPath);
	[[nodiscard]] std::optional<Entry> FindSavedSource(
		PeerId peer,
		MsgId msg,
		int attach,
		int64 fileSize);
	// Q80: pending rows are written at download start and match the
	// (peer, msg, attach) position before any sha exists.
	[[nodiscard]] int64 RecordPendingPosition(const Entry &source);
	[[nodiscard]] int64 ConsumePendingPosition(
		const Entry &source,
		const QString &savedPath);
	void DropPendingPosition(const Entry &source);
	[[nodiscard]] std::vector<Entry> LookupByPathPrefix(const QString &prefix);
	[[nodiscard]] bool Relink(int64 id, const QString &newPath);
	[[nodiscard]] std::vector<Entry> Recent(
		int limit,
		Filter filter = {},
		int offset = 0);
	[[nodiscard]] bool RecordTag(const QString &tag);
	[[nodiscard]] std::vector<TagSummary> TagSummaries(int limit);
	[[nodiscard]] bool RenameTag(const QString &from, const QString &to);
	[[nodiscard]] bool DeleteTag(const QString &tag);
	// Q81: drop one record by itself, siblings stay untouched.
	[[nodiscard]] int64 DropRecordByPath(const QString &savedPath);
	[[nodiscard]] bool DropRecordById(int64 id);
	[[nodiscard]] int RetagSource(PeerId peer, MsgId msg, const QString &tag);

	[[nodiscard]] bool Enabled() const;
	[[nodiscard]] rpl::producer<Entry> newSaved() const;

private:
	Database();
	~Database();

	rpl::event_stream<Entry> _newSaved;
	bool _enabled = false;

};

} // namespace SexCat
