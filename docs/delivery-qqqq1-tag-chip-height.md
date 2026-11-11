# QQQQ1 — 聊天列表文件夹 chip（"UNREAD"→"llNRFAD"）底部四分之一被裁

## 现象
v7.2.9-ayu.23，左侧聊天/频道列表，文件夹名 chip（如 `UNREAD`）大写字母底部约 4px 被下一行不透明背景盖掉，读成 "llNRFAD"。文件夹成员大多命中。

## 根因链（全部带行号）
1. 主列表 Row 创建/重算早于文件夹加载，`_tagColors` 尚空 → 行高按 62px 固化（dialogs_list.cpp:35/178/195）。
2. 文件夹加载：`addToChatList` 强制兜底取色写 `_tagColors`（dialogs_entry.cpp:461-469），**不发任何高度更新**（:441-452 无事件）。
3. 唯一自愈管线被 premium 门死：`tagsEnabled` 来自服务端 `is_tags_enabled`（data_chat_filters.cpp:478）→ dialogs_inner_widget.cpp 原 :583-585 `if (!tags) return;` → 存在变更自愈处理器永不接线。
4. 绘制侧谓词已被 fork 解门（"chips synthesized locally…premium flag must not gate"）→ chip 画在 y=52 于 62px 旧行内 → 底 ~4px 被下行背景盖（dialogs_layout.cpp:539）。
5. 第 3 轮修复（1a073e20cd）只统一了谓词函数，没修"存储高度缓存永不刷新"。

## 改动单
- **F1（根因，待落地）** dialogs_entry.cpp `setColorIndexForFilterId`：`_tagColors` size 变化时补发 `updateChatListEntryHeight()`。改动已备好（+2 行 WHY 注释 + size 守卫）；文件被另一活跃会话认领（session-05b57906），release 后一条 edit 即落地。必要性依据：data_chat_filters.cpp:816 批量重建直调 addToChatList，不经 refreshChatListEntry，无事件可自愈。
- **F2（已落地）** dialogs_inner_widget.cpp:582-604：去掉 `chatListEntryRefreshes` / `tagColorChanged` 处理器的 `tagsEnabled` 门，与"chip 本地合成不门控"决策对齐；存在变更自愈（updateHeights）现对非 premium 生效。
- **F3（已落地）** dialogs_layout.cpp:1141-1146：chip 绘制 y 钳位 `min(tagTop, 行高 - chip逻辑高)`，任何 stale 行高都不可能再裁（chip 上移 4px 完整显示）。

## 外部构建机验证清单
1. Debug 构建（外部构建机；本会话零构建）。
2. 全部聊天视图：任一文件夹（如 unread）成员的 chip 完整无裁切。
3. 切换文件夹视图往返：chip 完整。
4. 重启应用：chip 完整。
5. 在文件夹视图停留时用另一端把某聊天加入/移出文件夹，切回全部聊天：chip 完整（F1 落地后此路径也由事件兜底）。
6. 本地标签菜单（右键 → 本地标签）增删后行高正确（既有路径，回归确认）。
