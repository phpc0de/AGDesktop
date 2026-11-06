/*
This file is part of AyuGram for Telegram Desktop,
an unofficial fork of Telegram Desktop.

For license and copyright information please follow this link:
https://<obf:github-repo-ref>/blob/dev/LEGAL
*/
#include "ayu/debug/mtp_tl_codec.h"

#include "ayu/debug/mtp_ctor_notes.h"
#include "ayu/debug/mtp_scheme_docs.h"
#include "ayu/debug/mtp_scheme_table.h"
#include "core/application.h"
#include "data/data_peer.h"
#include "data/data_peer_id.h"
#include "data/data_session.h"
#include "main/main_account.h"
#include "main/main_domain.h"
#include "main/main_session.h"
#include "scheme.h"

#include <QtCore/QByteArray>
#include <QtCore/QDateTime>
#include <QtCore/QHash>
#include <QtCore/QStringList>
#include <QtCore/QVector>

#include <array>
#include <utility>

namespace Ayu::Debug {
namespace {

using Scheme::CtorDesc;

constexpr auto kVectorCtor = 0x1cb5c415; // vector#1cb5c415
constexpr auto kMaxRawHexChars = 4096;
constexpr auto kMaxCtorDepth = 6;
constexpr auto kMaxTextChars = 24000;
constexpr auto kMaxFlagGroups = 8;
constexpr auto kMaxHeuristicDepth = 3;
constexpr auto kResyncWindow = 256;
constexpr auto kMediaPayloadBytes = 10 * 1024;
constexpr auto kMinDateValue = 1'000'000'000;
constexpr auto kApiLayer = 229; // keep in sync with "// LAYER 229" at the end of api.tl
constexpr uint32_t kMediaCtorIds[] = {
	0x2331b22d, // photoEmpty
	0xfb197a65, // photo
	0x36f8c871, // documentEmpty
	0x8fd4c4d8, // document
	0xe216eb63, // messageMediaPhoto
	0x52d8ccd9, // messageMediaDocument
};

// The transport envelope ctors are commented out of mtproto.tl (parsed
// manually upstream), so they are provided here for decoding only.
constexpr Scheme::FieldDesc kEnvelopeMessageFields[] = {
	{"msg_id", "long", 0xFFFF},
	{"seqno", "int", 0xFFFF},
	{"bytes", "int", 0xFFFF},
	{"body", "Object", 0xFFFF},
};
constexpr Scheme::FieldDesc kEnvelopeContainerFields[] = {
	{"messages", "Vector<message>", 0xFFFF},
};
constexpr Scheme::FieldDesc kEnvelopeCopyFields[] = {
	{"orig_message", "Message", 0xFFFF},
};
constexpr Scheme::FieldDesc kEnvelopeGzipFields[] = {
	{"packed_data", "string", 0xFFFF},
};
constexpr Scheme::FieldDesc kEnvelopeRpcResultFields[] = {
	{"req_msg_id", "long", 0xFFFF},
	{"result", "Object", 0xFFFF},
};
constexpr Scheme::CtorDesc kEnvelopeCtors[] = {
	{0x5bb8e5a5, "message", "Message", 4u, kEnvelopeMessageFields},
	{0x73f1f8dc, "msg_container", "MessageContainer", 1u, kEnvelopeContainerFields},
	{0xe06046b2, "msg_copy", "MessageCopy", 1u, kEnvelopeCopyFields},
	{0x3072cfa1, "gzip_packed", "Object", 1u, kEnvelopeGzipFields},
	{0xf35c6d01, "rpc_result", "RpcResult", 2u, kEnvelopeRpcResultFields},
};

[[nodiscard]] const CtorDesc *FindCtor(uint32_t id) {
	static const auto index = [] {
		auto hash = QHash<uint32_t, const CtorDesc *>();
		hash.reserve(Scheme::kCtorsCount + 5);
		for (auto i = 0; i != Scheme::kCtorsCount; ++i) {
			// First table entry wins, matching the old linear scan.
			if (!hash.contains(Scheme::kCtors[i].id)) {
				hash.insert(Scheme::kCtors[i].id, &Scheme::kCtors[i]);
			}
		}
		for (const auto &envelope : kEnvelopeCtors) {
			if (!hash.contains(envelope.id)) {
				hash.insert(envelope.id, &envelope);
			}
		}
		return hash;
	}();
	return index.value(id, nullptr);
}

[[nodiscard]] const CtorDesc *FindCtor(const QString &name) {
	const auto utf8 = name.toUtf8();
	for (auto i = 0; i != Scheme::kCtorsCount; ++i) {
		if (!qstrcmp(Scheme::kCtors[i].name, utf8.constData())) {
			return &Scheme::kCtors[i];
		}
	}
	return nullptr;
}

[[nodiscard]] QString CtorName(uint32_t id) {
	if (const auto desc = FindCtor(id)) {
		return QString::fromUtf8(desc->name);
	}
	return u"ctor_0x%1"_q.arg(id, 8, 16, QChar(u'0'));
}

[[nodiscard]] bool IsPrimitive(const QString &type) {
	return type == u"int"_q
		|| type == u"int32"_q
		|| type == u"#"_q
		|| type == u"long"_q
		|| type == u"int53"_q
		|| type == u"int64"_q
		|| type == u"double"_q
		|| type == u"int128"_q
		|| type == u"int256"_q
		|| type == u"string"_q
		|| type == u"bytes"_q
		|| type == u"Bool"_q
		|| type == u"true"_q;
}

[[nodiscard]] QString EscapeBytes(const QByteArray &bytes) {
	auto result = QString();
	result.reserve(bytes.size() + 2);
	const auto escapeAscii = [&](uchar code) {
		switch (code) {
		case '\\': result += u"\\\\"_q; break;
		case '"': result += u"\\\""_q; break;
		case '\n': result += u"\\n"_q; break;
		case '\r': result += u"\\r"_q; break;
		case '\t': result += u"\\t"_q; break;
		default:
			if (code < 0x20 || code == 0x7F) {
				result += u"\\x%1"_q.arg(code, 2, 16, QChar(u'0'));
			} else {
				result += QChar(code);
			}
			break;
		}
	};
	// WHY: strings are UTF-8 on the wire; mapping single bytes to QChars
	// renders mojibake, so valid runs surface as real characters and only
	// invalid bytes fall back to \xNN.
	const auto text = QString::fromUtf8(bytes);
	if (text.toUtf8() == bytes) {
		auto plain = QString();
		const auto flush = [&] {
			if (!plain.isEmpty()) {
				result += plain;
				plain.clear();
			}
		};
		for (const auto &ch : text) {
			if (ch.unicode() >= 0x80) {
				plain += ch;
			} else {
				flush();
				escapeAscii(uchar(ch.unicode()));
			}
		}
		flush();
	} else {
		for (const auto byte : bytes) {
			const auto code = uchar(byte);
			if (code >= 0x80) {
				result += u"\\x%1"_q.arg(code, 2, 16, QChar(u'0'));
			} else {
				escapeAscii(code);
			}
		}
	}
	return result;
}

[[nodiscard]] QByteArray UnescapeBytes(const QString &text) {
	auto result = QByteArray();
	auto plain = QString();
	const auto flush = [&] {
		if (!plain.isEmpty()) {
			result += plain.toUtf8();
			plain.clear();
		}
	};
	for (auto i = 0; i != text.size(); ++i) {
		const auto ch = text[i];
		if (ch != QChar(u'\\') || i + 1 >= text.size()) {
			plain += ch;
			continue;
		}
		flush();
		const auto next = text[++i];
		switch (next.unicode()) {
		case '\\': result += '\\'; break;
		case '"': result += '"'; break;
		case 'n': result += '\n'; break;
		case 'r': result += '\r'; break;
		case 't': result += '\t'; break;
		case 'x': {
			auto ok = false;
			const auto hex = text.mid(i + 1, 2).toInt(&ok, 16);
			if (ok) {
				result += char(hex);
				i += 2;
			} else {
				result += 'x';
			}
		} break;
		default: result += char(next.unicode()); break;
		}
	}
	flush();
	return result;
}

[[nodiscard]] QString HexBytes(const QByteArray &bytes) {
	auto result = QString();
	result.reserve(bytes.size() * 2 + 2);
	for (const auto byte : bytes) {
		result += u"%1"_q.arg(uchar(byte), 2, 16, QChar(u'0'));
	}
	return result;
}

// Resolves user_id / chat_id / channel_id into a readable peer name so
// raw packets are debuggable without a second lookup tool. `owner` is the
// enclosing ctor name or vector element type, disambiguating generic ids.
[[nodiscard]] QString PeerAnnotation(
		const QString &field,
		qint64 value,
		const QString &owner) {
	if (!value) {
		return {};
	}
	const auto session = Core::App().domain().started()
		? Core::App().activeAccount().maybeSession()
		: nullptr;
	if (!session) {
		return {};
	}
	auto kinds = QStringList();
	const auto push = [&](const QString &kind) {
		if (!kinds.contains(kind)) {
			kinds << kind;
		}
	};
	const auto route = [&](const QString &text) {
		if (text.contains(u"user"_q)) {
			push(u"user"_q);
		} else if (text.contains(u"channel"_q)) {
			push(u"channel"_q);
		} else if (text.contains(u"chat"_q)) {
			push(u"chat"_q);
		}
	};
	route(field);
	route(owner);
	// WHY: generic `id` fields carry no kind in their name and the owner
	// route can still miss, so id-like fields are probed user→chat→channel
	// via peerLoaded; probing every value would mislabel dates and limits.
	if (field == u"id"_q || field.endsWith(u"_id"_q)) {
		for (const auto kind : { u"user"_q, u"chat"_q, u"channel"_q }) {
			push(kind);
		}
	}
	for (const auto &kind : kinds) {
		auto peerId = PeerId();
		if (kind == u"user"_q) {
			peerId = peerFromUser(UserId(uint64(value)));
		} else if (kind == u"chat"_q) {
			peerId = peerFromChat(ChatId(uint64(value)));
		} else {
			peerId = peerFromChannel(ChannelId(uint64(value)));
		}
		if (const auto peer = session->data().peerLoaded(peerId)) {
			return u"  [%1]"_q.arg(peer->name());
		}
	}
	return {};
}

// One-line media summary scraped from the already-rendered ctor body.
[[nodiscard]] QString MediaSummary(const QString &ctor, const QString &body) {
	const auto isPhoto = (ctor == u"photo"_q)
		|| (ctor == u"photoEmpty"_q)
		|| (ctor == u"messageMediaPhoto"_q);
	const auto isDocument = (ctor == u"document"_q)
		|| (ctor == u"documentEmpty"_q)
		|| (ctor == u"messageMediaDocument"_q);
	if (!isPhoto && !isDocument) {
		return {};
	}
	auto id = QString();
	auto size = QString();
	auto mime = QString();
	auto bestW = 0;
	auto bestH = 0;
	auto bestArea = 0;
	auto pendingW = -1;
	for (const auto &raw : body.split(u'\n')) {
		const auto sep = raw.indexOf(u": "_q);
		if (sep <= 0) {
			continue;
		}
		const auto field = raw.left(sep).trimmed();
		const auto value = raw.mid(sep + 2).section(u' ', 0, 0);
		if (field == u"w"_q) {
			pendingW = value.toInt();
		} else if (field == u"h"_q) {
			const auto area = pendingW * value.toInt();
			if (area > bestArea) {
				bestArea = area;
				bestW = pendingW;
				bestH = value.toInt();
			}
			pendingW = -1;
		} else if (field == u"id"_q && id.isEmpty()) {
			id = value;
		} else if (field == u"size"_q && size.isEmpty()) {
			size = value;
		} else if (field == u"mime_type"_q && mime.isEmpty()) {
			mime = (value.size() > 1
				&& value.startsWith(u'"')
				&& value.endsWith(u'"'))
				? value.mid(1, value.size() - 2)
				: value;
		}
	}
	if (id.isEmpty()) {
		return {};
	}
	auto text = isPhoto
		? u"[photo id=%1"_q.arg(id)
		: u"[document id=%1"_q.arg(id);
	if (!size.isEmpty()) {
		text += u" size=%1"_q.arg(size);
	}
	if (isPhoto && bestArea > 0) {
		text += u" %1x%2"_q.arg(bestW).arg(bestH);
	}
	if (!isPhoto && !mime.isEmpty()) {
		text += u" mime=%1"_q.arg(mime);
	}
	return text + QChar(u']');
}

[[nodiscard]] QString FieldDocNote(const CtorDoc &doc, const QString &field) {
	for (const auto &entry : QString::fromUtf8(doc.fields).split(u';')) {
		const auto sep = entry.indexOf(u':');
		if (sep > 0 && entry.left(sep).trimmed() == field) {
			return entry.mid(sep + 1).trimmed();
		}
	}
	return {};
}

struct Reader {
	const mtpPrime *from = nullptr;
	const mtpPrime *end = nullptr;
	int used = 0;
	const mtpPrime *base = nullptr;

