# CONTEXT.md — AyuGramDesktop AG 功能域（/grill 会话状态）

> 本文件是 /grill 技能的状态同步工件（非应用代码）。审计为只读，不修改任何应用文件、不构建。
> 更新策略：每轮用户回合后同步；结构歧义全部消除前不进入编码。
> 已定稿决策见 `docs/adr/0001-ag-checkpoint.md`、`docs/adr/0002-unlock-restricted-chats.md`、`docs/adr/0003-editable-message-replay.md`。

## 1. 项目指纹
- 仓库：AyuGramDesktop = Telegram Desktop (tdesktop) 的 AyuGram fork，基线 v7.0.9，分支 `dev`，HEAD `db3b9891cb`。
- API layer 228（`Telegram/SourceFiles/mtproto/scheme/api.tl:3087`）；AppVersion 7.0.9（`Telegram/build/version`）。
- Ayu 专有代码：`Telegram/SourceFiles/ayu/`（settings JSON、SQLite `tdata/ayudata.db`、forward 重传引擎、右键菜单、防撤回/编辑历史、translate/filter/message_shot…）。
- codegraph 索引：就绪（`.codegraph`，2,908 files / 98,735 nodes / 334,971 edges）。

## 2. 需求清单与已确认语义（全部定稿）
| # | 需求 | 定稿语义 |
|---|---|---|
| 1 | 保存 AG 检查点 | 触发=右键菜单「保存为 AG 检查点」+ 消息上快速右键两次（双击右键）；每会话一个，新覆盖旧 |
| 2 | 禁止转发聊天解锁转发 | 群/频道级 noforwards；执行必须走 AyuForward 下载-重传（服务端 CHAT_FORWARDS_RESTRICTED 会拒直接转发，apiwrap.cpp:583 证据） |
| 3 | 禁止复制解锁复制 | 客户端本地门，peer 级放开即解除 |
| 4 | 禁止下载解锁下载 | 同上；媒体经 AyuForward 重传后自然可保存 |
| 5 | 跳转+重进默认定位 | 随时跳转；重进该群默认定位到检查点（无则原生行为）；跳转复用 showPeerHistory/showHistory msgId 链路（自动高亮） |
| 6 | 突破“请更新最新版本”消息限制 | 机制 A 确认：lng_message_unsupported 占位；方案=Unsupported 分支展示 vmessage 原文（rich_message 摘要优先），保留 Legacy/提示标记 |
| 7 | 重放/编辑消息 | 两期：一期消息级 Edit & Resend（复用 Repeat message 入口+发送管线，不碰字节层）；二期任意 RPC 包重放（Debug 开关），均展示返回包 |
| 8 | 快速迭代 / 编译问题 | **定案（ADR-0004）**：构建在外部构建机验证；本会话零构建/零安装/零 Docker；默认反馈 = codegraph+grep 交叉核验；每次交付附验证清单 |

## 3. 术语表
- **AG 检查点**：按 peer(+topic) 持久化的 (peerId, msgId) 锚点；每会话一个。
- **三道门**：noforwards 派生的转发/复制/下载客户端限制。
- **AyuNoForwards**：fork 私有 MessageFlag/peer flag，与原生 `Flag::NoForwards` 并存；语义=“由 AyuForward 接管”。
- **AyuForward**：下载-再上传“伪装新消息”的转发引擎（`ayu/features/forward/ayu_forward.cpp`）。
- **机制 A**：`messageMediaUnsupported` → `UnsupportedMessageText()`（`lng_message_unsupported`），需求 6 的目标分支。
- **Repeat message**：fork 已有功能（复读/无引用重发），非抓包重放；作 Edit & Resend 的入口样板。
- **MTProto 重放**：在序列化边界缓存请求→改参→正常重发（禁字节级重发）→展示返回包。

