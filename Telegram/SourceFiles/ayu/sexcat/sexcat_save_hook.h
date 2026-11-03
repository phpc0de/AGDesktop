// This is the source code of AyuGram for Desktop.
//
// We do not and cannot prevent the use of our code,
// but be respectful and credit the original author.
//
// Copyright @Radolyn, 2026
#pragma once

#include "data/data_types.h"

class DocumentData;
class PhotoData;

namespace SexCat {

// Failures only log, the save itself is never affected.
void RecordSaveAttempt(
	const QString &savedPath,
	not_null<DocumentData*> document,
	FullMsgId contextId = {},
	int attachIndex = 0);
void RecordPhotoSaveAttempt(
	const QString &savedPath,
	not_null<PhotoData*> photo,
	FullMsgId contextId = {},
	int attachIndex = 0);

void MarkDocumentSaveAs(
	not_null<DocumentData*> document,
	FullMsgId contextId);
void NotifyDocumentSaved(
	const QString &savedPath,
	not_null<DocumentData*> document);
void ForgetDoctrineSaveAs(not_null<DocumentData*> document);

// Installs the download-manager hook once; safe to call repeatedly.
void EnsureDownloadListener();

} // namespace SexCat
