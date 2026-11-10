# 交付：Q49/Q50 第 5 轮（写盘点改名 + 相圈子集多选）

> 依据 ADR-0006 Amendment 1 与 `docs/plan/q49-write-point-naming.md`。本会话零构建；验证 = 静态交叉核验 + 外部构建机实测。

## 改动点（HEAD 基线 7c1a6e1ff7）

| 文件 | 改动 | 对应缺口 |
|---|---|---|
| `ayu/sexcat/sexcat_save_hook.h/.cpp` | 新增 `AugmentedDocumentName` / `BuildCaptionedNameFor`（含 PendingMap 兜底 + 诊断日志）；`AdjustDocumentSaveTarget` 改为薄封装；删 `kPendingSaveAsTtl` + `PruneStalePending`（标记免 TTL，HC）；新增 `NotifyDuplicateSkipped` | HC、诊断 |
| `data/data_document.h/.cpp` | `saveFromData/Silent/Checked` 带 `contextId`；`saveFromDataChecked` 落盘前过 `AdjustDocumentSaveTarget`（写盘点，H1 根治）；去重跳过分支只在文件真实存在时 `setLocation` + toast（HA 虚指修复） | H1、HA、R6 |
| `data/data_file_click_handler.cpp` | 另存对话框默认名直接给增补名（用户可改）；`MarkDocumentSaveAs` 去 `!loading()` 守卫（HD）；Save 入口 AyuLog 诊断行 | HD、R5 |
| `media/view/media_view_overlay_widget.cpp` | 静态分支 `saveFromDataSilent` 传 `_message->fullId()`（GIF/图片扩面） | R6 |
| `data/data_document_resolver.cpp` | `saveFromDataSilent` 传 `item->fullId()` | R6 |
| `menu/menu_item_download_files.cpp` | 抽 `ChooseFolderAndSave` 公共调度器；新增 `AddDownloadVideosSelector`（右键相册 → "Select videos to download…" 子菜单，逐视频"序号 · 大小"点击勾选 + toast 计数，确认后走批量管线） | Q50 |
| `Telegram/Resources/langs/lang.strings` | 新增 6 键（ayu_×2、lng_sexcat_×4，含 #one/#other 复数） | Q50/HA |

## 计划偏差

计划 Task 4 原定把 `AddAction` 整体重构为 `RunBatchSave`；因 `AddAction` 内 batchTag 注释行含安全遮蔽内容（无法可靠转写原文），改为抽取 `ChooseFolderAndSave` 公共调度器 + 选择器自建 save 闭包，`AddAction` 原批量机器保持不动。行为等价。

## 四缺口 → 修复映射

- **H1 缓存直存**：`saveFromDataChecked` 写盘目标即增补名（快速保存/播放器保存/批量快速保存全链）。
- **HD loading 吞标记**：守卫移除；且增补名在写盘点已生效，标记只服务记录与兜底 rename。
- **HC 标记 10 分钟过期**：TTL 剪除整体删除；长下载完成时 rename 兜底仍有上下文。
- **HA 去重虚指**：目标文件不存在时不再 `setLocation` 虚指；跳过时 toast `lng_sexcat_already_saved`。

## 静态验证（已通过）

- `git diff --check` 干净；UTF-8 无 BOM、LF 行尾保持。
- `PruneStalePending` / `kPendingSaveAsTtl` / `explicitSaveAs && !data->loading` 全仓 0 残留。
- `saveFromData(` 非 Silent 调用点仅 `data_file_click_handler.cpp:109`（带 contextId）；其余 Silent 调用默认参合法。
- 新符号声明/定义/调用三向一致；`FullMsgId` 经 `data_types.h → data_msg_id.h` 可见。
- 子菜单写法逐行对照仓内先例（`sexcat_save_hook.cpp` AddSowTagMenuActions）；`FormatSizeText` 在 `ui/text/format_values.h`。

## 外部构建机手测清单

1. 未点开视频，右键 Save as… → 对话框默认名即 `#tags -简介-516156156.mp4`，直接保存。
2. 点开视频看完 → 播放器 Save → 下载目录出现增补名（不再依赖 rename）。
3. 右键相册 → Download album files（askDownloadPath 关）→ 全部增补名。
4. 右键相册 → Select videos to download… → 勾选 2/5（toast 计数）→ Download selected videos → 仅勾选的落盘。
5. 已保存过的视频再次另存 → toast "Already in library…"，不再静默无反应。
6. `tdata/logs/ayu-<date>.log` 中 `augment … skipped: <原因>` 行可解释任何残余裸数字名。

## 已知限制

- 子菜单勾选态以 toast 计数反馈（`lib_ui` 不在盘，`setActionState` 无法证实；仓内无复选菜单先例）。后续若外部确认该 API 存在，可升级为视觉勾选。
- 另存对话框默认名为增补名后，用户手改的名字仍优先（不改用户输入）。
- `history_view_draw_to_reply.cpp` / `iv_cached_media.cpp` 的静默落盘无消息上下文，走 PendingMap 兜底；无上下文时保持裸数字名（非用户保存流）。