## 4. 审计结论摘要（证据详见 T1–T7 审计报告与 ADR）
### 需求 1/5（检查点）→ ADR-0001
- 右键入口：`HistoryInner::showContextMenu` → `addItemActions`（`history_inner_widget.cpp:3033` 起，仅 regular :3036-3041），Ayu 钩子 :3128-3132；共享构建 `FillContextMenuItems`（`history_view_context_menu.cpp:1488`，Ayu 钩子 :1124-1134）。双击右键走 contextMenuEvent 同路，无冲突；左键双击已被选词+快捷动作占用（`:2755-2808`）。
- 跳转链：showPeerHistory（`window_session_controller.cpp:3136-3149`）→ MainWidget::showHistory（`mainwidget.cpp:1386-1535`）→ HistoryWidget::showHistory（`history_widget.cpp:2999-3196`）→ isReadyFor/getReadyFor（`history.cpp:2987-3049`）→ firstLoadMessages around msgId（`:4840-4874`）→ countInitialScrollTop 显式 msgId 自动高亮+滚动（`:8215-8254`、highlight_manager）。
- 打开默认决策链：`dialogs_widget.cpp:1188-1202` → showThread → showHistory 归一（`:3024-3031`、`:3273-3277`、`:3293-3296` 内存级恢复 `_history->showAtMsgId` `:3143`）。介入点：打开前替换 showAtMsgId。
- 存储：**选定 `tdata/ayudata.db` 新增 Checkpoint 表**（sqlite_orm 模式照搬 DeletedMessage；多账号 userId 列；缓存入内存于会话初始化后）。

### 需求 2/3/4（三道门）→ ADR-0002
- 消息级已解锁（`:3124-3126` forbidsForward=false；`history_item_helpers.cpp:931-933` noforwards→AyuNoForwards；`data_user.cpp:690`；`:3128-3136`）。
- **唯一活源头**：群/频道 `Flag::NoForwards`（服务端置位，`data_session.cpp:933,1097`）→ allowsForwarding()=false（`data_chat.cpp:75-77`/`data_channel.cpp:787-789`）→ 三道 UI 门（`history_item.cpp:3051-3057`、`history_view_context_menu.cpp:427,434`、`history_inner_widget.cpp:3979-3981,657-658`、`history_view_list_widget.cpp:5987-5995`）。
- **服务端拒绝**：`CHAT_FORWARDS_RESTRICTED`（`apiwrap.cpp:583`，sendMessageFail 内，toast `lng_error_noforwards_*`；forwardMessages 失败回调即走 sendMessageFail `:4026-4037`）。
- **build 先决**：`data_session.cpp:934,1098` 引用不存在的 `data.is_ayuNoforwards()`（api.tl 无此 flag，全树 grep 无定义）→ 分支意图是 peer 级 noforwards 也映射 AyuNoForwards，但 v7.0.9 合并丢补丁 → HEAD 大概率编不过，落地前必先修。
- AyuForward 引擎已存在（`ayu_forward.cpp:236-245,321-510`；分流 `apiwrap.cpp:3834-3844`、`share_box.cpp:1868-1892`）。

### 需求 6（版本限制）→ ADR-0002 §4
- 机制 A：`CheckMessageMedia::Unsupported`（`history_item_helpers.cpp:1109-1110`）→ 构造分支丢 vmessage（`history_item.cpp:491-507` + Legacy）；`api.tl:124` message:string 必填 → 文本客户端可达（`history_item.cpp:559-565`）；rich_message 摘要先例（:495-498）。方案：Unsupported 分支展示原文/摘要 + 提示标记。

### 需求 7（重放）→ ADR-0003
- Repeat message：`ayu/ui/context_menu/context_menu.cpp:784-899`（默认隐藏 `ayu_settings.h:666`；Shift 无引用重发；rich+premium）。
- MTProto 拦截点：`sender.h:303` → `mtp_instance.cpp:1011-1125`（_requestMap/_parserMap）→ `session.cpp:422-440`（清 msg_id）→ `session_private.cpp:455-531,770-984`（赋 msg_id/seq_no）→ `:2651-2707`（msg_key+AES-IGE）。返回包：`:1824-1888`（rpc_result→requestId）→ `mtp_instance.cpp:1145-1202`；`Response.reply` 原始字节可达（`mtproto_response.h:71-77`）。`resend()`（`:2255-2310`）与 `overrideId`（`sender.h:229-232`）基建已有。

