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

## Amendment 2（Q53 轮拍板：语法推翻——"一坨"语法与永不放弃不变式）

### Context

- v7.2.9-ayu.35 实测 /hsck/290852（caption = `标题: 新版破解MMMB-195 五十路母の尻に興奮してしまい…4時間2` + `来源: …` + `加入我的频道 @onlyjavzh`）落盘仍为服务端名 `290852.mp4_4.mp4`。
- 用户推翻 Amendment 1 沿革下来的"番号优先语法"（§35 定稿 B）："正则个鸡巴，不是必须要规范性命名的"——三轮命名形状返工的根源是解析层过度聪明。
- 静态推演矛盾：对该原名，.35 语法本应产出非空候选并改名成功 ⇒ 本次未改名与语法无关，属链路未达或去重跳过后旧文件残留；去重跳过分支只 toast 不改名、无自愈。

### Decision

（回执三/回执四定稿；段序以回执四为准）

1. **语法 = 原始名 + 简介 + tags**（回执四：原始名居首、简介居中、tags 收尾；理由"现在的命名法不好看"）：终名 = `<原始名 stem> - <简介 blob> - <#tags>.<扩展名>`，**扩展名恒钉在文件名最末**——原始名前置时剥掉其扩展名、终名以扩展名收尾（Windows 按最后一个点取扩展名，原始名整段前置会把 .mp4 埋进名字中段，类型识别错乱）。blob = caption 清洗文本（非法字符清洗 + 样板剔除 + 空白折叠）+ hashtags；番号正则、【片名】【番号】结构化字段、CodeFromOriginal、code 优先 head 全部废除。段缺省连分隔符一并省略；简介与 tags 全缺 = 仅 stem+扩展名 = 原名 → 精确相等 no-op。
2. **预算与截断（用户规则：简介截断量由原始名与 tags 的长度决定）**：简介是唯一软段——按目录路径预算（260−目录−1，下限 40，组件上限 255）先留足 stem + 扩展名 + tags 段 + 分隔符，余量全部给简介；极端回退序 = 简介整段删 → 逐个砍尾 tag → 最后才裁 stem（保扩展名），永不整段放弃。
3. **硬不变式（防回归验收口径）**：永不因"看起来已命名"跳过（startsWith 启发式与 ≥2 `#` 标签列表特例全删），永不因超长整段放弃；幂等判定只认"候选名 == 原名"的精确 no-op。样板垃圾剔除**保留**（回执三③拍板：来源:/加入我的频道/@提及/标题: 标签/更多精彩TG/更多完整版/点击置顶/点击提取/URL）。
4. **不允许以改语法代替修链路**：重建后若仍遇零改名，以既有埋点日志（每个 bail 自报家门）定位写点缺口，逐入口补线。

### Consequences

- "已命名"误判类跳过（纯番号名 / 整段标题名 / 标签列表名 三形态）随启发式删除一并消灭。
- tag 上限（8）沿用；多文件 part 标记沿用，附着在 stem 段（`<stem> partN - 简介 - tags.ext`），原始名互异时自然无碰撞。
- 旧测试轮遗留的原始名文件由 R2 去重自愈修复（跳过分支就地补改名 + DB Relink + toast 点名文件），不再是"永不修复"。
- 实现落点：RefineDescription 退役（保留清洗与样板剔除）、BuildAugmentedName 换三段组装（stem→简介→tags，ext 钉尾）+ 精确幂等、NameParts.code 删除。
- 文件名以 tags 或扩展名收尾、以原始名开头：Explorer 排序/类型识别不受影响（扩展名已钉尾），按名字排序/搜索时同频道文件按原始名聚簇。
