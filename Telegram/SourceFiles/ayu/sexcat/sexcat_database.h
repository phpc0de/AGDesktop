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
	int src_attach = 0;
	int64 saved_at = 0;
};

struct Filter {
	std::optional<int64> src_peer_id;
	QString search;
};

class Database final {
public:
	static Database &instance();

	Database(const Database &other) = delete;
	Database &operator=(const Database &other) = delete;

	[[nodiscard]] bool RecordSaved(Entry entry);
	[[nodiscard]] std::vector<Entry> LookupByHash(QByteArray sha256);
	[[nodiscard]] std::vector<Entry> LookupBySource(PeerId peer, MsgId msg);
	[[nodiscard]] std::vector<Entry> Recent(int limit, Filter filter = {});

	[[nodiscard]] bool Enabled() const;
	[[nodiscard]] rpl::producer<Entry> newSaved() const;

private:
	Database();
	~Database();

	rpl::event_stream<Entry> _newSaved;
	bool _enabled = false;

};

} // namespace SexCat
