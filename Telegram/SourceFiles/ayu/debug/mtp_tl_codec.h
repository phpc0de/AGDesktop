/*
This file is part of AyuGram for Telegram Desktop,
an unofficial fork of Telegram Desktop.

For license and copyright information please follow this link:
https://github.com/AyuGram/AyuGramDesktop/blob/dev/LEGAL
*/
#pragma once

#include "mtproto/core_types.h"

class QString;
template <typename T>
class QVector;

namespace Ayu::Debug {

// Renders one TL object as an indented, re-parsable text tree.
// Unknown ctors degrade to raw hex blocks and never throw.
QString DecodeTlBody(const mtpPrime *from, const mtpPrime *end);

// Best-effort ctor name for a list row: unwraps message/msg_container/
// rpc_result envelopes so rows show the real request/response ctor.
QString DecodeTlRowName(const QVector<mtpPrime> &body);

// Removes transport envelope primes (message#5bb8e5a5 / rpc_result) so
// recorded responses decode as the real payload ctor.
void StripTlEnvelope(const mtpPrime *&from, const mtpPrime *&end);

// Parses the DecodeTlBody text format back into primes. Optional `#`
// flags fields are recomputed from which optional fields are present.
bool ParseTlText(
	const QString &text,
	QVector<mtpPrime> &out,
	QString &error);

} // namespace Ayu::Debug