## 5. 边界/约束
- 服务端不可绕：编辑他人消息（CHAT_ADMIN_REQUIRED，`api/api_editing.h:22-27`）、受保护群直接转发（CHAT_FORWARDS_RESTRICTED，`apiwrap.cpp:583`）。
- 安全红线：禁字节级重发（msg_key/msg_id 必须管线重算）；RPC 重放默认隐藏 + 二次确认。
- 交互：左键双击已被占用；“双击右键”零冲突。
- 工程约束：不改二进制序列化中间字段；不做注释/不用 Q_OS_LINUX；不构建（AGENTS.md）。grill：不写应用代码，只同步 CONTEXT.md/ADR。

## 6. 开放问题（已清零——7 条需求语义全部定稿）
- 遗留实现期小项（非阻塞，见各 ADR）：双击右键的确切判定（快速两次 vs 连击阈值）在实现计划中定；CHAT_FORWARDS_RESTRICTED 在“无限制群”转发失败路径是否复用 sendMessageFail（影响 toast 语义）；Checkpoint 是否带 topicId（论坛）实现期定；Unsupported 的 Legacy 位保留 vs 移除（ADR-0002 倾向保留）。

## 7. 执行路线图（定稿）
- **P0 build 修复**：修正 `data_session.cpp:934,1098`（`data.is_noforwards() ? Flag::AyuNoForwards : Flag()` 或等价），恢复可编译基线。
- **P1 检查点**（ADR-0001）：存储（ayudata.db Checkpoint 表 + 内存缓存）→ 保存入口（addItemActions + FillContextMenuItems + 双击右键）→ 菜单「跳到检查点」→ 进群默认定位（打开链替换 showAtMsgId）。
- **P2 三道门 + 需求 6**（ADR-0002）：peer 级 AyuNoForwards 对齐 → allowsForwarding 放开 → 菜单/复制/下载门同步开 → 转发执行统一 AyuForward 重传分流（补 peer 级判定）→ Unsupported 分支部分文本。
- **P3 重放一期**（ADR-0003）：消息级 Edit & Resend（Repeat message 入口扩展 + 发送管线装载），返回包回显。
- **P4 重放二期**（ADR-0003）：任意 RPC 拦截/参数编辑/返回包 dump（Debug 开关默认隐藏）。
- 每阶段完成写实现计划（writing-plans）再动代码；grill 阶段结束前不改应用代码。
## 8. 需求 8：快速迭代 / 编译问题（讨论提案，第一轮）
### 8.1 本 checkout 结构性不可构建（本轮实测，确定）
- 工具链：cmake / ninja / clang / ccache / sccache / docker **全部缺失**；仅 make 4.4.1、g++ 14.2.0、python3 13.5 在位。
- 子模块：`git submodule status | grep -c '^-'` = **37 个全部未初始化**。其中 `cmake`（`git ls-files -s cmake` → `160000`；`.gitmodules:41-42`）与 `Telegram/lib_base`（`.gitmodules:19-21`，index `160000`）是构建必需 → `cmake/version.cmake`、`cmake/td_scheme.cmake`、`lib_base` 头文件**不在盘上**（`cmake/` 实测空目录；T7 审计已记录 lib_base 头缺失）。`Telegram/ThirdParty/*` 22 个目录均为未初始化子模块。
- 外部依赖：`../Libraries`、`../ThirdParty`、`../win64/Libraries` 全部不存在（AGENTS.md 的依赖布局在此不成立）。
- 构建树：无 `out/`（与 AGENTS.md:54「out/ 已配置」的前提冲突）。
- 资源：5 核；`/` 与 `/root/workspace` **仅剩 2.8G/118G（98% 已用）**。
- 结论：即便装上 cmake，缺 37 个子模块 + Qt/Libraries 依赖 + 磁盘 2.8G，本机无法构建 tdesktop；**P0（phantom accessor 修复）也无法在此验证**。

