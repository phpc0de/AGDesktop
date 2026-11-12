// This is the source code of AyuGram for Desktop.
//
// We do not and cannot prevent the use of our code,
// but be respectful and credit the original author.
//
// Copyright @Radolyn, 2026
#pragma once

#include "ui/basic_click_handlers.h"
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

// Q49: server name, or the id-composed name for nameless documents.
[[nodiscard]] QString FallbackDocumentName(not_null<DocumentData*> document);

[[nodiscard]] QString AugmentedPhotoSavePath(
	const QString &dir,
	int index,
	HistoryItem *item);

// Q49: augmented "#tags -description-original" name, or empty when the
// naming rules don't apply. The source item comes from contextId or the
// latest save-as marker of this document.
[[nodiscard]] QString AugmentedDocumentName(
	not_null<DocumentData*> document,
	FullMsgId contextId = {});

// Q49: pre-write save target fix. A quick-saved document whose server
// name is bare digits gets a captioned name before the loader opens the
// file, so no post-save rename is needed. Returns targetPath unchanged
// when the naming rules don't apply.
[[nodiscard]] QString AdjustDocumentSaveTarget(
	const QString &targetPath,
	not_null<DocumentData*> document,
	FullMsgId contextId);

// Toast when the dedup gate skipped a download the library already has.
void NotifyDuplicateSkipped(const QString &path);

// Sow-list menu: save a #hashtag from the message, the one under the
// cursor (link), or the selected text as a tag name. Adds nothing when
// there is nothing to save.
void AddSowTagMenuActions(
	not_null<Ui::PopupMenu*> menu,
	HistoryItem *item,
	const QString &selectedText,
	const ClickHandlerPtr &link = {});

} // namespace SexCat

namespace Ayu::SexCat {

// In-process registry of documents whose file reached local storage,
// feeding the media downloaded badge.
void RememberDownloadedDocument(not_null<const DocumentData*> document);
[[nodiscard]] bool IsDownloadedDocument(not_null<const DocumentData*> document);

} // namespace Ayu::SexCat