	[[nodiscard]] bool remaining(int count) const {
		return from && (end - from) >= count;
	}
};

[[nodiscard]] int WordOffset(const Reader &reader) {
	return reader.base ? int(reader.from - reader.base) : 0;
}

// Multi-flags ctors (user#..., flags + flags2) select bits by group; the
// generator encodes a flagsN reference as (N << 8) | bit.
[[nodiscard]] int FlagGroupOf(const QString &name) {
	if (name == u"flags"_q) {
		return 0;
	}
	if (name.startsWith(u"flags"_q)) {
		auto ok = false;
		const auto group = name.mid(5).toInt(&ok);
		if (ok && group > 0 && group < kMaxFlagGroups) {
			return group;
		}
	}
	return 0;
}

[[nodiscard]] bool StringHeaderValid(const Reader &reader) {
	const auto header = uint32_t(reader.from[0]);
	const auto lengthLow = int(header & 0xFF);
	if (lengthLow == 255) {
		return false;
	}
	const auto length = (lengthLow == 254)
		? int((header >> 8) & 0xFFFFFF)
		: lengthLow;
	const auto words = (lengthLow == 254)
		? 1 + (length + 3) / 4
		: (length + 4) / 4;
	return words <= reader.end - reader.from;
}

[[nodiscard]] bool PrintableUtf8(const QByteArray &bytes) {
	const auto text = QString::fromUtf8(bytes);
	if (text.toUtf8() != bytes) {
		return false;
	}
	for (const auto &ch : text) {
		if (ch.unicode() < 0x20 || ch.unicode() == 0x7F) {
			return false;
		}
	}
	return true;
}

// A walk string header must carry content bytes exactly where the short
// form stores them, so [small int, 0] long pairs are not read as strings.
[[nodiscard]] bool StringHeaderPlausible(uint32_t header) {
	const auto lengthLow = int(header & 0xFF);
	if (lengthLow >= 254) {
		return lengthLow == 254;
	}
	for (auto k = 0; k != 3; ++k) {
		const auto byte = int((header >> (8 + k * 8)) & 0xFF);
		if (k < qMin(lengthLow, 3) ? byte < 0x20 : byte != 0) {
			return false;
		}
	}
	return true;
}

// Validates and consumes a TL string/bytes blob at the reader position.
[[nodiscard]] bool TryReadString(Reader &reader, QByteArray &out) {
	if (!reader.remaining(1)) {
		return false;
	}
	const auto header = uint32_t(*reader.from);
	const auto lengthLow = int(header & 0xFF);
	if (lengthLow == 255) {
		return false;
	}
	const auto length = (lengthLow == 254)
		? int((header >> 8) & 0xFFFFFF)
		: lengthLow;
	const auto dataWords = (lengthLow == 254)
		? (length + 3) / 4
		: qMax((length + 4) / 4 - 1, 0);
	if (!reader.remaining(1 + dataWords)) {
		return false;
	}
	const auto begin = reinterpret_cast<const char*>(reader.from);
	reader.from += 1 + dataWords;
	out = (lengthLow == 254)
		? QByteArray(begin + 4, length)
		: QByteArray(begin + 1, length);
	return true;
}

[[nodiscard]] QString Counted(Reader &reader, QString text) {
	reader.used += int(text.size());
	return text;
}

void ReadBytes(Reader &reader, QByteArray &out) {
	if (!reader.remaining(1)) {
		return;
	}
	const auto first = uint32_t(*reader.from++);
	const auto lengthLow = int(first & 0xFF);
	if (lengthLow == 254) {
		const auto length = int((first >> 8) & 0xFFFFFF);
		const auto need = (length + 3) / 4;
		if (reader.remaining(need)) {
			out = QByteArray(
				reinterpret_cast<const char*>(reader.from),
				length);
			reader.from += need;
		} else {
			reader.from = reader.end;
		}
	} else if (lengthLow == 255) {
		if (reader.remaining(3)) {
			const auto length = qint64((first >> 8) & 0xFFFFFF)
				| (qint64(uint32_t(reader.from[0])) << 24)
				| (qint64(uint32_t(reader.from[1]) & 0xFF) << 56);
			reader.from += 1;
			const auto need = (length + 3) / 4;
			if (need >= 0
				&& need <= int64(reader.end - reader.from)) {
				out = QByteArray(
					reinterpret_cast<const char*>(reader.from),
					int(length));
				reader.from += int(need);
			} else {
				reader.from = reader.end;
			}
		} else {
			reader.from = reader.end;
		}
	} else {
		const auto length = lengthLow;
		const auto need = (length + 4) / 4;
		if (need <= 0 || !reader.remaining(need - 1)) {
			reader.from = reader.end;
			return;
		}
		auto raw = QByteArray(need * 4, 0);
		raw[0] = char(first & 0xFF);
		raw[1] = char((first >> 8) & 0xFF);
		raw[2] = char((first >> 16) & 0xFF);
		raw[3] = char((first >> 24) & 0xFF);
		memcpy(
			raw.data() + 4,
			reinterpret_cast<const char*>(reader.from),
			(need - 1) * 4);
		reader.from += need - 1;
		out = raw.mid(1, length);
	}
}

void PackBytes(const QByteArray &bytes, QVector<mtpPrime> &out) {
	const auto pushData = [&](const char *data, int length) {
		const auto primes = (length + 3) / 4;
		for (auto i = 0; i != primes; ++i) {
			auto value = quint32(0);
			for (auto k = 0; k != 4; ++k) {
				const auto index = i * 4 + k;
				if (index < length) {
					value |= quint32(uchar(data[index])) << (k * 8);
				}
			}
			out.push_back(mtpPrime(value));
		}
	};
	const auto length = int(bytes.size());
	if (length < 254) {
		auto header = quint32(length);
		for (auto k = 0; k != 3 && k < length; ++k) {
			header |= quint32(uchar(bytes[k])) << (8 + k * 8);
		}
		out.push_back(mtpPrime(header));
		if (length > 3) {
			pushData(bytes.constData() + 3, length - 3);
		}
	} else {
		out.push_back(mtpPrime(quint32(254)
			| ((quint32(length) & 0xFF) << 8)
			| ((quint32(length) >> 8 & 0xFF) << 16)
			| ((quint32(length) >> 16 & 0xFF) << 24)));
		pushData(bytes.constData(), length);
	}
}

[[nodiscard]] QString DecodeValue(
		Reader &reader,
		const QString &expect,
		const QString &fieldName,
		int depth,
		const QString &owner);

[[nodiscard]] QString DecodeCtorValue(
		Reader &reader,
		const QString &fieldName,
		int depth);

[[nodiscard]] QString DecodeCtorBody(
		Reader &reader,
		const CtorDesc *desc,
		int depth) {
	const auto pad = QString(depth * 2, QChar(u' '));
	const auto ctorName = QString::fromUtf8(desc->name);
	const auto utf8Name = ctorName.toUtf8();
	const auto doc = FindCtorDoc(utf8Name.constData());
	auto out = pad + ctorName + u" {"_q;
	const auto ctorNote = FindCtorNote(utf8Name.constData());
	const auto purpose = ctorNote
		? QString::fromUtf8(ctorNote->purpose)
		: (doc ? QString::fromUtf8(doc->purpose) : QString());
	if (!purpose.isEmpty()) {
		out += u" // 用途："_q + purpose;
	}
	out += u"\n"_q;
	const auto attachNote = [&](QString &line, const QString &field) {
		auto note = doc ? FieldDocNote(*doc, field) : QString();
		if (note.isEmpty()) {
			const auto utf8 = field.toUtf8();
			if (const auto known = FindFieldNote(utf8.constData())) {
				note = QString::fromUtf8(known->note);
			}
		}
		if (!note.isEmpty()) {
			const auto nl = line.indexOf(u'\n');
			line.insert((nl >= 0) ? nl : line.size(), u" // "_q + note);
		}
	};
	auto groupFlags = std::array<quint32, kMaxFlagGroups>{};
	auto groupRead = std::array<bool, kMaxFlagGroups>{};
	for (auto i = 0; i != int(desc->fieldsCount); ++i) {
		const auto &field = desc->fields[i];
		const auto name = QString::fromUtf8(field.name);
		const auto type = QString::fromUtf8(field.type);
		const auto optional = (field.flagBit != 0xFFFF);
		if (type == u"#"_q) {
			if (!reader.remaining(1)) {
				reader.from = reader.end;
				out += pad + u"  // <buffer end>\n"_q;
				return out;
			}
			const auto group = FlagGroupOf(name);
			groupFlags[group] = uint32_t(*reader.from++);
			groupRead[group] = true;
			auto names = QStringList();
			for (auto j = 0; j != int(desc->fieldsCount); ++j) {
				const auto &other = desc->fields[j];
				if (other.flagBit != 0xFFFF
					&& (other.flagBit >> 8) == group
					&& (groupFlags[group]
						& (quint32(1) << (other.flagBit & 0xFF)))) {
					names << QString::fromUtf8(other.name);
				}
			}
			out += pad + u"  %1: 0x%2"_q
				.arg(name)
				.arg(groupFlags[group], 8, 16, QChar(u'0'));
			if (!names.isEmpty()) {
				out += u" // "_q + names.join(QChar(u','));
			}
			out += u"\n"_q;
			continue;
		}
		if (optional) {
			const auto group = field.flagBit >> 8;
			const auto bit = field.flagBit & 0xFF;
			if (!groupRead[group]
				|| !(groupFlags[group] & (quint32(1) << bit))) {
				continue;
			}
			if (type == u"true"_q) {
				// WHY: flags.N?true is virtual — the bit alone carries it,
				// so consuming a word here shifts every later field.
				auto line = pad + u"  %1: true\n"_q.arg(name);
				attachNote(line, name);
				out += line;
				continue;
			}
		}
		if (!reader.remaining(1)) {
			reader.from = reader.end;
			out += pad + u"  // <buffer end>\n"_q;
			return out;
		}
		auto rendered = DecodeValue(reader, type, name, depth + 1, ctorName);
		attachNote(rendered, name);
		out += rendered;
		if (reader.used > kMaxTextChars) {
			out += pad + u"  // <output limit>\n"_q;
			break;
		}
	}
	if (depth == 0 && reader.from < reader.end) {
		out += pad + u"  // %1 trailing primes\n"_q.arg(reader.end - reader.from);
	}
	if (const auto summary = MediaSummary(ctorName, out); !summary.isEmpty()) {
		out += pad + u"  // "_q + summary + u"\n"_q;
	}
	out += pad + u"}\n"_q;
	return out;
}

[[nodiscard]] QString RawCtorValue(
		Reader &reader,
		uint32_t id,
		const QString &fieldName,
		int depth) {
	const auto pad = QString(depth * 2, QChar(u' '));
	auto out = pad + (fieldName.isEmpty()
		? u"ctor_0x%1 {\n"_q.arg(id, 8, 16, QChar(u'0'))
		: u"%1: ctor_0x%2 {\n"_q
			.arg(fieldName)
			.arg(id, 8, 16, QChar(u'0')));
	auto hex = QString();
	auto ascii = QByteArray();
	while (reader.remaining(1) && hex.size() < kMaxRawHexChars) {
		const auto word = uint32_t(*reader.from++);
		hex += u"%1 "_q.arg(word, 8, 16, QChar(u'0'));
		for (auto shift = 0; shift != 32; shift += 8) {
			const auto byte = char((word >> shift) & 0xFF);
			ascii += (byte >= 0x20 && byte < 0x7F) ? byte : '.';
		}
	}
	out += pad + u"  raw: "_q + hex.trimmed() + u"\n"_q;
	if (!ascii.isEmpty()) {
		out += pad + u"  text: \""_q
			+ QString::fromLatin1(ascii) + u"\"\n"_q;
	}
	out += pad + u"}\n"_q;
	return Counted(reader, std::move(out));
}

[[nodiscard]] bool IsMediaCtor(uint32_t id) {
	for (const auto media : kMediaCtorIds) {
		if (media == id) {
			return true;
		}
	}
	return false;
}

[[nodiscard]] bool IsDateField(const QString &name) {
	return name == u"date"_q
		|| name == u"expires"_q
		|| name == u"was_online"_q
		|| name.endsWith(u"_date"_q)
		|| name.endsWith(u"_at"_q);
}

// The wire data contradicting the declared field type means an earlier
// field was decoded at a wrong width. Bool/true/# are exact words.
[[nodiscard]] bool WireContradicts(
		const Reader &reader,
		const QString &expect) {
	if (expect == u"Bool"_q || expect == u"true"_q || expect == u"#"_q) {
		return false;
	}
	if (expect == u"string"_q || expect == u"bytes"_q) {
		return !StringHeaderValid(reader);
	}
	const auto word = uint32_t(reader.from[0]);
	return word == kVectorCtor || FindCtor(word) != nullptr;
}

// A word qualifies as a bare int only when small or date-like; random
// hashes fall through to resync instead of eating the rest of the payload.
[[nodiscard]] bool PlausibleInt(qint32 value) {
	return (value > -0x1000000 && value < 0x1000000)
		|| value >= kMinDateValue;
}

// Skips forward to the next known ctor id / vector header and resumes
// decoding there; the abandoned gap is reported as unparsed words.
[[nodiscard]] bool TryResync(Reader &reader, int depth, QString &out) {
	const auto pad = QString(depth * 2, QChar(u' '));
	const auto limit = qMin(int(reader.end - reader.from), kResyncWindow);
	auto skipped = 0;
	while (skipped != limit) {
		const auto word = uint32_t(reader.from[skipped]);
		if (word == kVectorCtor || FindCtor(word)) {
			break;
		}
		++skipped;
	}
	if (skipped == limit) {
		return false;
	}
	if (skipped > 0) {
		out += Counted(reader, pad + u"// 未能解析 %1 words\n"_q.arg(skipped));
		reader.from += skipped;
	}
	out += DecodeCtorValue(reader, QString(), depth);
	return true;
}

[[nodiscard]] QString HeuristicJunk(Reader &reader, int depth) {
	const auto pad = QString(depth * 2, QChar(u' '));
	const auto offset = WordOffset(reader);
	const auto word = uint32_t(*reader.from++);
	return Counted(reader, pad + u"%1: raw 0x%2\n"_q
		.arg(offset)
		.arg(word, 8, 16, QChar(u'0')));
}

[[nodiscard]] QString HeuristicVector(Reader &reader, int depth);

// Renders one generic TL value (bool / ctor / vector / string / long /
// double / int) at the current prime; false means binary junk.
[[nodiscard]] bool HeuristicScalar(Reader &reader, int depth, QString &out) {
	const auto pad = QString(depth * 2, QChar(u' '));
	const auto offset = WordOffset(reader);
	const auto word = uint32_t(reader.from[0]);
	if (word == mtpc_boolTrue || word == mtpc_boolFalse) {
		++reader.from;
		out = Counted(reader, pad + u"%1: %2\n"_q.arg(offset).arg(
			(word == mtpc_boolTrue) ? u"true"_q : u"false"_q));
		return true;
	}
	if (word == kVectorCtor) {
		out = HeuristicVector(reader, depth);
		return true;
	}
	if (FindCtor(word)) {
		out = pad + u"%1: "_q.arg(offset)
			+ DecodeCtorValue(reader, QString(), depth).trimmed() + u"\n"_q;
		return true;
	}
	auto bytes = QByteArray();
	if (StringHeaderPlausible(word) && TryReadString(reader, bytes)) {
		auto text = PrintableUtf8(bytes)
			? EscapeBytes(bytes)
			: (u"hex[%1]: 0x%2%3"_q
				.arg(bytes.size())
				.arg(HexBytes(bytes.left(16)))
				.arg(bytes.size() > 16 ? u"…"_q : QString()));
		if (text.size() > 512) {
			text = text.left(512) + u"…"_q;
		}
		out = Counted(reader, pad + u"%1: \"%2\"\n"_q.arg(offset).arg(text));
		return true;
	}
	if (reader.remaining(2)
		&& word != 0
		&& word < 0x01000000
		&& uint32_t(reader.from[1]) == 0) {
		reader.from += 2;
		out = Counted(reader, pad + u"%1: long %2\n"_q
			.arg(offset)
			.arg(qint64(word)));
		return true;
	}
	if (reader.remaining(2)) {
		const auto exponent = int((uint32_t(reader.from[1]) >> 20) & 0x7FF);
		if (exponent >= 0x3F5 && exponent <= 0x412) {
			const auto bits = quint64(word)
				| (quint64(uint32_t(reader.from[1])) << 32);
			auto value = 0.0;
			memcpy(&value, &bits, sizeof(value));
			reader.from += 2;
			out = Counted(reader, pad + u"%1: double %2\n"_q
				.arg(offset)
				.arg(QString::number(value, 'g', 17)));
			return true;
		}
	}
	if (PlausibleInt(qint32(word))) {
		++reader.from;
		auto line = u"%1: %2"_q.arg(offset).arg(qint32(word));
		if (qint32(word) >= kMinDateValue) {
			line += u" // %1"_q.arg(QDateTime::fromSecsSinceEpoch(
				qint64(qint32(word)),
				Qt::UTC).toString(u"yyyy-MM-dd hh:mm"_q));
		}
		out = Counted(reader, pad + line + u"\n"_q);
		return true;
	}
	return false;
}

[[nodiscard]] QString HeuristicVector(Reader &reader, int depth) {
	const auto pad = QString(depth * 2, QChar(u' '));
	if (depth > kMaxCtorDepth) {
		return pad + u"vector // <depth limit>\n"_q;
	}
	const auto offset = WordOffset(reader);
	const auto count = int32_t(*reader.from++);
	if (count < 0 || count > reader.end - reader.from) {
		return Counted(reader, pad
			+ u"%1: vector // <bad count %2, skipped>\n"_q
				.arg(offset)
				.arg(count));
	}
	auto out = Counted(reader, pad + u"%1: vector[%2] {\n"_q
		.arg(offset)
		.arg(count));
	for (auto i = 0; i != count; ++i) {
		if (!reader.remaining(1)) {
			out += pad + u"  // <buffer end at item %1>\n"_q.arg(i);
			break;
		}
		auto piece = QString();
		if (!HeuristicScalar(reader, depth + 1, piece)) {
			auto resync = QString();
			piece = TryResync(reader, depth + 1, resync)
				? std::move(resync)
				: HeuristicJunk(reader, depth + 1);
		}
		out += piece;
		if (reader.used > kMaxTextChars) {
			out += pad + u"  // <output limit>\n"_q;
			break;
		}
	}
	out += pad + u"}\n"_q;
	return out;
}

[[nodiscard]] QString HeuristicWalk(Reader &reader, int depth) {
	const auto pad = QString(depth * 2, QChar(u' '));
	auto out = QString();
	while (reader.remaining(1)) {
		if (reader.used > kMaxTextChars) {
			out += pad + u"// <output limit>\n"_q;
			break;
		}
		auto piece = QString();
		if (!HeuristicScalar(reader, depth, piece)) {
			auto resync = QString();
			piece = TryResync(reader, depth, resync)
				? std::move(resync)
				: HeuristicJunk(reader, depth);
		}
		out += piece;
	}
	return out;
}

// Unknown ctor: walk the payload as generic TL values, not bare hex.
[[nodiscard]] QString UnknownCtorValue(
		Reader &reader,
		uint32_t id,
		const QString &fieldName,
		int depth) {
	const auto pad = QString(depth * 2, QChar(u' '));
	if (depth > kMaxHeuristicDepth) {
		return RawCtorValue(reader, id, fieldName, depth);
	}
	auto out = Counted(reader, pad + (fieldName.isEmpty()
		? u"ctor_0x%1 {\n"_q.arg(id, 8, 16, QChar(u'0'))
		: u"%1: ctor_0x%2 {\n"_q.arg(fieldName).arg(id, 8, 16, QChar(u'0'))));
	const auto words = int(reader.end - reader.from);
	if (IsMediaCtor(id) || words * 4 > kMediaPayloadBytes) {
		reader.from = reader.end;
		out += Counted(reader, pad
			+ u"  // media omitted (%1 bytes)\n"_q.arg(words * 4 + 4));
	} else {
		out += HeuristicWalk(reader, depth + 1);
	}
	out += pad + u"}\n"_q;
	return out;
}

// Element type is unknown here: ctor elements recurse, rest stay words.
[[nodiscard]] QString DecodeUnknownVector(
		Reader &reader,
		const QString &fieldName,
		int depth) {
	const auto pad = QString(depth * 2, QChar(u' '));
	const auto prefix = fieldName.isEmpty()
		? QString()
		: fieldName + u": "_q;
	if (depth > kMaxCtorDepth) {
		return pad + u"%1vector // <depth limit>\n"_q.arg(prefix);
	}
	if (!reader.remaining(1)) {
		reader.from = reader.end;
		return pad + u"%1// <buffer end>\n"_q.arg(prefix);
	}
	const auto count = int32_t(*reader.from++);
	// A garbage count would run primes past the payload; skip instead.
	if (count < 0 || count > reader.end - reader.from) {
		return pad + u"%1vector // <bad count %2, skipped>\n"_q
			.arg(prefix)
			.arg(count);
	}
	auto out = pad + u"%1vector {\n"_q.arg(prefix);
	for (auto i = 0; i != count; ++i) {
		if (!reader.remaining(1)) {
			out += pad + u"  // <buffer end at item %1>\n"_q.arg(i);
			break;
		}
		const auto id = uint32_t(*reader.from++);
		if (id == kVectorCtor) {
			out += DecodeUnknownVector(reader, QString(), depth + 1);
		} else if (const auto desc = FindCtor(id)) {
			out += DecodeCtorBody(reader, desc, depth + 1);
		} else {
			out += pad + u"  %1\n"_q.arg(int32_t(id));
		}
		if (reader.used > kMaxTextChars) {
			out += pad + u"  // <output limit>\n"_q;
			break;
		}
	}
	out += pad + u"}\n"_q;
	return out;
}

[[nodiscard]] QString DecodeCtorValue(
		Reader &reader,
		const QString &fieldName,
		int depth) {
	const auto pad = QString(depth * 2, QChar(u' '));
	if (!reader.remaining(1)) {
		reader.from = reader.end;
		return pad + u"// <buffer end>\n"_q;
	}
	const auto id = uint32_t(*reader.from++);
	if (id == kVectorCtor) {
		return DecodeUnknownVector(reader, fieldName, depth);
	}
	if (depth > kMaxCtorDepth) {
		return RawCtorValue(reader, id, fieldName, depth);
	}
	if (const auto desc = FindCtor(id)) {
		if (!fieldName.isEmpty()) {
			return pad + fieldName + u": "_q
				+ DecodeCtorBody(reader, desc, depth).trimmed() + u"\n"_q;
		}
		return DecodeCtorBody(reader, desc, depth);
	}
	return UnknownCtorValue(reader, id, fieldName, depth);
}

[[nodiscard]] QString DecodeValue(
		Reader &reader,
		const QString &expect,
		const QString &fieldName,
		int depth,
		const QString &owner) {
	const auto pad = QString(depth * 2, QChar(u' '));
	const auto prefix = fieldName.isEmpty()
		? QString()
		: fieldName + u": "_q;
	if (expect.startsWith(u"Vector<"_q) || expect.startsWith(u"vector<"_q)) {
		const auto element = expect.mid(7, expect.size() - 8);
		if (!reader.remaining(2)) {
			reader.from = reader.end;
			return pad + u"%1// <buffer end>\n"_q.arg(prefix);
		}
		if (uint32_t(reader.from[0]) != kVectorCtor) {
			reader.from += 2;
			return pad + u"%1vector // <not a vector, skipped>\n"_q.arg(prefix);
		}
		++reader.from;
		const auto count = int32_t(*reader.from++);
		// A garbage count would run primes past the payload; skip instead.
		if (count < 0 || count > reader.end - reader.from) {
			return pad + u"%1vector // <bad count %2, skipped>\n"_q
				.arg(prefix)
				.arg(count);
		}
		auto out = pad + u"%1vector {\n"_q.arg(prefix);
		for (auto i = 0; i != count; ++i) {
			if (!reader.remaining(1)) {
				out += pad + u"  // <buffer end at item %1>\n"_q.arg(i);
				break;
			}
			if (IsPrimitive(element)) {
				const auto before = reader.from;
				out += DecodeValue(
					reader,
					element,
					QString(),
					depth + 1,
					element);
				if (reader.from == before) {
					break;
				}
			} else {
				out += DecodeCtorValue(reader, QString(), depth + 1);
			}
			if (reader.used > kMaxTextChars) {
				out += pad + u"  // <output limit>\n"_q;
				break;
			}
		}
		out += pad + u"}\n"_q;
		return out;
	}
	if (IsPrimitive(expect)) {
		if (!reader.remaining(1)) {
			reader.from = reader.end;
			return pad + u"%1// <buffer end>\n"_q.arg(prefix);
		}
		if (WireContradicts(reader, expect)) {
			const auto offset = WordOffset(reader);
			auto resync = QString();
			if (TryResync(reader, depth, resync)) {
				return Counted(reader, pad + prefix
					+ u"// ⚠ 字段错位自上一字段（offset %1）\n"_q.arg(offset))
					+ resync;
			}
		}
		if (expect == u"string"_q) {
			auto bytes = QByteArray();
			ReadBytes(reader, bytes);
			auto text = EscapeBytes(bytes);
			// A single huge value must not consume the whole budget.
			const auto room = qMax(kMaxTextChars - reader.used, 0);
			if (text.size() > room) {
				text = text.left(room) + u"… <output limit>"_q;
			}
			return Counted(reader, pad + u"%1\"%2\"\n"_q.arg(prefix).arg(text));
		}
		if (expect == u"bytes"_q) {
			auto bytes = QByteArray();
			ReadBytes(reader, bytes);
			const auto shown = qMin<qsizetype>(bytes.size(), 32);
			auto line = pad + u"%1bytes[%2]: 0x%3%4"_q
				.arg(prefix)
				.arg(bytes.size())
				.arg(HexBytes(bytes.left(shown)))
				.arg(bytes.size() > 32 ? u"…"_q : QString());
			if (fieldName == u"file_reference"_q && !bytes.isEmpty()) {
				// WHY: the ticket is opaque on the wire; the base64 copy
				// lets tickets be compared across packets without a tool.
				line += u" // opaque access ticket, not decodable (%1 bytes)"_q
					.arg(bytes.size());
				line += u"\n"_q + pad + u"// base64: "_q
					+ QString::fromLatin1(bytes.toBase64());
			}
			return Counted(reader, line + u"\n"_q);
		}
		if (expect == u"Bool"_q) {
			const auto value = uint32_t(*reader.from++);
			const auto text = (value == mtpc_boolTrue)
				? u"true"_q
				: (value == mtpc_boolFalse)
				? u"false"_q
				: u"false <marker 0x%1>"_q.arg(value, 8, 16, QChar(u'0'));
			return Counted(reader, pad + u"%1%2\n"_q.arg(prefix).arg(text));
		}
		if (expect == u"true"_q) {
			return Counted(reader, pad + u"%1true\n"_q.arg(prefix));
		}
		if (expect == u"double"_q) {
			if (!reader.remaining(2)) {
				reader.from = reader.end;
				return pad + u"%1// <buffer end>\n"_q.arg(prefix);
			}
			const auto low = quint64(uint32_t(*reader.from++));
			const auto high = quint64(uint32_t(*reader.from++));
			const auto bits = low | (high << 32);
			auto value = 0.0;
			memcpy(&value, &bits, sizeof(value));
			return Counted(reader, pad + u"%1%2\n"_q
				.arg(prefix)
				.arg(QString::number(value, 'g', 17)));
		}
		if (expect == u"int128"_q || expect == u"int256"_q) {
			const auto count = (expect == u"int128"_q) ? 4 : 8;
			if (!reader.remaining(count)) {
				reader.from = reader.end;
				return pad + u"%1// <buffer end>\n"_q.arg(prefix);
			}
			auto hex = QString();
			for (auto i = 0; i != count; ++i) {
				hex += u"%1"_q
					.arg(uint32_t(*reader.from++), 8, 16, QChar(u'0'));
			}
			return Counted(
				reader,
				pad + u"%1int128/256: 0x%2\n"_q.arg(prefix).arg(hex));
		}
		if (expect == u"long"_q || expect == u"int53"_q || expect == u"int64"_q) {
			if (!reader.remaining(2)) {
				reader.from = reader.end;
				return pad + u"%1// <buffer end>\n"_q.arg(prefix);
			}
			const auto low = quint64(uint32_t(*reader.from++));
			const auto high = quint64(uint32_t(*reader.from++));
			const auto value = qint64(low | (high << 32));
			return Counted(reader, pad + u"%1%2%3\n"_q
				.arg(prefix)
				.arg(value)
				.arg(fieldName.isEmpty()
					? QString()
					: PeerAnnotation(fieldName, value, owner)));
		}
		const auto value = qint32(uint32_t(*reader.from++));
		if (expect == u"#"_q) {
			return Counted(reader, pad + u"%1flags: 0x%2\n"_q
				.arg(prefix)
				.arg(uint32_t(value), 8, 16, QChar(u'0')));
		}
		auto line = pad + u"%1%2"_q.arg(prefix).arg(value)
			+ PeerAnnotation(fieldName, value, owner);
		if (IsDateField(fieldName) && value >= kMinDateValue) {
			line += u" // %1"_q.arg(QDateTime::fromSecsSinceEpoch(
				qint64(value),
				Qt::UTC).toString(u"yyyy-MM-dd hh:mm"_q));
		}
		return Counted(reader, line + u"\n"_q);
	}
	return DecodeCtorValue(reader, fieldName, depth);
}

[[nodiscard]] QString CleanLine(const QString &raw) {
	auto line = raw.trimmed();
	if (line.startsWith(u"//"_q)) {
		return {};
	}
	const auto comment = line.indexOf(u" //"_q);
	return (comment >= 0) ? line.left(comment).trimmed() : line;
}

class TlParser {
public:
	TlParser(const QString &text, QString &error) : _error(error) {
		_lines = text.split(u'\n');
	}

