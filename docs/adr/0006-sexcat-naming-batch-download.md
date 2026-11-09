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
