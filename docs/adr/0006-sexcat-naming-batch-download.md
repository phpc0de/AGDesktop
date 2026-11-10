# ADR-0006: sexcat 命名增补与批量下载交互（Q49 / Q50 / Q52）

## Context

/grill 审计轮（CONTEXT.md §16）取证：

- Q49 改名（`MaybeAugmentedSavePath`，sexcat_save_hook.cpp:568-615）只在显式保存链触发（另存对话框 / 媒体查看器 / 批量+选过文件夹）；快速保存与批量快速保存走下载管理器（`HandleLoadedDownload` :617-653）只记 tag、不改名 → 用户拿到的仍是 `516156156.mp4`。
- `ExtractHashtag`（:132-136）正则纯 ASCII（`#([A-Za-z0-9_]{1,64})`）："#抖音" 提不出；纯中文 tag 消息右键无保存菜单项；"#抖音 #abc" 只提出 `abc`。
- 批量下载（menu_item_download_files.cpp）无独立"另存为…"入口（路径行为被 `askDownloadPath` 设置控制）；全仓无"取消全部在途"，下载区 "Delete all"（info_downloads_widget.cpp:116）连已完成文件一起删。
- 选中文字保存的标签无关联语义（Q52.2"作品怎么关联"）。

## Decision

（2025 年 /grill 收敛轮，用户拍板）

1. **Q49 命名格式** = `#tags -简介-原名`（示例 `#抖音 #abc -今天打老虎-516156156.mp4`）：tags 段 = caption 内全部 hashtag 以空格相连，后接 `" -"`，再接简介段（caption 去 hashtag 后余文清洗），再接 `"-"` + 原名；缺段连同分隔符省略。触发条件不变（原名主干全数字 + 消息有文字），长度上限 240（截断保原名）。
2. **Q49 覆盖面** = 所有下载完成路径统一改名：改名下沉到下载管理器完成回调（`HandleLoadedDownload`），显式保存链保留；照片命名（photo_N.jpg 非"无效命名"场景）不参与。
3. **Q52 关联语义** = 凡 `#任意字符`（Unicode hashtag）即参与自动关联。识别**复用客户端自身的 hashtag 识别**（消息实体 `EntityType::Hashtag` / `HashtagClickHandler`），废弃自制 ASCII 正则；仅在实体缺失时用 Unicode 兜底正则（同语义）。右键点在 hashtag 上 → 保存光标下那一个 tag；选中的 `#文字` 同样参与自动关联；纯非 `#` 文字仅作标签（retro-tag 手工挂）。
4. **Q50 缺口** = 批量菜单新增独立"另存为…"入口（恒弹文件夹选择、无视 `askDownloadPath`，一次选择应用到整批，tag/改名沿既有管线自动生效）；新增 `DownloadManager::cancelAllLoading()`（只取消在途、不动已完成文件与记录），入口放下载条 + 下载区头部，与 "Delete all" 语义分开。

## Consequences

- lib_ui 子模块不在盘上，"#抖音" 是否产 `Hashtag` 实体需外部构建机/实机确认；实体缺失时兜底正则接管（语义一致，实现路径不同）。
- 自动下载落盘文件（有 caption 且原名全数字）也会被改名——已接受。
- 改名后记录/去重链（pending 行、sha 回填、FindRecordedForDocument）统一以新 path 走，避免路径漂移。
- 批量快速保存"已缓存文件（saveFromData 直存）不产生 sexcat 记录"为既有设计，本轮不改。
- `cancelAllLoading` 为 DownloadManager 新公开 API；取消经 `DocumentData::cancel()` → `destroyLoader` → `ForgetDoctrineSaveAs`，pending 位清理链路已有。

## Amendment 1（第 4 轮拍板 / QQQQ49–QQQQ50：改名下沉到写盘点）

### Context