	bool parse(QVector<mtpPrime> &out) {
		const auto line = nextLine();
		if (line.isEmpty()) {
			return fail(u"empty input"_q);
		}
		if (line.startsWith(u"ctor_0x"_q) && line.endsWith(u" {"_q)) {
			auto ok = false;
			const auto id = line.mid(7, line.size() - 9).toUInt(&ok, 16);
			if (!ok) {
				return fail(u"bad unknown ctor id"_q);
			}
			const auto raw = nextLine();
			if (!raw.startsWith(u"raw: "_q)) {
				// WHY: heuristic unknown-ctor blocks show structure but
				// carry no raw primes; skip to the block end and continue.
				--_pos;
				auto depth = 1;
				auto guard = 0;
				while (depth != 0 && ++guard != 10'000) {
					const auto line = nextLine();
					if (line.isEmpty()) {
						return fail(u"unterminated ctor_0x block"_q);
					}
					const auto trimmed = line.trimmed();
					depth += (trimmed.endsWith(u'{') ? 1 : 0)
						- (trimmed == u"}" ? 1 : 0);
				}
				if (depth != 0) {
					return fail(u"unterminated ctor_0x block"_q);
				}
				return true;
			}
			out.push_back(mtpPrime(id));
			for (const auto &token : raw.mid(5).split(u' ', Qt::SkipEmptyParts)) {
				const auto value = token.toUInt(&ok, 16);
				if (!ok) {
					return fail(u"bad raw prime %1"_q.arg(token));
				}
				out.push_back(mtpPrime(value));
			}
			return true;
		}
		if (!line.endsWith(u" {"_q)) {
			return fail(u"expected a constructor block"_q);
		}
		const auto name = line.left(line.size() - 1).trimmed();
		const auto desc = FindCtor(name);
		if (!desc) {
			return fail(u"unknown constructor %1"_q.arg(name));
		}
		if (!parseCtor(desc, out)) {
			return false;
		}
		return atEnd();
	}


private:
	[[nodiscard]] QString nextLine() {
		while (_pos < _lines.size()) {
			const auto clean = CleanLine(_lines[_pos++]);
			if (!clean.isEmpty()) {
				return clean;
			}
		}
		return QString();
	}

