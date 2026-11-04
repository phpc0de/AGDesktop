// This is the source code of AyuGram for Desktop.
//
// We do not and cannot prevent the use of our code,
// but be respectful and credit the original author.
//
// Copyright @Radolyn, 2026
#include "ayu/sexcat/sexcat_hash_store.h"

#include "ayu/utils/ayu_log.h"

#include <crl/crl.h>

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>

#include <deque>
#include <mutex>
#include <utility>
#include <vector>

namespace SexCat {
namespace {

constexpr auto kMaxHashFileSize = int64(16) * 1024 * 1024 * 1024;
constexpr auto kHashChunkSize = int64(1024) * 1024;

void LogError(const QString &message) {
	Ayu::Log::Write(u"sexcat"_q, message);
}

QString SidecarPath(const QString &path) {
	return path + u".sexcat.json"_q;
}

bool WritePayload(const QString &target, const QByteArray &payload) {
	auto file = QFile(target);
	if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
		return false;
	}
	return file.write(payload) == payload.size();
}

std::optional<HashHit> ParseAdsPayload(const QByteArray &payload) {
	const auto lines = QString::fromUtf8(payload).split(u'\n');
	if (lines.size() < 2) {
		return std::nullopt;
	}
	const auto sha = QByteArray::fromHex(lines.first().toLatin1());
	if (sha.size() != 32) {
		return std::nullopt;
	}
	const auto parts = lines.at(1).split(u':');
	if (parts.size() != 2) {
		return std::nullopt;
	}
	auto peerOk = false;
	auto msgOk = false;
	const auto peer = parts.at(0).toLongLong(&peerOk);
	const auto msg = parts.at(1).toLongLong(&msgOk);
	if (!peerOk || !msgOk) {
		return std::nullopt;
	}
	auto hit = HashHit();
	hit.sha256 = sha;
	hit.src_peer_id = peer;
	hit.src_msg_id = msg;
	return hit;
}

#ifdef Q_OS_WIN
bool WriteAdsStream(const QString &path, const QByteArray &payload) {
	const auto stream = QDir::toNativeSeparators(path + u":sexcat.sha"_q);
	return WritePayload(stream, payload);
}

std::optional<HashHit> ReadAdsStream(const QString &path) {
	auto file = QFile(QDir::toNativeSeparators(path + u":sexcat.sha"_q));
	if (!file.open(QIODevice::ReadOnly)) {
		return std::nullopt;
	}
	return ParseAdsPayload(file.readAll());
}
#else
bool WriteAdsStream([[maybe_unused]] const QString &path, [[maybe_unused]] const QByteArray &payload) {
	return false;
}

std::optional<HashHit> ReadAdsStream([[maybe_unused]] const QString &path) {
	return std::nullopt;
}
#endif // Q_OS_WIN

std::optional<HashHit> ReadSidecar(const QString &path) {
	auto file = QFile(SidecarPath(path));
	if (!file.open(QIODevice::ReadOnly)) {
		return std::nullopt;
	}
	const auto doc = QJsonDocument::fromJson(file.readAll());
	if (!doc.isObject()) {
		return std::nullopt;
	}
	const auto object = doc.object();
	const auto sha = QByteArray::fromHex(object.value(u"sha256"_q).toString().toLatin1());
	if (sha.size() != 32) {
		return std::nullopt;
	}
	const auto peerValue = object.value(u"srcPeerId"_q);
	const auto msgValue = object.value(u"srcMsgId"_q);
	auto hit = HashHit();
	hit.sha256 = sha;
	hit.src_peer_id = peerValue.isString()
		? peerValue.toString().toLongLong()
		: qint64(peerValue.toDouble());
	hit.src_msg_id = msgValue.isString()
		? msgValue.toString().toLongLong()
		: qint64(msgValue.toDouble());
	return hit;
}

struct HashJob {
	QString path;
	Fn<void(QByteArray sha256)> callback;
};

std::mutex hashMutex;
std::deque<HashJob> hashQueue;
bool hashWorking = false;

QByteArray HashFileBlocking(const QString &path) {
	const auto info = QFileInfo(path);
	if (!info.isFile() || info.size() > kMaxHashFileSize) {
		LogError(u"Refused to hash %1: missing or larger than %2 bytes"_q.arg(path).arg(kMaxHashFileSize));
		return {};
	}
	auto file = QFile(path);
	if (!file.open(QIODevice::ReadOnly)) {
		LogError(u"Failed to open %1 for hashing"_q.arg(path));
		return {};
	}
	auto hasher = QCryptographicHash(QCryptographicHash::Sha256);
	auto buffer = std::vector<char>(size_t(kHashChunkSize));
	while (!file.atEnd()) {
		const auto read = file.read(buffer.data(), kHashChunkSize);
		if (read < 0) {
			LogError(u"Failed to read %1 while hashing"_q.arg(path));
			return {};
		}
		hasher.addData(buffer.data(), qsizetype(read));
	}
	return hasher.result();
}

void DrainHashQueue() {
	while (true) {
		auto job = std::optional<HashJob>();
		{
			std::lock_guard lock(hashMutex);
			if (hashQueue.empty()) {
				hashWorking = false;
				return;
			}
			job = std::move(hashQueue.front());
			hashQueue.pop_front();
		}
		const auto sha256 = HashFileBlocking(job->path);
		crl::on_main([callback = std::move(job->callback), sha256] {
			callback(sha256);
		});
	}
}

} // namespace

namespace HashStore {

bool TrySidecar(const QString &path, const QByteArray &sha256, PeerId src, MsgId msg) {
	if (sha256.size() != 32) {
		return false;
	}
	auto object = QJsonObject();
	object.insert(u"sha256"_q, QString::fromLatin1(sha256.toHex()));
	object.insert(u"srcPeerId"_q, QString::number(src.value));
	object.insert(u"srcMsgId"_q, QString::number(msg.bare));
	return WritePayload(SidecarPath(path), QJsonDocument(object).toJson(QJsonDocument::Compact));
}

bool WriteAds(const QString &path, const QByteArray &sha256, PeerId src, MsgId msg) {
	if (sha256.size() != 32) {
		LogError(u"Refused to persist hash for %1: sha256 must be 32 raw bytes"_q.arg(path));
		return false;
	}
	const auto payload = QString::fromLatin1(sha256.toHex())
		+ u"\n"_q
		+ QString::number(src.value)
		+ u":"_q
		+ QString::number(msg.bare);
	if (WriteAdsStream(path, payload.toUtf8())) {
		return true;
	}
	if (!TrySidecar(path, sha256, src, msg)) {
		LogError(u"Failed to persist hash for %1: ADS and sidecar both failed"_q.arg(path));
		return false;
	}
	return true;
}

std::optional<HashHit> ReadAds(const QString &path) {
	if (auto hit = ReadAdsStream(path)) {
		return hit;
	}
	return ReadSidecar(path);
}

void HashFileAsync(const QString &path, Fn<void(QByteArray sha256)> callback) {
	if (path.isEmpty() || !callback) {
		return;
	}
	if (QFileInfo(path).size() > kMaxHashFileSize) {
		LogError(u"Refused to hash %1: larger than %2 bytes"_q.arg(path).arg(kMaxHashFileSize));
		crl::on_main([callback = std::move(callback)] {
			callback({});
		});
		return;
	}
	{
		std::lock_guard lock(hashMutex);
		hashQueue.push_back(HashJob{ path, std::move(callback) });
		if (hashWorking) {
			return;
		}
		hashWorking = true;
	}
	crl::async(DrainHashQueue);
}

} // namespace HashStore

} // namespace SexCat
