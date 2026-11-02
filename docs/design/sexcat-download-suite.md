# SexCat 下载溯源套件 — 设计稿 v1

> 目标版本：`v7.2.9-ayu.10`（实现完成后写 release notes 再交付 tag）
> 覆盖需求：#20 保存成功浅色提示 / #21 专属数据目录 / #22 下载溯源 / #23 下载历史 / #25 气泡内已保存标记 / #26 改名移动仍可对应 / #27 仅限另存 / #28 UI 设计
> **For agentic workers:** REQUIRED SUB-SKILL: superpowers:subagent-driven-development（推荐）或 superpowers:executing-plans 逐任务实施。步骤用 `- [ ]` 跟踪。

**目标**：手动"另存为"的每个文件自动留下可溯源记录（来源消息、时间、路径、内容 SHA-256），聊天里一眼看到哪些下载过，改名/移动后仍能对应回源。

**架构**：专属 SQLite（`tdata/sexcat/sexcat.db`）+ NTFS 备用数据流（ADS）存哈希 + 另存完成点钩子记录 + 气泡徽标/历史窗口两个 UI 面。与 AyuGram 的 `ayudata.db` 完全隔离。

**技术栈**：sqlite_orm（仓库既有）、NTFS ADS（QFile 流式写 `name:stream`）、rpl 事件流、tdesktop style/lang 体系。

## 全局约束

- 存储目录 `tdata/sexcat/`，专属本套件，**绝不读写 `tdata/ayudata.db`**（用户明确要求）。
- 仅记录用户手动"另存为"成功的事件；自动下载/快速下载不记录（#27）。
- sqlite_orm 逆序 schema 教训：索引声明先于表；初始化失败 → 整套功能静默禁用 + `[sexcat]` 日志，**不拦启动**。
- 注释配给：一处 `// WHY:` 例外，其余单行内注释；无 BOM；Windows 源文件 CRLF。
- 尺寸一律 `.style` 定义（`st::sexcat*`），时长用 `constexpr auto k... = crl::time(N)`；文本全走 `lng_sexcat_*`。
- 本仓库禁止本地编译（AGENTS.md）：每任务以"CI 构建绿 + 运行日志按脚本验证"收口。

---

## 1. 数据模型（`tdata/sexcat/sexcat.db`）

```sql
CREATE TABLE saved_file (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    sha256 BLOB(32) NOT NULL,          -- 内容哈希，下载时算一次
    file_name TEXT NOT NULL,           -- 另存时的文件名
    saved_path TEXT NOT NULL,          -- 另存时的完整路径（仅历史参考）
    file_size INTEGER NOT NULL,
    src_peer_id INTEGER NOT NULL,      -- 来源对话 id（bits 封装同 tdesktop PeerId）
    src_msg_id INTEGER NOT NULL,       -- 来源消息 id
    src_title TEXT NOT NULL,           -- 来源对话标题快照（源会话被删仍可读）
    src_attach INT NOT NULL,           -- 文件在消息中的序号（相册第几张）
    saved_at INTEGER NOT NULL          -- unix time
);
CREATE INDEX idx_saved_file_sha ON saved_file (sha256);      -- 先于表注册（逆序教训）
CREATE INDEX idx_saved_file_src ON saved_file (src_peer_id, src_msg_id);
```

- 同一文件多次另存 = 多条记录（历史显示"最近有效路径"）；无唯一约束。
- 查询接口（`SexCat::Database`，单例，后台读写队列）：
  - `LookupByHash(sha256) -> std::vector<Entry>`
  - `LookupBySource(PeerId, MsgId) -> std::vector<Entry>`
  - `Recent(int limit, Filter) -> std::vector<Entry>`
  - `RecordSaved(Entry)` + `rpl::event_stream<Entry> newSaved`（徽标缓存失效信号）

## 2. SHA 复用策略（#26 核心，性能优先）

哈希只在**下载完成时算一次**（文件已在内存/已落临时文件），写三处：

1. `sexcat.db.saved_file.sha256`（权威记录）；
2. **NTFS ADS**：`<saved_path>:sexcat.sha`，内容 `64位hex + \\n + <srcPeerId>:<srcMsgId>` —— 文件改名/移动 **ADS 跟随文件走**，命中即零运算拿到溯源；
3. 非 NTFS（exFAT/FAT32 U 盘）写失败 → 侧车文件 `<file>.sexcat.json`（同名目录，`{"sha256":..., "src":...}`）。