### 8.2 仓库构建设施（确定）
- Docker Debug 入口：`Telegram/build/docker/centos_env/build_debug.sh:8-11` → `run.sh bash -lc "cd /usr/src/tdesktop && cmake --build out --config Debug --target Telegram"`（只 build、不重新 configure，依赖预配置 out/）。
- `run.sh`：`:9-16` **硬性要求** `$HOME/Telegram/DesktopPrivate` 存在否则退出；`:18-20` CPU=`nproc-2`、内存=`总内存-4G`；`:22` 挂载 `$HOME/Telegram/tdesktop:/usr/src/tdesktop` + `$HOME/Telegram/DesktopPrivate:/usr/src/DesktopPrivate`，镜像 `tdesktop:centos_env`，`--rm -it`。→ 本 checkout 路径不匹配（`/root/workspace/AyuGramDesktop`）且无 DesktopPrivate，该脚本在本机不可直接用。
- 生产脚本 `build.sh:6-7`：`Telegram/configure.sh` + `cmake --build ../out --config ${CONFIG:-Release}`（默认 Release，与 AGENTS.md:67 冲突；仅用于镜像/依赖准备）。
- 镜像：rockylinux:8 + gcc-toolset-15（`Dockerfile:3,7-8`）、ninja/lld/ccache（`:16-19,30`）、`CMAKE_GENERATOR=Ninja`（`:78`）、`CCACHE_MAXSIZE=20G`（`:76`）、`CCACHE_DIR=/var/cache/ccache`（`:73`）、**最终镜像 `CCACHE_DISABLE=true`（`:1069`）**——ccache 只服务依赖构建阶段的 BuildKit cache mount（`:169-185`）；应用增量完全依赖**持久化挂载的 out/ 树**（`VOLUME ["/usr/src/tdesktop"]` `:1072`）。
- CMake 侧速度手段极少：`Telegram/CMakeLists.txt:258` 仅 `target_precompile_headers(stdafx.h)`；仓库内无 ccache/launcher/dev_mode 接线（grep 无命中）。
- 约定/工作流（T8-harness 子审计）：`AGENTS.md:5`「Avoid building the project」；`:48-54` Debug target；`:67` 禁 Release；`:115-146` macOS clean 边界；`:148-161` 锁恢复授权边界；`.agents/shared/build-lock-recovery.md:25` 要求序列化构建但**无真正 build-lock**（`workspace.py` 只有事后恢复）；`.agents/skills/perform-task/references/pipeline.md:248-290` 真实顺序=实现→Debug 构建→四镜头评审/修复/重建→提交→测试；`type: verify` 例外（`:304-319`）；`.github/` 无 workflows（无仓库内 CI）。已知陷阱：资源 codegen 不跟踪 mtime（`.agents/shared/test-loop.md:503-511`）。

### 8.3 未取证（下一轮/人工）
- scheme / lang.strings / styles 改动触发多大范围 codegen 与重编（`cmake/td_scheme.cmake` 不在盘上，无法读）。
- 外部构建机的真实增量耗时基线（本机无数据）。

### 8.4 定案（2025-09-25，详见 ADR-0004）
1. 构建执行地 = **外部构建机**（用户侧）；本会话不构建、不安装、不启 Docker（沿用 AGENTS.md:5 与既定只读约定）。
2. 默认反馈手段 = **codegraph callers/callees + grep 声明/定义交叉核验**（用户选定）。
3. 「避免构建」vs P0 必须验证 = **保持不构建、由外部验证**：我出精确改动点 + 需锁定的子模块 commit + 验证清单，外部构建机回填报错后迭代。
4. 成本依据见 §8.5；B（本机自建，≥80G 磁盘 + 解除约定）已评估后否决；C 的静态手段被采纳为默认反馈。