	[[nodiscard]] bool atEnd() {
		return nextLine().isEmpty();
	}

	bool fail(const QString &message) {
		_error = message;
		return false;
	}

	bool parsePrimitive(
			const QString &type,
			const QString &value,
			QVector<mtpPrime> &out) {
		auto ok = false;
		if (type == u"int"_q || type == u"int32"_q || type == u"#"_q) {
			const auto parsed = value.toLongLong(&ok, 0);
			if (!ok) {
				return fail(u"bad int value %1"_q.arg(value));
			}
			out.push_back(mtpPrime(quint32(qint32(parsed))));
		} else if (type == u"long"_q || type == u"int53"_q || type == u"int64"_q) {
			const auto parsed = value.toLongLong(&ok, 0);
			if (!ok) {
				return fail(u"bad long value %1"_q.arg(value));
			}
			const auto bits = quint64(parsed);
			out.push_back(mtpPrime(quint32(bits & 0xFFFFFFFF)));
			out.push_back(mtpPrime(quint32(bits >> 32)));
		} else if (type == u"double"_q) {
			const auto parsed = value.toDouble(&ok);
			if (!ok) {
				return fail(u"bad double value %1"_q.arg(value));
			}
			auto bits = quint64(0);
			memcpy(&bits, &parsed, sizeof(bits));
			out.push_back(mtpPrime(quint32(bits & 0xFFFFFFFF)));
			out.push_back(mtpPrime(quint32(bits >> 32)));
		} else if (type == u"int128"_q || type == u"int256"_q) {
			auto hex = value;
			if (hex.startsWith(u"0x"_q)) {
				hex = hex.mid(2);
			}
			const auto target = (type == u"int128"_q) ? 16 : 32;
			if (hex.size() % 2 != 0 || hex.size() / 2 > target) {
				return fail(u"bad %1 value"_q.arg(type));
			}
			auto bytes = QByteArray(target, 0);
			for (auto i = 0; i != hex.size() / 2; ++i) {
				bytes[i] = char(hex.mid(i * 2, 2).toUInt(&ok, 16));
				if (!ok) {
					return fail(u"bad hex byte"_q);
				}
			}
			for (auto i = 0; i != target; i += 4) {
				auto prime = quint32(0);
				for (auto k = 0; k != 4; ++k) {
					prime |= quint32(uchar(bytes[i + k])) << (k * 8);
				}
				out.push_back(mtpPrime(prime));
			}
		} else if (type == u"string"_q) {
			if (value.size() < 2
				|| !value.startsWith(u'"')
				|| !value.endsWith(u'"')) {
				return fail(u"string must be quoted"_q);
			}
			PackBytes(UnescapeBytes(value.mid(1, value.size() - 2)), out);
		} else if (type == u"bytes"_q) {
			auto hex = value;
			if (hex.startsWith(u"bytes["_q)) {
				const auto sep = hex.indexOf(u"]: "_q);
				if (sep < 0) {
					return fail(u"bad bytes header %1"_q.arg(value));
				}
				hex = hex.mid(sep + 3);
			}
			if (hex.startsWith(u"0x"_q)) {
				hex = hex.mid(2);
			}
			if (hex.endsWith(u"…"_q)) {
				return fail(u"bytes preview is truncated"_q);
			}
			if (hex.size() % 2 != 0) {
				return fail(u"bytes must be even-length hex"_q);
			}
			auto bytes = QByteArray(hex.size() / 2, 0);
			for (auto i = 0; i != bytes.size(); ++i) {
				bytes[i] = char(hex.mid(i * 2, 2).toUInt(&ok, 16));
				if (!ok) {
					return fail(u"bad hex byte"_q);
				}
			}
			PackBytes(bytes, out);
		} else if (type == u"Bool"_q || type == u"true"_q) {
			if (value == u"true"_q) {
				out.push_back(mtpPrime(mtpc_boolTrue));
			} else if (value == u"false"_q) {
				out.push_back(mtpPrime(mtpc_boolFalse));
			} else {
				return fail(u"expected true/false"_q);
			}
		} else {
			return fail(u"unsupported primitive %1"_q.arg(type));
		}
		return true;
	}

