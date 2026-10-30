/*
This file is part of AyuGram for Telegram Desktop,
an unofficial fork of Telegram Desktop.

For license and copyright information please follow this link:
https://<obf:github-repo-ref>/blob/dev/LEGAL
*/
#include "ayu/debug/mtp_tl_codec.h"

#include "ayu/debug/mtp_scheme_table.h"
#include "core/application.h"
#include "data/data_peer.h"
#include "data/data_peer_id.h"
#include "data/data_session.h"
#include "main/main_account.h"
#include "main/main_session.h"

#include <QtCore/QByteArray>
#include <QtCore/QStringList>
#include <QtCore/QVector>

#include <utility>

namespace Ayu::Debug {
namespace {

using Scheme::CtorDesc;

constexpr auto kVectorCtor = 0x15c4b51c; // vector#15c4b51c
constexpr auto kMaxRawHexChars = 4096;

[[nodiscard]] const CtorDesc *FindCtor(uint32_t id) {
	for (auto i = 0; i != Scheme::kCtorsCount; ++i) {
		if (Scheme::kCtors[i].id == id) {
			return &Scheme::kCtors[i];
		}
	}
	return nullptr;
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

[[nodiscard]] bool IsPoly(const QString &type) {
	return type.isEmpty() || type == u"X"_q;
}

[[nodiscard]] QString EscapeBytes(const QByteArray &bytes) {
	auto result = QString();
	result.reserve(bytes.size() + 2);
	for (const auto byte : bytes) {
		const auto code = uchar(byte);
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
	}
	return result;
}

[[nodiscard]] QByteArray UnescapeBytes(const QString &text) {
	auto result = QByteArray();
	result.reserve(text.size());
	for (auto i = 0; i != text.size(); ++i) {
		const auto ch = text[i];
		if (ch != QChar(u'\\') || i + 1 >= text.size()) {
			result += char(ch.unicode());
			continue;
		}
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
// raw packets are debuggable without a second lookup tool.
[[nodiscard]] QString PeerAnnotation(const QString &field, qint64 value) {
	if (!value) {
		return {};
	}
	const auto session = Core::App().activeAccount().maybeSession();
	if (!session) {
		return {};
	}
	auto peerId = PeerId();
	if (field.contains(u"user"_q)) {
		peerId = peerFromUser(UserId(uint64(value)));
	} else if (field.contains(u"chat"_q) && !field.contains(u"channel"_q)) {
		peerId = peerFromChat(ChatId(uint64(value)));
	} else {
		peerId = peerFromChannel(ChannelId(uint64(value)));
	}
	const auto peer = session->data().peerLoaded(peerId);
	if (!peer) {
		return {};
	}
	return u"  [%1]"_q.arg(peer->name());
}

struct Reader {
	const mtpPrime *from = nullptr;
	const mtpPrime *end = nullptr;

	[[nodiscard]] bool remaining(int count) const {
		return from && (end - from) >= count;
	}
};

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
			const auto need = int((length + 3) / 4);
			if (reader.remaining(need)) {
				out = QByteArray(
					reinterpret_cast<const char*>(reader.from),
					int(length));
				reader.from += need;
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
		int depth);

[[nodiscard]] QString DecodeCtorBody(
		Reader &reader,
		const CtorDesc *desc,
		int depth) {
	const auto pad = QString(depth * 2, QChar(u' '));
	auto out = pad + QString::fromUtf8(desc->name) + u" {\n"_q;
	auto flags = quint32(0);
	auto flagsRead = false;
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
			flags = uint32_t(*reader.from++);
			flagsRead = true;
			auto names = QStringList();
			for (auto j = 0; j != int(desc->fieldsCount); ++j) {
				const auto &other = desc->fields[j];
				if (other.flagBit != 0xFFFF
					&& (flags & (quint32(1) << other.flagBit))) {
					names << QString::fromUtf8(other.name);
				}
			}
			out += pad + u"  flags: 0x%1"_q.arg(flags, 8, 16, QChar(u'0'));
			if (!names.isEmpty()) {
				out += u" // "_q + names.join(u','_q);
			}
			out += u"\n"_q;
			continue;
		}
		if (optional) {
			if (!flagsRead || !(flags & (quint32(1) << field.flagBit))) {
				continue;
			}
			if (type == u"true"_q) {
				out += pad + u"  %1: true\n"_q.arg(name);
				continue;
			}
		}
		if (!reader.remaining(1)) {
			reader.from = reader.end;
			out += pad + u"  // <buffer end>\n"_q;
			return out;
		}
		out += DecodeValue(reader, type, name, depth + 1);
	}
	if (depth == 0 && reader.from < reader.end) {
		out += pad + u"  // %1 trailing primes\n"_q.arg(reader.end - reader.from);
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
	const auto desc = FindCtor(id);
	if (!desc) {
		auto out = pad + (fieldName.isEmpty()
			? u"ctor_0x%1 {\n"_q.arg(id, 8, 16, QChar(u'0'))
			: u"%1: ctor_0x%2 {\n"_q
				.arg(fieldName)
				.arg(id, 8, 16, QChar(u'0')));
		auto hex = QString();
		while (reader.remaining(1) && hex.size() < kMaxRawHexChars) {
			hex += u"%1 "_q.arg(uint32_t(*reader.from++), 8, 16, QChar(u'0'));
		}
		out += pad + u"  raw: "_q + hex.trimmed() + u"\n"_q;
		out += pad + u"}\n"_q;
		return out;
	}
	if (!fieldName.isEmpty()) {
		return pad + fieldName + u": "_q
			+ DecodeCtorBody(reader, desc, depth).trimmed() + u"\n"_q;
	}
	return DecodeCtorBody(reader, desc, depth);
}

[[nodiscard]] QString DecodeValue(
		Reader &reader,
		const QString &expect,
		const QString &fieldName,
		int depth) {
	const auto pad = QString(depth * 2, QChar(u' '));
	const auto prefix = fieldName.isEmpty()
		? QString()
		: fieldName + u": "_q;
	if (expect.startsWith(u"Vector<"_q)) {
		const auto element = expect.mid(7, expect.size() - 8);
		if (!reader.remaining(2)) {
			reader.from = reader.end;
			return pad + u"%1// <buffer end>\n"_q.arg(prefix);
		}
		if (uint32_t(reader.from[0]) != kVectorCtor) {
			reader.from += 2;
			return pad + u"%1vector // <not a vector, skipped>\n"_q.arg(prefix);
		}
		reader.from += 2;
		const auto count = int32_t(*reader.from++);
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
					depth + 1);
				if (reader.from == before) {
					break;
				}
			} else {
				out += DecodeCtorValue(reader, QString(), depth + 1);
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
		if (expect == u"string"_q) {
			auto bytes = QByteArray();
			ReadBytes(reader, bytes);
			return pad + u"%1\"%2\"\n"_q.arg(prefix).arg(EscapeBytes(bytes));
		}
		if (expect == u"bytes"_q) {
			auto bytes = QByteArray();
			ReadBytes(reader, bytes);
			return pad + u"%1bytes: 0x%2\n"_q.arg(prefix).arg(HexBytes(bytes));
		}
		if (expect == u"Bool"_q) {
			const auto value = uint32_t(*reader.from++);
			return pad + u"%1%2\n"_q
				.arg(prefix)
				.arg(value == boolTrue ? u"true"_q : u"false"_q);
		}
		if (expect == u"true"_q) {
			return pad + u"%1true\n"_q.arg(prefix);
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
			return pad + u"%1%2\n"_q
				.arg(prefix)
				.arg(QString::number(value, 'g', 17));
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
			return pad + u"%1int128/256: 0x%2\n"_q.arg(prefix).arg(hex);
		}
		if (expect == u"long"_q || expect == u"int53"_q || expect == u"int64"_q) {
			if (!reader.remaining(2)) {
				reader.from = reader.end;
				return pad + u"%1// <buffer end>\n"_q.arg(prefix);
			}
			const auto low = quint64(uint32_t(*reader.from++));
			const auto high = quint64(uint32_t(*reader.from++));
			const auto value = qint64(low | (high << 32));
			return pad + u"%1%2%3\n"_q
				.arg(prefix)
				.arg(value)
				.arg(fieldName.isEmpty()
					? QString()
					: PeerAnnotation(fieldName, value));
		}
		const auto value = qint32(uint32_t(*reader.from++));
		if (expect == u"#"_q) {
			return pad + u"%1flags: 0x%2\n"_q
				.arg(prefix)
				.arg(uint32_t(value), 8, 16, QChar(u'0'));
		}
		return pad + u"%1%2%3\n"_q
			.arg(prefix)
			.arg(value)
			.arg(PeerAnnotation(fieldName, value));
	}
	return DecodeCtorValue(reader, fieldName, depth);
}

[[nodiscard]] QString CleanLine(const QString &raw) {
	auto line = raw.trimmed();
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
			out.push_back(mtpPrime(id));
			const auto raw = nextLine();
			if (!raw.startsWith(u"raw: "_q)) {
				return fail(u"expected raw primes line"_q);
			}
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
			if (hex.startsWith(u"0x"_q)) {
				hex = hex.mid(2);
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
				out.push_back(mtpPrime(boolTrue));
			} else if (value == u"false"_q) {
				out.push_back(mtpPrime(boolFalse));
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
		if (type.startsWith(u"Vector<"_q)) {
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
		const auto flagsSlot = (desc->fieldsCount > 0
			&& QString::fromUtf8(desc->fields[0].type) == u"#"_q)
			? int(out.size())
			: -1;
		out.push_back(mtpPrime(desc->id));
		if (flagsSlot >= 0) {
			out.push_back(mtpPrime(0));
		}
		auto flags = quint32(0);
		for (auto i = 0; i != int(desc->fieldsCount); ++i) {
			const auto &field = desc->fields[i];
			const auto name = QString::fromUtf8(field.name);
			const auto type = QString::fromUtf8(field.type);
			const auto optional = (field.flagBit != 0xFFFF);
			if (type == u"#"_q) {
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
				flags |= (quint32(1) << field.flagBit);
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
		if (flagsSlot >= 0) {
			out[flagsSlot] = mtpPrime(flags);
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
	auto reader = Reader{ from, end };
	if (!reader.remaining(1)) {
		return u"// empty body\n"_q;
	}
	return DecodeValue(reader, QString(), QString(), 0);
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
