# 交付：QQQ1/QQQ3/QQQ10/Q20（UI 颜色与下载历史四连修）

> 本轮全程先读 UI 代码、先根因后动手（systematic-debugging 四阶段）。零构建；验证 = 静态交叉核验 + 外部构建机实测。

## QQQ1 — 补充修正（用户："NRFAD 跟文件夹标签同一行"）

**归属更正**："NRFAD"不是 AG 检查点标记（那在名字行最右，文本 `AG n/m`）。它是**文件夹标签行的 chip 文本**——所有 chip 都被 `toUpper()` 渲染（filter 标题 `dialogs_inner_widget.cpp:6008`、本地标签 `:225`），小写原文在 chip 里显示为大写。

**真显示 bug（证据链）**：
- 绘制侧：`hasChatsFilterTags(context.filter)` 为 false 时，`localTags` 非空仍切 `taggedDialogRow`，chip 画在 y=52（inner_widget :1353-1356）。
- 高度侧：`Row::ComputeSt` → 同名函数在 `_tagColors.size()==1 && first==exclude`（聊天只属于当前文件夹）时**提前 return false**，本地标签兜底不可达 → 行高保持 62px。
- 后果：62px 行里 chip 从 y52 起画，底部 ~4px 被下一行背景盖住；大写字母失去下横 → **E 读作 F**、n/r 顶部残笔像 "ll" → "UNREAD"/测试标签 被读成 "NRFAD"，恰好"四分之一隐藏"。只在**文件夹视图**触发（全部聊天 exclude=0 不触发），解释了此前三次修复都没打中的现象。

**修复**（`dialogs_entry.cpp` hasChatsFilterTags，commit 1a073e20cd）：排除命中时不再短路，继续落到本地标签检查；高度与绘制在每个用例下一致（真值表逐格核验；`data_chat_filters.cpp:809` 的 exclude=0 用例不变，accessibility 调用方现在与绘制一致）。dialogs_layout 的徽章重叠修复保留——那是名字行另一处独立覆盖源。

**验证方式**：修好后 chip 完整可读，文本本体一目了然（大概率是某个 "Unread" 文件夹的 chip 或此前 QQ2 测试时设置过的本地标签；右键 → Set local tags 可查）。

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

## 编译错误审计（7c1a6e1..HEAD，覆盖 20b0f94388 批次）

四路并行静态审计（数据层 / sexcat / UI-dialogs / 跨文件符号一致性），全部结论经人工复核：

**已修复（commit 后附）**
1. **error**：`menu_item_download_files.cpp:375` `FormatSizeText` 无限定名（声明于 `namespace Ui`，本文件无 `using namespace`）→ 改 `Ui::FormatSizeText`。这是 20b0f94388 批次引入的唯一确定编译错误。
2. **warning（codegen 风险）**：`lng_sexcat_lost_group` 是全 lang.strings 唯一"非复数键 + {count} 占位符"组合（{count} 按名映射 count-tag 系统，无先例可证 codegen 行为）→ 改为 `#one/#other` 复数键，调用点（lt_count + int）与 `lng_seconds` 先例完全同型，无需改代码。
3. **warning（运行时行为）**：下载历史分组双击展开被 Qt 默认 `expandsOnDoubleClick` 二次翻转抵消（净效果为零）→ `setExpandsOnDoubleClick(false)` + 保留手动翻转。
4. **warning（一致性）**：`Relink(row->id, file)` uint64→int64 隐式转换 → 显式 `int64(row->id)`，与兄弟路径一致。
5. **info**：移除因 Q10 改动而孤立的 `base/unixtime.h` include。

**确认通过（无需改动）**：AyuForbidden 位唯一（61/30 空闲）+ include 链齐全 + Data::Flags removeFlags API 存在；saveFromData 家族与 SexCat 写点五函数声明/定义/全仓调用点逐一匹配；Relink 双列 set 有基提交 5 列先例；`Entry::saved_path` 是 QString（std::string→QString 转换在 EntryFromRow，直接传 QFileInfo 合法）；applyFilter 双 vector const 性正确；dialogs_layout 变量作用域完整（旧变量 nameW/markLimit 零残留）；样式键 7 个全部存在；新增 6 个 lang 键双向核对无孤儿；枚举位/删除符号/include 全部通过。

**遗留 info（不修）**：三处零参 `saveFromDataSilent()` 拿不到 rename 上下文（行为提示，编译正确）；lib_ui/lib_base 子模块未检出，Text::String::DrawArgs 等以同树既有调用点等价性验证。

## 外部构建机手测清单

1. 有检查点的会话、长名字 + 认证徽章：AG 标记完整可读（不再被盖左半）。
2. 特殊限制频道：左侧列表行名、顶部群名（+顶部栏色条）显示红色；重启/全量更新后不消失。
3. 下载的文件改名：历史窗口 File name 与 Current path 同步更新（新名字）。
4. 移动文件到之前存过的目录：自动 relink，Current path 更新；跨盘移动用右键 Relocate 指认（不再产生重复行）。
5. 失效记录集中在底部折叠分组，双击展开，颜色灰。

## 已知限制

- 跨盘（不同分区）移动仍无法被 shell 通知捕获（Windows SHChangeNotify 边界），需右键 Relocate 一次。
- 子列表/搜索行的检查点标记同样受益于本次几何修复，但窄行 (narrow) 布局不绘制标记行，属上游行为。