	bool parseValue(
			const QString &type,
			const QString &value,
			QVector<mtpPrime> &out) {
		if (IsPrimitive(type)) {
			return parsePrimitive(type, value, out);
		}
		if (type.startsWith(u"Vector<"_q) || type.startsWith(u"vector<"_q)) {
			if (!value.endsWith(u" {"_q)
				|| value.left(value.size() - 1).trimmed() != u"vector") {
				return fail(u"expected vector block"_q);
			}
			const auto element = type.mid(7, type.size() - 8);
			out.push_back(mtpPrime(kVectorCtor));
			const auto countSlot = out.size();
			out.push_back(mtpPrime(0));
			auto count = 0;
			while (true) {
				const auto line = nextLine();
				if (line.isEmpty()) {
					return fail(u"unterminated vector"_q);
				}
				if (line == u"}"_q) {
					break;
				}
				if (line.endsWith(u" {"_q)) {
					const auto nested = line.left(line.size() - 1).trimmed();
					const auto desc = FindCtor(nested);
					if (!desc) {
						return fail(u"unknown ctor %1"_q.arg(nested));
					}
					out.push_back(mtpPrime(desc->id));
					if (!parseCtor(desc, out)) {
						return false;
					}
				} else if (!parsePrimitive(element, line, out)) {
					return false;
				}
				++count;
			}
			out[countSlot] = mtpPrime(quint32(count));
			return true;
		}
		if (!value.endsWith(u" {"_q)) {
			return fail(u"expected nested constructor block"_q);
		}
		const auto name = value.left(value.size() - 1).trimmed();
		const auto desc = FindCtor(name);
		if (!desc) {
			return fail(u"unknown constructor %1"_q.arg(name));
		}
		return parseCtor(desc, out);
	}

