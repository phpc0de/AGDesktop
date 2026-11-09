# Delivery: QQ1/QQ2/QQ3/QQ5/QQ10/QQ12 batch (commit 41a3bd3dc5 + QQ2/QQ5.1 commit)

## QQ1 — checkpoint mark in chat list (what it is + clip fix)
- The "AG 123/456"-style text beside a chat name is the AyuGram checkpoint
  mark: `AG <current max msg id>/<checkpoint id>`, rendered by
  `ResolveCheckpointMark` (dialogs/ui/dialogs_layout.cpp) for peers with a
  saved checkpoint.
- Display bug fixed: the reserved-width check now accounts for the mute
  icon, and the mark is elided to the actually available width instead of
  running under the row date / being covered.

## QQ3 — restricted-peer tint
- Both tint sites were already shipped (v7.2.9-ayu.20/.21): chat header
  (history_view_top_bar_widget.cpp:748-765) and chat list
  (dialogs_layout.cpp).
- Real defect found and fixed: the active/hovered row overrode the tint
  with dialogsNameFgActive/Over — tint now wins on all states.
- Tint semantics: region-restricted = orange (attentionButtonFg), banned
  channel = red, noforwards = purple, ayu-noforwards = purple variant.

## QQ5.1 — video corner info (duration / size / resolution)
- The top-left chip exists upstream (history_view_video_status.cpp
  PaintVideoCornerStatus, fed by history_view_gif.cpp).
- Two gaps fixed:
  - album/grouped parts suppressed the chip entirely (`_smallGroupPart`)
    — now drawn per part, clipped to its own cell;
  - the disk size only appeared while downloading — the chip text now
    includes `, <size>` for finished videos (duration + size + resolution).

## QQ5.2 — downloaded badge white box
- Root cause: `icons/sexcat/saved.svg` was stroke-based (`fill="none"`)
  which the style codegen recolors via fill → glyph rendered empty, plus
  badge bg was `msgServiceFg` (whitish) with near-white fg.
- Fixed: fill-based glyph + service-blue chip (`msgServiceBg`) with white
  glyph (`msgServiceFg`).

## QQ10 — moved/renamed downloads not auto-relinked
- Shell watch (Windows SHChangeNotify) watched only Desktop + the
  configured download path; when the download setting is the default
  (empty), the default Downloads folder was never watched.
- Fixed: fallback to QStandardPaths DownloadLocation; auto-relocate
  search budget raised (5000→20000 files, depth 2→4).
- New: history window refresh also feeds every missing recent row into
  the relocate search (`SexCat::RelocateMissingEntries`), and `Relink`
  now fires newSaved so the open history window and badges update live.

## QQ12 — jump to unread button
- Already exists: bottom-right corner button (history_view_corner_buttons
  + ui/controls/jump_down_button). Appears when scrolled up or when there
  are unread messages below the viewport; click jumps to the first unread
  message (corner_buttons.cpp downClick → UnreadMessagePosition), Ctrl+click
  to the very bottom. No code change made.

## QQ2 — local folder tags (NEW, per user's chosen design)
- Official-style colored chips under the chat list name, rendered through
  the same machinery as premium folder tags (`Ui::ChatsFilterTag`), tagged
  row height via the existing taggedDialogRow switch.
- Storage: `AyuSettings::localPeerTags/setLocalPeerTags`
  (ayu_settings.json, `localPeerTags` map peerId → space-separated texts).
- Menu: right-click chat → "Set local tags" → "Edit tags…" (space
  separated) / "Clear tags"; chip color derives from a hash of the text.
- Row geometry via `Entry::hasChatsFilterTags` extended to local tags.
