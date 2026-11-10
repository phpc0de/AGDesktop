# 交付：QQQ1/QQQ3/QQQ10/Q20（UI 颜色与下载历史四连修）

> 本轮全程先读 UI 代码、先根因后动手（systematic-debugging 四阶段）。零构建；验证 = 静态交叉核验 + 外部构建机实测。

## QQQ1 — 聊天列表里那个"四分之一隐藏的字母标记"是什么

**它是什么**：AyuGram 检查点标记。聊天列表会话名右侧的 `AG <当前最大消息序号>/<检查点消息ID>` 小字（dialogs/ui/dialogs_layout.cpp `ResolveCheckpointMark`），只在保存过本地检查点（AG 检查点功能）的会话上显示，用于一眼看出离线历史边界。

**显示病根（真根因，非猜测）**：`paintPeerBadge(rowName.maxWidth())` 用**未截断**的名字全宽定位认证/高级徽章；当名字被省略号截断时，徽章漂进右侧区域，正好压住静音图标和检查点标记的左半 → 标记只剩后四分之三，读起来像乱码（用户看到的 "NRFAD"）。

**修复**（dialogs_layout.cpp PaintRow）：
- 先预留标记宽度，再以 `nameWidth = min(maxWidth, 可用宽)` 统一定位 badge、静音图标、标记；
- 名字绘制 `availableWidth` 用同一 nameWidth（顺带修掉名字与徽章间的缝隙）；
- `markAvailable = (rectForName.width() - nameWidth) + mark.width`，标记终点恰好在日期边界，不再过省略/不再压日期。

## QQQ3 — 特殊限制频道红色 tint（顶部群名 + 左侧列表）

**接线本来就对**（top_bar_widget.cpp:748、dialogs_layout.cpp:1044 前几轮已通），**真根因**：`Flag::Forbidden` 是瞬态旗标——全量 `MTPDchannel` / `MTPDchat` apply 的 flagsMask 包含它而 flagsSet 不含 → 首次全量频道数据一到就被抹掉 → tint 永远看不到。这就是为什么前三次修接线都"看起来对但没用"。

**修复**：持久化客户端旗标
- `ChannelDataFlag::AyuForbidden`（1<<61）/ `ChatDataFlag::AyuForbidden`（1<<30）；
- `channelForbidden` / `chatForbidden` apply 时置位（data_session.cpp）；
- 全量更新只在频道 `!is_restricted()` / 群组 `count > 0`（证明可访问）时清除；
- `Ayu::PeerTintOf` 首查 AyuForbidden → `PeerTint::Banned`（红），两个表面立即生效；顶部栏底部 2px 色条同样点亮。

## QQQ10 — 移动/改名后 Current path / File name 不更新

三个根因，全部修复：
1. **`Database::Relink` 只写 saved_path**，file_name 永不更新 → 改名被捕获后名字列仍旧名。修复：Relink 同步写 `QFileInfo(newPath).fileName()`。
2. **手动 Relocate（有效行）走 `RecordSaved(id=0)` 插入** → 生成重复新行、旧灰行永留。修复：SHA 校验通过后改走 `Relink(row->id, file)`（更新路径+名字、触发 newSaved 实时刷新），不再插新行。
3. **自动搬迁覆盖面**：watch 根新增 Documents；`RelocateCandidates` 追加"DB 近期记录的目录"作为同盘候选（用户移动文件的去处高度重复）。跨盘移动仍属 shell 通知能力边界，可用右键 Relocate 指认。

## Q20 — 失效灰行折叠 + 沉底

下载历史 applyFilter 拆 valid/lost 两组：有效行保持时间序在前；灰行全部收进列表最底部的折叠分组 `Lost records (N) — double-click to expand`（新增 lang 键 `lng_sexcat_lost_group`），双击分组行展开/收起；`refreshRow` 兼容子节点定位；右键菜单对分组行自动跳过。

## 静态验证（已通过）

- `git diff --check` 干净；UTF-8 无 BOM、LF 保持。
- `AyuForbidden` 八个触点（2 枚举 + 4 data_session + 2 peer_tint）一致。
- dialogs_layout 无 `nameW` 残留；badge/静音/标记共用 nameWidth 游标。
- 历史窗口 `RecordSaved`/`base::unixtime` 旧用法清除；`Recent(limit, {})` 默认参存在。
- 注释预算：2 行尾注 + 1 个两行 WHY 块，合规。

## 外部构建机手测清单

1. 有检查点的会话、长名字 + 认证徽章：AG 标记完整可读（不再被盖左半）。
2. 特殊限制频道：左侧列表行名、顶部群名（+顶部栏色条）显示红色；重启/全量更新后不消失。
3. 下载的文件改名：历史窗口 File name 与 Current path 同步更新（新名字）。
4. 移动文件到之前存过的目录：自动 relink，Current path 更新；跨盘移动用右键 Relocate 指认（不再产生重复行）。
5. 失效记录集中在底部折叠分组，双击展开，颜色灰。

## 已知限制

- 跨盘（不同分区）移动仍无法被 shell 通知捕获（Windows SHChangeNotify 边界），需右键 Relocate 一次。
- 子列表/搜索行的检查点标记同样受益于本次几何修复，但窄行 (narrow) 布局不绘制标记行，属上游行为。