**气泡徽标查询优先级**（气泡渲染按 (peer_id, msg_id) 直查 db 索引，本来就不需要哈希——hash 链路服务历史窗口的"文件→来源"反查）：

```
历史窗口反查：
1. 读 ADS `<path>:sexcat.sha`        → 命中：查 db by sha（索引点查，O(log n)）
2. 读侧车 `<file>.sexcat.json`       → 同上
3. db 按 (path 基名 + size + mtime) 提示命中 → 展示但标记"未验证"
4. 全文件 SHA-256（后台线程、串行限速队列，>512MB 文件提示）→ 算完回写 ADS
```

> 难度评估（回应 #26"有点难度吧"）：ADS 方案把"移动改名后对应"变成零运算命中，真正的工作量在 (a) 钩子点找全（多入口另存）、(b) 侧车文件跟随行为、(c) 旧文件一次性回填工具（可选，M4）。

## 3. 记录拦截点（仅另存，#27）

- 钩子位置：手动另存的统一完成点——聊天右键"另存为..."、媒体查看器、文档查看器最终都走 `DocumentSaveClickHandler` / `PhotoSaveClickHandler` 的保存完成路径 + `File::OfferSaveAs` 之后的落盘成功回调。任务 T1 时以 grep 锁定全部入口（预期 ≤4 处），共用一个 `SexCat::RecordSaveAttempt(...)` 入口去重。
- 记录内容在落盘成功后异步写入（db + ADS），**失败只记日志，绝不影响保存本身**。
- SHA 来源：下载/加载完成时已计算的哈希缓存（`DocumentData` 媒体下载完成点），无缓存才补算（后台线程）。

## 4. UI 设计（#28/#20/#25/#23）

### 4.1 气泡徽标（#25）

- 位置：气泡媒体/文件区右上角内侧，14px 圆形角标（`st::sexcatBadgeSize`），图标 `gui/icons/sexcat/saved`（下载箭头+对勾双层 icon，`iconColor` 用 `msgOutDateFg` 系）；浅色底、hover 亮起。
- 语义：该消息的文件曾被另存过（按来源键查询，与文件现存无关）。
- 交互：hover → tooltip "已保存到 <最近路径>"；右键 → 菜单：打开所在文件夹 / 查看下载历史 / 重新验证。
- 性能：消息级 LRU（容量 `kBadgeCacheSize = 512`），db 查询异步返回，命中前不占位（渐进出现）；`newSaved` 事件触发相关消息重查。

### 4.2 保存成功提示（#20）

- 另存成功 + 记录完成 → 底部浅色 toast："已保存 · 已记录来源"，3 秒（`kSexCatToastDuration = crl::time(3000)`），复用 `Ui::Toast` 基建（浅色样式）。

### 4.3 下载历史窗口（#22/#23）

- 独立窗口（不塞设置页）：列 = 时间 / 文件名 / 来源对话（可点击跳转消息）/ 当前路径（存在性检测：丢失置灰 + "已丢失"徽标）。
- 顶部工具条：按对话过滤（下拉）、关键词搜索（文件名/标题）、"验证选中"（重算 SHA 对比，失败标红）。
- 路径丢失时：提供"重新定位"（用户选新路径 → 校验哈希 → 回写 ADS → 更新最近有效路径）。
- 设置页入口：AyuGram 设置 → 新增 "SexCat" 一节：总开关（默认开，关=不记录+不显示徽标）+ "打开下载历史"按钮。

### 4.4 lang keys（全部新增到 `lang.strings`，zh-hans 同步）

`lng_sexcat_saved_toast` / `lng_sexcat_badge_tooltip` / `lng_sexcat_open_folder` / `lng_sexcat_view_history` / `lng_sexcat_reverify` / `lng_sexcat_history_title` / `lng_sexcat_col_time` / `lng_sexcat_col_name` / `lng_sexcat_col_source` / `lng_sexcat_col_path` / `lng_sexcat_lost` / `lng_sexcat_relocate` / `lng_sexcat_verify` / `lng_sexcat_settings_toggle` / `lng_sexcat_hashing`

## 5. 文件结构（实施锁定）

