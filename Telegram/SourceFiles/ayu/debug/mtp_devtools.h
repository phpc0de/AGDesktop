// This is the source code of AyuGram for Desktop.
//
// We do not and cannot prevent the use of our code,
// but be respectful and credit the original author.
//
// Copyright @Radolyn, 2026
#pragma once

#include "mtproto/core_types.h"
#include "mtproto/details/mtproto_serialized_request.h"
#include "rpl/event_stream.h"

namespace MTP {
struct Error;
} // namespace MTP

namespace Ayu::Debug {

struct MtpEntry {
	mtpRequestId requestId = 0;
	crl::time when = 0;
	int kind = 0; // 0 = request, 1 = response, 2 = error
	QString name;
	QString text;
	QVector<mtpPrime> body;
	ShiftedDcId dc = 0;
};

[[nodiscard]] std::vector<MtpEntry> MtpEntriesSnapshot();
[[nodiscard]] rpl::producer<> MtpEntriesUpdates();
void RecordMtpRequest(
	mtpRequestId id,
	const MTP::details::SerializedRequest &request);
void RecordMtpResponse(mtpRequestId id, const mtpBuffer &reply);
void RecordMtpError(mtpRequestId id, const MTP::Error &error);
void ReplayMtpRequest(mtpRequestId id);
void ClearMtpEntries();
void ShowMtpDevtools();

} // namespace Ayu::Debug