	bool parseCtor(const CtorDesc *desc, QVector<mtpPrime> &out) {
		out.push_back(mtpPrime(desc->id));
		auto groupSlots = std::array<int, kMaxFlagGroups>{};
		auto groupFlags = std::array<quint32, kMaxFlagGroups>{};
		groupSlots.fill(-1);
		for (auto i = 0; i != int(desc->fieldsCount); ++i) {
			const auto &field = desc->fields[i];
			const auto name = QString::fromUtf8(field.name);
			const auto type = QString::fromUtf8(field.type);
			const auto optional = (field.flagBit != 0xFFFF);
			if (type == u"#"_q) {
				groupSlots[FlagGroupOf(name)] = int(out.size());
				out.push_back(mtpPrime(0));
				const auto line = nextLine();
				if (!line.isEmpty() && !line.startsWith(u"flags"_q)) {
					--_pos;
				}
				continue;
			}
			const auto line = nextLine();
			if (line.isEmpty()) {
				return fail(u"unexpected end inside %1"_q.arg(
					QString::fromUtf8(desc->name)));
			}
			if (line == u"}"_q) {
				return fail(u"missing field %1"_q.arg(name));
			}
			auto value = line;
			if (const auto sep = line.indexOf(u": "_q); sep >= 0) {
				if (line.left(sep) != name) {
					if (optional) {
						// Optional field absent: put the line back and go on.
						--_pos;
						continue;
					}
					return fail(u"expected field %1, got %2"_q
						.arg(name)
						.arg(line.left(sep)));
				}
				value = line.mid(sep + 2);
			} else if (!optional) {
				return fail(u"expected field %1"_q.arg(name));
			} else {
				--_pos;
				continue;
			}
			if (optional) {
				groupFlags[field.flagBit >> 8]
					|= (quint32(1) << (field.flagBit & 0xFF));
				if (type == u"true"_q) {
					continue;
				}
			}
			if (value.endsWith(u" {"_q)) {
				// Nested block: its fields follow on the next lines.
				if (!parseValue(type, value, out)) {
					return false;
				}
				continue;
			}
			if (!IsPrimitive(type) || !parsePrimitive(type, value, out)) {
				return fail(u"expected block value for %1"_q.arg(name));
			}
		}
		// WHY: flags are the first wire field, so they are recomputed
		// only after every optional field's presence is known.
		for (auto g = 0; g != kMaxFlagGroups; ++g) {
			if (groupSlots[g] >= 0) {
				out[groupSlots[g]] = mtpPrime(groupFlags[g]);
			}
		}
		const auto closer = nextLine();
		if (closer != u"}"_q && !closer.isEmpty()) {
			return fail(u"unexpected trailing data in %1"_q.arg(
				QString::fromUtf8(desc->name)));
		}
		return true;
	}