> 注：本日 agent 预算已触顶（`/budget unblock` 后可恢复子智能体编排）；T8-cmake/T8-docker 由主会话以只读工具补完。

### 8.5 成本模型（2025-09-25：实测 + 估算，用户要求先算账再定）
**实测（本机，确定）**
- 仓库 346M（.git 315M）；自身代码 1,367 个 .cpp / 2,848 个 .cpp+.h / **936,739 行**（`Telegram/SourceFiles`）。
- 37 个子模块**全部未初始化**：含构建必需的 `Telegram/lib_base`、`Telegram/lib_ui`、`Telegram/lib_tl`、`Telegram/lib_storage`、`Telegram/codegen`、`cmake`（.gitmodules 路径清单见上）；无 LFS（.gitattributes 命中 0）。
- 磁盘：`/` 与 `/root/workspace` 现剩 **31G**（16:12 时仅 2.8G，波动极大，曾达 98% 满）；内存 9.3G（可用 4.0G）；5 核。
- 工具链：cmake / ninja / clang / ccache / sccache / docker 缺失；gcc 14.2、make 4.4、python3 13.5 在位。

**估算（依据：tdesktop 构建特征 + 本机资源 + 仓库脚本约束；非实测）**
| 成本项 | A 外部构建机 | B 本机补全自建 | C 纯静态核验 |
|---|---|---|---|
| 磁盘增量 | 你那台机器 +20~35G（MSVC out/） | **+60~80G**：子模块 2~4G + `../Libraries`(Qt6 等) 1.5~3G + Docker 镜像 8~15G + out/ 树 25~40G（静态 asan + `-gdwarf64`）→ **本机 31G 不够，须先清出 ~80G** | 0 |
| 网络 | 0 | 3~6G 下载（子模块 + Qt + 镜像层） | 0 |
| 首次构建 | 30~60 min（你的机器） | 3~6 h（5 核）／5~8 h（Docker 路径，`run.sh:18-20` 把 CPU 限到 nproc-2=3、内存 total-4G≈5.3G） | — |
| 单次迭代 | 人工往返 ≥数分钟（含报错回填） | 增量 1~4 min（重 TU 编译 20~90s + 链接 1~3 min；`-static-libasan` 链接重） | 秒级（codegraph + grep） |
| 工具链 | 无需 | 需装 cmake≥3.25（`CMakeLists.txt:9`）+ ninja，且需**允许 install/构建**——与既有"禁构建/禁安装"约定冲突 | 无 |
| 内存风险 | 低 | 链接需 3~6G 而可用仅 4.0G → 需降并行度（-j1/-j2）或去 sanitizer | 无 |
| 一致性风险 | 外部机子模块 commit 须与本 checkout 对齐，否则验证失真 | 无 | P0 类"编不过"无法验证 |
| 维护成本 | 低 | 高（镜像/子模块/磁盘/并发 out/ 无锁） | 无 |

**结论**：已选定的组合 = C（codegraph+grep 交叉核验）+ A（保持不构建、外部验证）与成本最优一致；B 只在一次性投入 ≥80G 磁盘 + 数小时 + 解除"禁构建"约定时才划算。

### 8.6 外部验证交付物模板（ADR-0004 落地格式，每个交付回合附）
1. 精确改动点：`file:line` + 关键摘录/补丁片段。
2. 需与外部机对齐的子模块 commit（至少 `cmake`、`Telegram/lib_base`、`Telegram/lib_ui`、`Telegram/lib_tl`、`Telegram/lib_storage`、`Telegram/codegen`）——本 checkout 子模块未初始化，commit 以主仓 `.gitmodules`/`.git` 记录为准，外部机需 `git submodule update --init` 到同一 commit。
3. 外部验证命令：Windows `cmake --build out --config Debug --target Telegram`（AGENTS.md:48-54）；WSL/Linux `Telegram/build/docker/centos_env/build_debug.sh`（:25；注意 `run.sh:22` 挂载路径需为 `$HOME/Telegram/tdesktop`）。
4. 风险点与影响面（重编范围、是否触发 codegen、是否需重配 out/）。
5. 静态自检结果：符号声明↔定义、locale key、style 引用、include 路径、TL/codegen 连带（重点：fork 的 `data_session.cpp:934,1098` phantom accessor 类问题）。