```
Telegram/SourceFiles/ayu/sexcat/
├── sexcat_database.h/.cpp     # sqlite_orm 封装、读写队列、newSaved 事件
├── sexcat_hash_store.h/.cpp   # ADS/侧车读写、后台哈希队列、回写
├── sexcat_save_hook.cpp       # 另存完成点挂钩（T1 锁定入口后建）
├── sexcat_badge.h/.cpp        # 气泡徽标数据面（LRU + 查询），绘制挂 historyview
└── sexcat_history_window.h/.cpp  # 历史窗口 UI
Telegram/Resources/gui/icons/sexcat/saved.svg
Telegram/Resources/ui/sexcat.style
```

## 6. 任务切分

### Task 1: sexcat 基建（db + 哈希存储，无 UI）

**Files:** Create `ayu/sexcat/sexcat_database.{h,cpp}`、`sexcat_hash_store.{h,cpp}`；CMakeLists 加源。

**Interfaces (Produces):** `SexCat::Database::instance()`、`RecordSaved/LookupByHash/LookupBySource/Recent`、`newSaved()`、`HashStore::WriteAds/ReadAds/TrySidecar`。

- [ ] sqlite_orm 建库（索引先于表 + try/catch 兜底），初始化失败→禁用+日志
- [ ] ADS/侧车读写（Windows ADS：`QFile` 路径拼 `:sexcat.sha` 流；失败降级侧车）
- [ ] Commit: `Add sexcat dedicated storage and hash store`

### Task 2: 另存拦截 + SHA 写入

**Files:** Create `sexcat_save_hook.cpp`；Modify `DocumentSaveClickHandler` / `PhotoSaveClickHandler` / `File::OfferSaveAs` 落盘成功点（T2 先 grep 锁定 ≤4 入口）。

**Interfaces (Consumes):** T1 全部。Produces: `SexCat::RecordSaveAttempt(path, DocumentData*)`。

- [ ] 各入口统一记录（仅手动另存；自动下载路径零改动）
- [ ] 下载完成点哈希缓存接线，无缓存后台补算
- [ ] 保存成功 toast（#20，浅色 3s）
- [ ] Commit: `Record manual save-as with provenance and toast`

### Task 3: 气泡徽标（#25）

**Files:** Create `sexcat_badge.{h,cpp}`、`sexcat.style`、`icons/sexcat/saved.svg`；Modify 气泡媒体绘制（`history_view_media_grouped` / `history_view_document` 角标绘制点）。

- [ ] LRU + 异步查询 + newSaved 失效；hover/右键菜单（打开文件夹/历史/验证）
- [ ] Commit: `Show saved badge on chat bubbles`

### Task 4: 历史窗口 + 设置 + 打磨（#22/#23/#28 收口）

**Files:** Create `sexcat_history_window.{h,cpp}`；Modify `ayu/settings` 新增 SexCat 节；lang.strings 全量 key；zh-hans 翻译。

- [ ] 列表/过滤/跳转消息/存在性检测/重新定位/验证工具
- [ ] 设置节：总开关 + 历史入口
- [ ] 自测脚本：另存→toast→徽标出现→改名移动→历史反查命中→验证通过
- [ ] Commit: `Add sexcat download history window and settings`
- [ ] 写 `docs/release-notes/v7.2.9-ayu.10.md`，交付 tag `.10`

## 8. 文件夹扫描补录（feature 30）

历史窗口「扫描文件夹」：递归枚举文件，三层哈希复用——已有 ADS/旁车 sha 的直接跳过；
否则 1MB 分块哈希，sha 命中库记录时取最新记录的源信息回写 ADS 并补录条目（旧下载重链），
未命中仅写 sha（源 0:0，供后续校验与去重）。超 512MB 与读取失败计入 failed，全程主线程回调。

## 7. 自测脚本（M4 交付前全过）

1. 另存图片/文件/相册第 2 张 → toast 出现 3 秒消失，db/ADS 各有一条记录
2. 徽标出现在对应气泡；右键菜单三项可用
3. 把文件改名 + 移目录 → 历史窗口"重新定位"→ 哈希校验通过 → ADS 回写
4. U 盘（exFAT）另存 → 侧车生成，改名后仍可反查
5. 总开关关闭 → 新另存不记录、徽标不再出现（旧记录保留）
6. 启动日志无 `[sexcat]` 错误；删 sexcat.db 重启 → 自动重建不崩溃