R1–R3 交付（v7.2.9-ayu.22）后实测仍"下载文件就是原名字"（用户第 4 轮原话）。只读取证找到四个幸存缺口，共同形状：**改名钩子挂在链路节点上，而写盘动作有多条各自为政的路径**：

- H1 缓存直存：`DocumentSaveClickHandler::Save` 在 `saveFromData()` 成功时直接 return（data_file_click_handler.cpp:109-120），快速保存全链无改名；`saveFromDataChecked` 按裸数字名写盘（data_document.cpp:1549-1575）。流媒体缓存（看过/早前看过）即触发。批量快速保存同洞（menu_item_download_files.cpp:241，`SaveAndTrack` 默认 `Mode::ToCacheOrFile`）。
- HD loading 态吞标记：`explicitSaveAs && !data->loading()`（data_file_click_handler.cpp:140）——文档已在加载（自动下载/预取/另一保存）时 `MarkDocumentSaveAs` 被跳过 → 完成回调无条目 → 永不改名。`askDownloadPath=开` 的快存/另存目标本身即裸数字名，全靠事后 rename。
- HC 标记 10 分钟过期：`kPendingSaveAsTtl = 10min`（sexcat_save_hook.cpp:61）→ 长下载完成时 `NotifyDocumentSaved` 找不到条目提前 return（sexcat_save_hook.cpp:947-955）。
- HA 去重虚指：`SexCatShouldSkipDuplicate` 命中时 `setLocation(toFile)` 把位置指到不存在的文件（data_document.cpp:1330-1336）→ 后续 fast save 被 `saveFromData()` 虚指短路成静默 no-op。

用户已拍板：① 复现 = 未点开直接另存仍裸数字名；② caption 与视频同条消息（素材源无缺）；③ 修复面 = **写盘时改名 + 裸数字名 GIF/图片一并纳入**；④ 子集多选 = **右键子菜单勾选**（播放器批量不需要）。

### Decision

1. **改名下沉到写盘点（单一钩子）**：一切用户可见终名在生成时即为增补名——`saveFromDataChecked` / `saveFromDataSilent` 落盘目标、fast save 目标、批量文件夹目标、**ToNewFile 对话框默认名**（对话框里直接显示增补名，用户可改；不再依赖完成回调 rename）。触发门槛不变：原名主干全数字 + 组级 caption/tag 非空。
2. **事后 rename 降级为纯兜底**：保留 `MaybeAugmentedSavePath`（DownloadManager 完成回调、遗留链），但不再是任何路径的唯一防线；`MarkDocumentSaveAs` 的标记**不再受 10 分钟 TTL 剪除**，HD 的 `!data->loading()` 守卫移除（loading 态也标记，由写盘点保证终名正确）。
3. **HA 修复**：去重跳过分支不再对不存在的 `toFile` 做 `setLocation`；跳过时 toast"已在库中/文件已存在"（消除"点了没反应"）。
4. **裸数字名 GIF/图片纳入**：`saveFromDataSilent` 静默落盘（media_view_overlay_widget.cpp:4812 等）与 photo 场景同样过写盘点钩子；非全数字名（photo_N.jpg 等）不触发。
5. **Q50 子集多选**：右键相册菜单新增"选择视频下载…"子菜单，逐条列出组内视频（序号 + 类型/大小）复选，确认后整批走既有 AddAction 管线（写盘点改名 + batchTag 沿用）。
6. **诊断（防第 5 个 Q）**：保存链五点 AyuLog 埋桩（Enabled / item / 全数字判定 / 增补候选为空 / rename 或写盘结果），任一守卫命中即落日志，外部实测可一键定位。

### Consequences

- 对话框默认名变为增补名：用户在另存对话框里仍可手动改名（用户输入优先）。
- 事后 rename 失败不再导致裸数字名（写盘点已保证），rename 仅覆盖绕行链。
- GIF/图片静默落盘改名属于行为变更：裸数字名媒体落盘名带 caption tag（原 Decision 2 只承诺视频；本修订扩面，用户已拍板）。