## 9. P0 改动单（2025-09-27 完成审计，明细见 `docs/p0-build-fix-plan.md`）
- 故障：`data_session.cpp:934,1098` 调用 api.tl(layer 228) 中不存在的 `data.is_ayuNoforwards()`（全仓无定义）→ HEAD 编不过。
- 修复：两处 `flagsSet` 表达式合并为 `| (data.is_noforwards() ? Flag::AyuNoForwards : Flag())`（对齐消息级 `history_item_helpers.cpp:932-933` 与用户级 `data_user.cpp:1104-1107` 先例；`flagsMask` 已含该位 `:923,1047`）。
- 连带效应（已取证）：`allowsForwarding()`（`data_chat.cpp:75-77`/`data_channel.cpp:787-789`）恒 true → 三门解锁；`ayu_forward.cpp:244` peer 级判定接管 → 转发走下载-重传，绕开服务端 `CHAT_FORWARDS_RESTRICTED`（`apiwrap.cpp:583`）；受限提示 UI（`info_media_provider.cpp:111-114`、`history_inner_widget.cpp:658-661`）不再显示（预期）。
- 状态：改动单已出，**未落应用代码**（/grill 冻结）；待外部构建机应用并回填日志。

## 10. 计划与复核产物索引（2025-09-27：六路并发 subagent + 主线程逐条抽查）
| 文档 | 内容 | 主线程抽查结果 |
|---|---|---|
| `docs/plan/p1-checkpoint-storage.md`（913 行） | Checkpoint 表 DDL / CRUD API / 线程模型 / 缓存 | ✅ `ayu_database.cpp:14` 文件级单例成立；`DialogId` 类型【无法确定】 |
| `docs/plan/p1-checkpoint-entry.md`（771 行） | 两处菜单插入点 + 开关 10 触点 + 双击右键 | ✅ `history_inner_widget.cpp:3132/:3133` 锚点吻合；双击右键判定不可靠 → deferred 方案 |
| `docs/plan/p1-checkpoint-navigation.md`（787 行） | 拦截点 / 链接隔离论证 / 降级 | ✅ `window_session_controller.cpp` 对 dialogs_widget 引用数=0 → 拦截不劫持 permalink；勘误：列表点击实为 `Way::ClearStack`（:1194） |
| `docs/plan/p2-unsupported-partial-text.md`（541 行） | 规则 F / 保留 Legacy / 三处同改 | ✅ `history_item.cpp:500/:2325/:2637` 与第二入口 `webPageNotModified`（`history_item_helpers.cpp:1082-1083`）实测成立 |
| `docs/review/p0-independent-review.md` | P0 独立复核，8 项额外问题 | ✅ 采信 5 项（行号/写路径/allowForwarding/第 7 消费点/技术债）；verdict：可单独提交 |
| `docs/review/submodule-blindspots.md`（366 行） | 45 条假设 + 29 项外部核对清单 | ✅ 采信 3 项勘误；盲区实测（37/37 子模块空、styles 目录不存在）成立 |

