// This is the source code of AyuGram for Desktop.
//
// We do not and cannot prevent the use of our code,
// but be respectful and credit the original author.
//
// Copyright @Radolyn, 2026
#pragma once

#include "base/basic_types.h"
#include "data/data_msg_id.h"

#include <QByteArray>
#include <QString>

#include <optional>

namespace SexCat {

struct HashHit {
	QByteArray sha256; // raw 32 bytes
	int64 src_peer_id = 0;
	int64 src_msg_id = 0;
};

namespace HashStore {

bool WriteAds(const QString &path, const QByteArray &sha256, PeerId src, MsgId msg);
bool TrySidecar(const QString &path, const QByteArray &sha256, PeerId src, MsgId msg);
[[nodiscard]] std::optional<HashHit> ReadAds(const QString &path);
void HashFileAsync(const QString &path, Fn<void(QByteArray sha256)> callback);
[[nodiscard]] QByteArray ComputeFileSha256(const QString &path);

} // namespace HashStore

} // namespace SexCat
