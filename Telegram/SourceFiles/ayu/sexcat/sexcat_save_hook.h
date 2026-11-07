// This is the source code of AyuGram for Desktop.
//
// We do not and cannot prevent the use of our code,
// but be respectful and credit the original author.
//
// Copyright @Radolyn, 2026
#pragma once

#include "data/data_types.h"

#include <QByteArray>
#include <QString>

class DocumentData;
class PhotoData;
class HistoryItem;

namespace Ui {
class PopupMenu;
} // namespace Ui

namespace SexCat {

// Failures only log, the save itself is never affected.
void RecordSaveAttempt(
	const QString &savedPath,
	not_null<DocumentData*> document,
	FullMsgId contextId = {},
	int attachIndex = 0,
	const QString &sourceTag = {});
void RecordPhotoSaveAttempt(
	const QString &savedPath,
	not_null<PhotoData*> photo,
	FullMsgId contextId = {},
	int attachIndex = 0,
	const QString &sourceTag = {});

void MarkDocumentSaveAs(
	not_null<DocumentData*> document,
	FullMsgId contextId,
	const QString &sourceTag = {});
void NotifyDocumentSaved(
	const QString &savedPath,
	not_null<DocumentData*> document);
void ForgetDoctrineSaveAs(not_null<DocumentData*> document);

[[nodiscard]] bool ShouldSkipDuplicateDownload(
	not_null<DocumentData*> document,
	const QString &toFile);

// Recorded save path of a library entry for this attachment, or empty.
[[nodiscard]] QString RecordedDocumentPath(
	not_null<DocumentData*> document,
	FullMsgId contextId);

// True when the library has a source row for this attachment, even if
// the recorded file no longer exists at its path.
[[nodiscard]] bool HasRecordedSource(
	not_null<DocumentData*> document,
	FullMsgId contextId);

// Hash of a finished download, already computed by the loader.
void NoteDownloadedSha(const QString &path, const QByteArray &sha256);
[[nodiscard]] bool ShouldTrackDownloadHash();

// Installs the download-manager hook once; safe to call repeatedly.
void EnsureDownloadListener();

// First #hashtag of the item text (group caption included), or empty.
[[nodiscard]] QString SourceTagForItem(HistoryItem *item);

// Sow-list menu: save a #hashtag from the message or the selected text
// as a tag name. Adds nothing when there is nothing to save.
void AddSowTagMenuActions(
	not_null<Ui::PopupMenu*> menu,
	HistoryItem *item,
	const QString &selectedText);

} // namespace SexCat

namespace Ayu::SexCat {

// In-process registry of documents whose file reached local storage,
// feeding the media downloaded badge.
void RememberDownloadedDocument(not_null<const DocumentData*> document);
[[nodiscard]] bool IsDownloadedDocument(not_null<const DocumentData*> document);

} // namespace Ayu::SexCat