**关键更正（主线程实测，非 subagent 自述）**
1. `channel#d49f34c6` 在 **api.tl:104**（:103 是 `chatForbidden`）——原 P0 改单已修。
2. `run.sh:20` 内存规则实测为 `MemTotalGb > 12 ? -4 : 不扣` → §8.2/§8.5 的「内存 total-4G≈5.3G」对本机（9.3G）不成立（实为不扣）。
3. `MTP::Sender::send` **全仓不存在**（`sender.h` 无 `void send(`）→ ADR-0003 拦截点改为 `Instance::sendSerialized`（`mtp_instance.h:162`/`:206`）。
4. 群设置读写闭环成立：`edit_peer_info_box.cpp:2914-2925` 发真实 `MTPmessages_ToggleNoForwards`；并暴露既有 bug——P0 未修时管理端开关恒显示关闭且每次重发 `enabled=false`。
5. P0 范围扩容（对齐 ADR-0002 决策 1）：补 `data_chat.cpp:75-77` / `data_channel.cpp:787-789` → `return true;`（2 文件 4 行）。
6. 新增影响面：`data_peer.cpp:772` `canExportChatHistory()`（群历史导出将变可用，需外部回归）。
7. 技术债（单独立项，不进 P0）：`data_chat.h:27` `AyuNoForwards = (1 << 31)`（无底层类型 enum→int，移入符号位属 UB，与 channel 侧 `1ULL<<63` 不对称）。
8. P0 断点面收窄：脚本交叉校验 `data_session.cpp` 全部 144 个 MTP 访问器 vs api.tl，仅 `is_ayuNoforwards` 缺失 → MTP 维度无第二处同类断点（非 MTP 类仍【无法确定】）。
9. 双击右键：Qt 派发因 `lib_ui` 空目录无法证伪 → 采纳 deferred 方案（首次右键仅起 Timer，超时用合成 `QContextMenuEvent` 重放菜单），默认关闭。
10. P2 展示规则定稿：`rich_message` 摘要 → `vmessage` 原文 → 兜底；保留 Legacy；三处同改；提示用斜体后缀（不用 `setAyuHint`，会覆盖真实 `post_author`）。

**待用户拍板**：① deferred 双击右键手势是否默认开启；② 是否授权开始修改应用代码（P0 先落地 / P1 先做）。

## 11. 实现交付（2025-09-27，P0+P1+P2 已落代码）
- 交付报告：`docs/delivery-2025-09-27-p0-p1-p2.md`（19 文件 +308/-16）。
- 复审报告：`docs/review/r1-data-layer.md`（P0 回归）、`r2-storage.md`（存储层）、`r3-ui-gesture.md`（UI/手势）、`r4-p2-unsupported.md`（需求6，0 确定 BUG）。
- 复审揪出并已修：peer 菜单早退死代码、搜索守卫用错（`uniqueChatsInSearchResults` 依赖 supportMode）、`itemRemoved` 早退导致悬垂指针、`MsgId::bare` int64→int 窄化、读侧 topicId 硬编码（论坛失效）、getCheckpoint 异常静默、`allowsForwarding` 顺带解锁整库导出。
- 有意保留：进群默认钉在检查点（需求 5）；双击右键默认开（用户拍板，500ms 延迟）；`AyuNoForwards = (1 << 31)` 符号位技术债（改底层类型牵动不可见 `base::flags`）。
- 未修缺口：设置页无该开关 UI 复选框（可改 `tdata/ayu_settings.json`）、peer 菜单跳转不带 topic、孤儿检查点不清理、**本机零编译验证**。

## 12. 需求 7 状态更新（2025-09-27）
- **未实现**。ADR-0003 方案完备，但「从消息右键菜单写入主输入框」无公开路径：`HistoryWidget::insertTextAtCursor` 是 public（`history_widget.h:113`），而 `History::owner()` 继承自 `PeerData::owner()` 返回 `Data::Session&`（`data_peer.h:226`）、`MainWidget` 不暴露 widget、`Window::ActiveInstance` 所在头不在本树；备选的本地草稿路线需要 `Telegram/lib_ui` 里的 `TextWithTags`/`Ui::InputField` 签名（子模块未初始化，不可核对）。
- 决策：不在本 checkout 盲写。落地需外部构建机或先初始化子模块（详见 `docs/delivery-2025-09-27-p0-p1-p2.md` §4）。

