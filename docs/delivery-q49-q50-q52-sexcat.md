# Delivery: Q49 naming / Q50 batch save-as & cancel-all / Q52 hashtag menus

Batch: grill rounds R1-R4, next tag target `v7.2.9-ayu.22` (tag only after
external build machine passes Debug compile + the manual checklist below).
Design decisions: `docs/adr/0006-sexcat-naming-batch-download.md`.

## Verification status

- Static: declarations == definitions == call sites, lang keys present,
  includes/icon references resolved, `git diff --check` clean (evidence in
  session transcript).
- Compile: NOT run in this session (repo rule: no builds here); external
  build machine runs Debug compile.

## What changed

### Q49 - captioned save names (root cause: rename-after-save path drift)

- New primary mechanism is save-TARGET injection, not post-save rename:
  - `SexCat::AdjustDocumentSaveTarget` (ayu/sexcat/sexcat_save_hook.cpp)
    rewrites the numeric server name of a quick-saved document to
    `#tags -description-original` before the loader opens the file.
  - Wired in `DocumentSaveClickHandler::Save` (data/data_file_click_handler.cpp)
    for every non-dialog quick save, and in the batch folder branch
    (menu/menu_item_download_files.cpp).
- `DocumentFileNameForSave` continues to provide uniqueness for the
  original name; the augmented candidate is uniquified again in the same
  directory (` (2)`, ` (3)`...).
- `HandleLoadedDownload` keeps a rename safety net for downloads that
  bypassed the save chain (in-flight retargets, external adds); the
  download-manager entry then keeps pointing at the original path.
- `MaybeAugmentedSavePath` now uses the new format and collects tags plus
  caption from the item AND its album siblings (the album-caption gap is
  fixed at group level, not via `item->emptyText()`).
- Format EXACT: `#抖音 #abc -今天打老虎-516156156.mp4` — tags joined by
  space, then `" -"`, description, `"-"`, original name. Absent segments
  drop their separators. 240-char cap preserves the original name.
- Hashtags come from message entities (`EntityType::Hashtag`) — full
  unicode, reusing the client's own recognition; a unicode regex
  `#([^\s#]{1,64})` with trailing-punctuation trim is the fallback only
  for entity-less texts. ASCII-only `ExtractHashtag` is removed.

### Q50 - batch save-as and one-click cancel

- `AddAction` (menu_item_download_files.cpp) takes const refs and a new
  `forceFolderDialog` flag; multi-select gets a second entry
  "Save selected as..." (ayu_DownloadFilesAs) and the album menu gets
  "Save all album files as..." (ayu_DownloadAlbumAs). Both always open the
  folder dialog regardless of askDownloadPath, and run the same batch tag
  + augmented-name pipeline.
- `DownloadManager::cancelAllLoading()` (public wrapper over the private
  `loadingStop(nullptr)`) cancels everything still downloading; finished
  entries and their files are untouched.
- Entry points: downloads section top-bar menu (only while
  `loadingInProgress()`, `st::menuIconCancel`) and right-click on the
  chat-list download bar (new `DownloadBar::menuRequests()` producer fed
  by a `ContextMenuFilter` event filter on the bar button; dialogs_widget
  opens an ephemeral PopupMenu).

### Q52.1/52.2/52.3/52.4 - hashtag association

- `AddSowTagMenuActions` gains a `ClickHandlerPtr` param; both menu call
  sites pass the active link (`ClickHandler::getActive()` classic,
  `request.link` new UI). Right-clicking "#抖音" offers that exact tag;
  without a cursor link the first tag of the message is offered.
- Multiple hashtags: direct action for the cursor/first tag plus a
  submenu "Save hashtag to sow list" with all remaining tags.
- Selected text is still saved verbatim as a tag name.
- Association stays automatic: any `#任意字符` in a caption becomes a
  sow tag for the saved file (same extraction, so tags saved via Q52.2
  match tags derived from later downloads).
- Q52.4 answer (audit, already in CONTEXT.md §16.4): yes — the Q50 batch
  entries run through the same `SourceTagForItem`/`AdjustDocumentSaveTarget`
  pipeline as Q49.

## Build-machine checklist (Debug)

1. `cmake --build out --config Debug --target Telegram`
2. Single captioned video quick-save (menu "Save"): lands as
   `#抖音 #abc -今天打老虎-516156156.mp4`, no rename afterwards.
3. Same video with only `#抖音`: `#抖音 -516156156.mp4`.
4. Album where the caption sits on another item: name still augmented.
5. Batch multi-select quick-save: each file gets its captioned name.
6. Batch multi-select "Save selected as...": folder dialog always opens;
   chosen folder receives captioned names; one sexcat row per file.
7. Album "Save all album files as...": same as 6 for the whole album.
8. Start several downloads, right-click the download bar → "Cancel all
   downloading": in-flight stop, finished files untouched.
9. Downloads section top-bar menu shows "Cancel all downloading" only
   while something is downloading.
10. Right-click directly on "#抖音" in a message → menu offers
    "Save #抖音 to sow list" (exact tag under cursor).
11. Select text `#关键词` → right-click → "Save selected text to sow list";
    a later download whose caption contains that hashtag associates.
12. Save As dialog keeps the numeric name → post-save rename still applies
    (existing behavior, unchanged).

## Known limits (accepted this round)

- Batch quick-save of files already in cache records nothing in sexcat
  (records are gated on explicit save targets); unchanged behavior.
- Photos are never renamed (documents only).
- When the safety-net rename fires, the download-manager entry keeps the
  original path until restart.
- A rename can fail if another process locks the file at completion time;
  logged via AyuGram log, original name kept.

## Commits

- (this batch, single commit) Save captioned names, batch save-as, cancel-all downloads