	QStringList _lines;
	int _pos = 0;
	int flagsSlot = -1;
	QString &_error;

};

} // namespace

void StripTlEnvelope(const mtpPrime *&from, const mtpPrime *&end) {
	const auto message = FindCtor(u"message"_q);
	const auto container = FindCtor(u"msg_container"_q);
	const auto result = FindCtor(u"rpc_result"_q);
	// WHY: recorded responses may or may not carry the transport envelope
	// depending on the recording point, so peeling is idempotent and
	// bounded rather than a fixed offset.
	for (auto guard = 0; guard != 16; ++guard) {
		if (end - from < 5) {
			return;
		}
		const auto ctor = uint32_t(*from);
		if (message && ctor == message->id) {
			from += 4;
		} else if (container && ctor == container->id) {
			from += 2;
		} else if (result && ctor == result->id) {
			from += 3;
		} else {
			return;
		}
	}
}

QString DecodeTlBody(const mtpPrime *from, const mtpPrime *end) {
	auto reader = Reader{ from, end, 0, from };
	if (!reader.remaining(1)) {
		return u"// empty body\n"_q;
	}
	return u"// layer=%1\n"_q.arg(kApiLayer)
		+ DecodeValue(reader, QString(), QString(), 0, QString());
}

QString DecodeTlRowName(const QVector<mtpPrime> &body) {
	if (body.isEmpty()) {
		return {};
	}
	auto from = body.constData();
	auto end = from + body.size();
	StripTlEnvelope(from, end);
	for (auto guard = 0; guard != 16 && from < end; ++guard) {
		const auto ctor = uint32_t(*from);
		const auto desc = FindCtor(ctor);
		if (!desc) {
			return CtorName(ctor);
		}
		const auto name = QString::fromUtf8(desc->name);
		if (name == u"message"_q) {
			from += 4;
		} else if (name == u"msg_container"_q) {
			from += 2;
		} else if (name == u"rpc_result"_q) {
			from += 3;
		} else {
			return name;
		}
	}
	return {};
}

bool ParseTlText(
		const QString &text,
		QVector<mtpPrime> &out,
		QString &error) {
	auto parser = TlParser(text, error);
	out.clear();
	auto result = QVector<mtpPrime>();
	if (!parser.parse(result)) {
		out.clear();
		return false;
	}
	out = std::move(result);
	error = QString();
	return true;
}

} // namespace Ayu::Debug