## 13. 交接（2025-10-22）
- 換會話請先讀 **`HANDOFF.md`**（§1 現狀、§2 環境事實、§3 需求1~8真實狀態、§4 改動清單、§5 複審/已修BUG、§6 缺口、§7 需求7阻塞分析、§8 檔案地圖、§9 下一步、§10 工作紀律）。
- 已提交 3 commit（`6ac8e67915` 源碼+文檔、`98c7dd830f` CI errors.txt、`8c530f0d50` 交接同步），基線 `db3b9891cb`；**尚未 push / 尚未打 tag**；權威變更集 = `git log --oneline db3b9891cb..HEAD`。
- 2025-10-22 收盤：GitHub Actions 發布流水線（`.github/workflows/release-win-amd64.yml`）就位；新會話請先讀 `HANDOFF.md` §11/§12 與 `docs/build-package/github-actions.md`。

## 14. 交付后反馈轮 1（2025-10-28，v7.2.9-ayu.4 实测，/grill 分诊）
明细见根目录 `1.txt`（2025-10-28 段）。
- **0 右键变慢**：根因=双击右键手势 500ms 定时器（`history_inner_widget.cpp:153,414,2864-2886`）。**拍板：延迟降 250ms，手势保留**。
- **1 顶栏跳转按钮**（需求 1/5 UI 变更）：`history_view_top_bar` 群名右、搜索左；有检查点才亮；消息菜单入口保留。
- **2 解锁转发失败**：合并后钩子均在（apiwrap:3878-3889、share_box:1877-1894）。复现=右键→转发→选目标（分流覆盖内）→ 失败点收窄为判定 false / AyuForward 内部失败回退。行动=AyuLog 五点埋桩+可复制 toast。
- **3 ToS 封禁频道**：服务端封锁不可突破；上限=本地缓存回看+可复制错误。
- **4 重放一期无反应根因实锤**：菜单回调走 `list->insertTextAtCursor` → `_insertTextCallback`（空则静默，`history_view_list_widget.cpp:5957-5960`）；全仓唯一接线在 `history_view_chat_section.cpp:710` 且接 `fieldForMention()`（普通聊天为 null）；经典界面无人接线。修复=双路接主输入框+传 TextWithEntities+setFocus+四点日志。二期未写，CI 绿可排期。编辑他人消息不可行。
- **5 调试工具箱**：8 条提案见 1.txt（AyuLog/可复制 toast/诊断复制/错误透传/回包查看器/诊断面板/launch log/诊断包导出）。
- **6 文件夹标签**：premium 门控（settings_folders.cpp:1090-1106），待确认账号 premium。

## 15. 反馈轮 1 夜批（2025-10-28 深夜，全程自动 commit，未 push）
- `052a5132a0` 手势延迟 500→250ms（项 0）。
- `a507177480` 解锁转发五点 AyuLog 埋桩（项 2）。
- `c95c8bbcc3` E&R 双路接主输入框 + `ShowCopyableDetails` 复制回退 toast（项 4）。
- `cd16a39170` 顶栏检查点跳转按钮：`topBarAyuCheckpoint`（show_in_chat 图标）置于搜索左侧，`TopBarWidget::jumpToCheckpoint()`（topic→showTopic，否则 showPeerHistory，ClearStack；无检查点→日志+可复制提示）（项 1）。
- `b2b2f36290` AyuGram 会话子菜单新增「Copy Chat Diagnostics」：peer id/type/转发锁/限制原因/检查点 一键复制+toast，子菜单不再因无检查点而隐藏（项 5）。
- `b82f6226b7` 发现上游把 `UnavailableReason::Extract/Compute` 桩成空（限制原因完全不落地）；在 data_session 两处解析点记录服务端原始 restriction_reason（项 3 证据链）。
- `f423cda581` `processCallback` handleError 落 AyuLog：req id/code/type/desc（项 2/5，服务端拒绝全量留痕）。
- 待办：push dev + 打 tag（v7.2.9-ayu.6，远端已有 .5）；E&R 二期（经典+新 UI 之外的发送路径）未排期。
