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

## 16. QQQ49 / QQQ50 / QQQ52 审计轮（/grill，代码只读取证；Q52/Q80 重构已提交、工作树干净）
### 16.1 Q49 命名增补 — 现状与"没修好"的形状
- 改名本体：`MaybeAugmentedSavePath`（sexcat_save_hook.cpp:568-615），条件 = 原名主干全数字（`IsAllDigitsName` :519）+ 消息有文字；名字 = `CleanSaveNameText(整段caption) + '-' + 原名`（:546-565）。Windows 非法字符已清洗（:138-161），长度上限 240（:516）。
- **触发面缺口（Q49 一直没修好的根因）**：改名只在 `RecordSaveAttempt` 链触发（:750→:768）。该链只覆盖：显式另存对话框（data_file_click_handler.cpp:111/:137 → NotifyDocumentSaved → data_document.cpp:1118/:1310）、媒体查看器保存（media_view_overlay_widget.cpp:3354）、批量+已选文件夹（menu_item_download_files.cpp:233-234）。
- **快速保存（askDownloadPath=关，含批量快速保存）不改名**：走 `SaveAndTrack`（menu_item_download_files.cpp:236）→ 下载管理器 → `HandleLoadedDownload`（:617-653）只记 tag、**无改名调用**。单条快速保存同样不改名。
- 命名格式差异：现状 = 整段拼接（`#抖音 #abc 今天打老虎-516156156.mp4`）；用户示例 = `#抖音 #abc -今天打老虎-516156156.mp4`（tags 段 + `" -"` + 简介 + `-` + 原名）。格式待拍板。
### 16.2 Q50 批量下载 — 功能在，两缺口确认
- 批量多选/全选/相册已存在：`AddDownloadFilesAction`（menu_item_download_files.cpp:281/:316；classic history_inner_widget.cpp:3617/:3939，新 UI history_view_context_menu.cpp:1467）+ `AddDownloadAlbumAction`（:348；新 UI :1263）。
- 缺口①另存为：批量仅一条保存项，路径行为被 `askDownloadPath` 设置控制（:251-275：开=弹文件夹框一次，关=静默丢默认目录）；无独立"另存为…"入口（单文件 Save as 存在）。
- 缺口②一键取消：全仓无"取消全部在途"。下载条（ui/controls/download_bar.cpp）纯进度 pill 无取消；下载区仅逐条取消 + "Delete all"（info_downloads_widget.cpp:116 → `deleteAll`，data_download_manager.cpp:611-639 **连已完成文件一起删**）。需新增 `cancelAllLoading`（只取消在途、不动已完成）。
- 附带发现：批量快速保存时已缓存文件（`saveFromData` 直存）完全不产生 sexcat 记录（data_file_click_handler.cpp:109-115 仅 explicitSaveAs 才 Record）。
### 16.3 Q52.1/52.2/52.3/52.4
- Q52.1：入口在**消息菜单**（history_inner_widget.cpp:3225 / history_view_context_menu.cpp:1900），非"点在 hashtag 上"判定；tag = 整条正文第一个 hashtag（`SourceTagForItem` :900-921，组内兄弟也扫）。**`ExtractHashtag` 纯 ASCII**（`#([A-Za-z0-9_]{1,64})` :132-136）→ 纯 "#抖音" 提不出：纯中文 tag 消息连菜单项都不出现；"#抖音 #abc" 只提出 "abc"。多 tag 永远取第一个，无光标定位。
- Q52.2：选中文字保存已存在（:983-988 → `SaveSowTag` :176，只注册标签）。**纯文字标签不自动关联任何作品**；自动关联仅限"下载消息 caption 含同名 ASCII hashtag"；手工关联 = 消息菜单 retro-tag 子菜单（`ApplyRetroTag` :924-941 → `RetagSource`，按 peer+msg 挂已下载记录）。
- Q52.3：保存自动注册 tag 已做（`RecordTag` :316/:398；批量 `batchTag` menu_item_download_files.cpp:89-107），但同受 ASCII 正则限制，#抖音 提不出 → Unicode 修复后 Q52.3 才成立。
- Q52.4 答案：**部分**。批量选文件夹路径 = 改名✓+tag✓；批量快速保存 = tag✓+改名✗；已缓存文件快速保存 = 全无记录。Q49 对 Q50 无完整支持。
### 16.4 本轮三问（已拍板，2025 收敛轮）
1. Q49 命名格式：**A 按示例 `#tags -简介-原名`** ✓
2. Q49 覆盖面：**A 所有下载完成路径统一改名** ✓
3. Q52.2 关联语义：**`#任意字符` 即关联；复用客户端自身 hashtag 识别（实体/ClickHandler），废弃自制 ASCII 正则**（用户原话："肯定是 #任意字符 就关联，客户端是自动识别 #任意字符，复用就好"）✓
- Q50 拟办未收异议，视为采纳：批量"另存为…"恒弹文件夹选择；`cancelAllLoading`（只取消在途）入口在下载条 + 下载区头部。

## 17. 执行路线图（Q49/Q50/Q52，定稿；细节见 ADR-0006）
- **R1 hashtag 地基**：✅ 已实现——`SplitHashtags`/`CleanTagToken`（实体 `EntityType::Hashtag` 扫描 + Unicode 兜底正则 `#[^\s#]{1,64}` + 尾部标点修剪）；`AugmentNameParts` 扫 item+组内兄弟（tag 全保留按序、desc 取组内首个非 hashtag 文字）；`TagFromLink` 经 `HashtagClickHandler`（dragText 带 #）取光标下 tag；classic（history_inner_widget.cpp:3225）与新 UI（history_view_context_menu.cpp:1900）均传 link；多 tag 时直项=光标下/首个，其余进子菜单。
- **R2 Q49 命名**：✅ 已实现——`BuildAugmentedName` 产出 `#tags -简介-原名`（缺段省分隔，240 截断保原名，保留名前缀 `_`）；`AdjustDocumentSaveTarget` 注入两路：`DocumentSaveClickHandler::Save`（!explicitSaveAs 快速保存，data_file_click_handler.cpp:134-138）+ 批量选文件夹分支（menu_item_download_files.cpp:234-238），落盘即终名、零漂移；`HandleLoadedDownload` 兜底改名（绕过链路时，管理器条目路径保留原名）；`MaybeAugmentedSavePath` 换新格式并放宽 `emptyText` 守卫为组级（相册 caption 挂首条消息时视频也能改名）。
- **R3 Q50 交互**：✅ 已实现——`AddAction` 改 const 引用 + `forceFolderDialog` 参数；多选/相册菜单各新增"Save selected as..."/"Save all album files as..."（恒弹文件夹选择，整批同 tag/改名管线）；`DownloadManager::cancelAllLoading()`（public，包 `loadingStop(nullptr)`，只取消在途）；入口 = 下载区顶栏菜单（loadingInProgress 才出现，menuIconCancel）+ 下载条右键菜单（download_bar 新增 `menuRequests()` producer，ContextMenuFilter 事件过滤器）；lang 新增 ayu_DownloadAlbumAs/ayu_DownloadFilesAs/ayu_CancelAllDownloading。
- **R4 验证**：静态交叉验证 ✅（声明/定义/调用点/图标 `st::menuIconCancel`/popupMenuWithIcons 经 style_chat、LF 行尾、`git diff --check` 干净、修正 `base/click_handler.h`→`ui/basic_click_handlers.h`）；外部构建机 Debug 编译 + 手测清单见 `docs/delivery-q49-q50-q52-sexcat.md`，未构建未打 tag。
- grill 冻结保持至用户令下开工；本体代码 0 改动。

## 18. 第 4 轮审计（QQQQ49 / QQQQ50）：缓存直存是幸存根因（/grill，只读取证）
> Q 计数 3→4 = R1–R3 交付（v7.2.9-ayu.22 起有效）实测后仍未修好。HEAD `7c1a6e1ff7`，`fbf2354d38..HEAD` 对相关文件仅 download_bar.cpp +4 行 include，无回归回滚。

### 18.1 Q49 幸存缺口（全部实测于当前 HEAD）
- **H1 主根因：缓存直存绕过全部改名钩子**。`DocumentSaveClickHandler::Save`（data_file_click_handler.cpp:109-120）在 `data->saveFromData()` 成功时**直接 return**：快速保存（!explicitSaveAs）既不走 RecordSaveAttempt（:110 仅 `!wasLocal && explicitSaveAs`）也不走 AdjustDocumentSaveTarget（:134 在此分支之后）。而 `saveFromDataChecked`（data_document.cpp:1549-1575）按 `DocumentFileNameForSave` 把缓存字节**直接写进下载目录的裸数字名**并置 `_location`。
  - 触发面 = 一切"字节已在流媒体缓存"后的保存：**播放器保存按钮**（media_view_overlay_widget.cpp:3547 `SaveAndTrack` Mode::ToFile）、聊天右键快速保存、**批量快速保存**（menu_item_download_files.cpp:241 SaveAndTrack 同入此洞）→ "看完再存 = 必现原名"，这正是用户最常见流程。
  - 变体 H1b：播放器静态分支 `saveFromDataSilent`（media_view_overlay_widget.cpp:4812）**不点保存**就把图片/GIF 按裸数字名写进下载目录。
- H2 次级风险：DownloadManager 兜底 `MaybeAugmentedSavePath`（sexcat_save_hook.cpp:738-751→705-736）在完成回调里 `QFile::rename`，Windows 句柄占用即失败（仅 LogFailure）；且管理器条目 path 保留旧名。
- H3 去重静默跳过：`DocumentData::save` 对已记录 (document,toFile) `setLocation`+return（data_document.cpp:1330-1336）→ 重存同一相册"点了没反应"。
- 格式/门槛本身符合 ADR-0006：`BuildAugmentedName`（sexcat_save_hook.cpp:658-688）= `#tags -简介-原名`、240 截断保原名、保留名检查（:634-654）；R2 两注入点（data_file_click_handler.cpp:134-138、menu_item_download_files.cpp:232-238）在各自分支内确实生效。
- 结构性结论：**改名钩子挂在"链路节点"上，而缓存直存是从旁绕行的第三条落盘路径**；只要钩子不在写盘点上，任何新绕行路径都会复发（第 4 个 Q 的由来）。

### 18.2 Q50 现状
- 入口两 UI 均已挂接：classic 相册 history_inner_widget.cpp:3594/:3774、多选 :3618/:3940；新 UI history_view_context_menu.cpp:1252-1268（相册仅在 `!overSelection && selectedItems.empty()` 时出现）。相册两条（Download album files / Save all album files as…）、多选两条（…as / Save selected as…）。
- 批量"另存为"分支改名✓（menu_item_download_files.cpp:232-238）；**批量快速保存（askDownloadPath=关）→ SaveAndTrack → H1** → 相册快速批量同样裸数字名（Q50"需自动重命名"因此被 Q49 连坐）。
- 未做：全屏播放器内批量入口；相册内子集多选（全选已有）。

### 18.3 本轮三问（已回执，见 18.4）
1. Q49 复现序列（验证/否证 H1 为主根因；并确认实测 build ≥ v7.2.9-ayu.22）。
2. Q50 缺口定位：入口不可见 / 落名未改 / 需播放器批量 / 需相圈子集多选。
3. 修复面拍板：改名下沉到写盘点（`saveFromDataChecked` 计算增补名直接落盘，显式链+管理器兜底保留）± 把裸数字名 GIF/图片静默落盘一并纳入。

### 18.4 第一轮问答回执（拍板 + 新增假设）
- **复现序列 = 未点开直接另存仍得裸数字名** → H1 非唯一根因；显式/批量"另存为"链在实测中失守。
- Q50 缺口 = ①批量落名仍裸数字（连坐 Q49）②**相册内子集多选**（新增需求；全选已有、播放器批量**不需要**、入口可见**无问题**）。
- **修复面拍板 = 写盘时改名 + 裸数字名 GIF/图片一并纳入**（用户选定）。
- 新增假设（第二轮问答分流）：
  - **HA 去重静默跳过 + 旧文件残留**：Q80 `SexCatShouldSkipDuplicate` 对已记录 (peer,msg,attach) 重存静默跳过并 `setLocation(toFile)` 虚指（data_document.cpp:1330-1336）→ 后续 fast save 因 `saveFromData()` 虚指短路成 no-op；文件夹里留的是旧轮次的数字名文件，"重下≠新文件"。
  - **HB 增补素材源为空**：tag/简介不在被保存文档的消息正文或相册组内（独立文本消息/实体缺失）→ `BuildAugmentedName` 返回空 → 不改名（批量选文件夹分支与 fast 注入点都会因此直落原名）。
  - **HC 另存改名标记 10 分钟过期**：`kPendingSaveAsTtl = 10min`（sexcat_save_hook.cpp:61，PruneStalePending :72-83）；单个 Save as… 的下载（大视频/慢网/批量串行长队列）超过 10 分钟后 `NotifyDocumentSaved` 找不到 Pending 条目提前 return（sexcat_save_hook.cpp:947-955）→ 永不改名。批量选文件夹分支不受此影响（落盘即增补终名），单个 Save as 链实锤受害。
- 定稿后动作：ADR-0006 修订（Decision 2 细化为"写盘点单一钩子 + 标记无 TTL 依赖"）→ 实现计划 → 才动代码。

### 18.5 第二轮问答回执 + 定案（全歧义清零，写入 ADR-0006 Amendment 1）
- 用户回执（原话级）：①"我下载文件就是原名字"（新下载即原名，排除旧文件残留视角）；② caption"和视频在一起啊，同一条消息"（HB 素材源无缺排除）；③ 子集多选 = **右键子菜单勾选**。
- 新实锤 **HD**：`SaveAndTrack` 默认 `Mode::ToCacheOrFile`（data_file_click_handler.h）；`askDownloadPath=开` 时所有快存/另存目标本身即裸数字名，全押在事后 rename 上；而 `explicitSaveAs && !data->loading()`（data_file_click_handler.cpp:140）在文档已加载（自动下载/预取）时**吞掉改名标记** → 确定性复现。四缺口 H1/HA/HC/HD 共同形状 = 改名钩子不在写盘点上。
- **定案（ADR-0006 Amendment 1）**：写盘点单一钩子（含 ToNewFile 对话框默认名）；事后 rename 降为兜底；标记免 TTL、去 `!loading()` 守卫；去重跳过去虚指 + toast；裸数字名 GIF/图片纳入；Q50 右键子菜单勾选；保存链五点 AyuLog 防第 5 个 Q。
- **执行路线图（R1–R6）**：R1 写盘点钩子（saveFromDataChecked/Silent、fast 目标、批量目标、对话框默认名）→ R2 标记寿命 + HD 守卫移除 → R3 HA 去重虚指修复 → R4 Q50 子集子菜单 → R5 五点诊断日志 → R6 GIF/图片扩面验证。
- 交付形态：先出实现计划（writing-plans，docs/plan/），经外部构建机编译实测；本会话代码零改动。

## 19. 第 5 轮实现交付（用户令"开工"后）
- 计划：`docs/plan/q49-write-point-naming.md`；交付：`docs/delivery-q49-q50-write-point.md`；实现按 ADR-0006 Amendment 1 全量落地（H1/HA/HC/HD 四缺口 + 子集多选 + 6 lang 键 + 五点诊断）。
- 计划偏差：AddAction 因遮蔽注释行不可转写，重构改为抽 `ChooseFolderAndSave` + 选择器自建 save 闭包（行为等价）。
- 静态验证全绿（git diff --check / 0 残留 / 符号三向一致 / include 链确认）；等待外部构建机编译 + 手测清单（见交付文档）。
- 本轮代码改动文件：sexcat_save_hook.h/.cpp、data_document.h/.cpp、data_file_click_handler.cpp、media_view_overlay_widget.cpp、data_document_resolver.cpp、menu_item_download_files.cpp、lang.strings。

## 20. 第 6 轮：UI 四连修（QQQ1/QQQ3/QQQ10/Q20）
- 用户令：改 UI 前必须先读 UI 代码；本轮按 systematic-debugging 四阶段走，全部先根因后动手。
- **QQQ1**：那个被截断的字母标记 = 聊天列表 AG 检查点标记（`AG <当前消息序号>/<检查点ID>`，dialogs_layout ResolveCheckpointMark，仅在有检查点的会话显示）。显示病根：badge 用未截断的 `rowName.maxWidth()` 定位，名字省略时 badge 漂进静音图标/标记区盖住标记左半；修复 = 统一定位游标（nameWidth 截断 + markAvailable 公式）。
- **QQQ3**：两处 tint 接线早已正确；真根因 = `Flag::Forbidden` 是瞬态——全量 MTPDchannel/chat apply 的 flagsMask 把它抹掉。修复 = 持久化客户端旗标 `AyuForbidden`（ChannelDataFlag 1<<61 / ChatDataFlag 1<<30）：channelForbidden/chatForbidden 置位，全量更新仅在频道未 restricted / 群组有真实成员时清除；PeerTintOf 首查它 → Banned 红。
- **QQQ10**：Relink 只更新 saved_path 不更新 file_name（改名后名字列永旧）；手动 relocate 对有效行走 RecordSaved 插入 → 产生重复行且旧行 path 永灰。修复 = Relink 同步写 file_name；手动 relocation 改走 Relink；自动搬迁候选目录加 DB 近期记录的目录（用户常用目的地）；watch 根加 Documents。
- **Q20**：下载历史失效灰行折叠沉底：applyFilter 分 valid/lost 两组，lost 归入一个折叠的顶级分组（双击展开），refreshRow 兼容子节点。
- 改动文件：dialogs_layout.cpp、data_channel.h、data_chat.h、data_session.cpp、peer_tint.cpp、sexcat_database.cpp、sexcat_shell_watch.cpp、sexcat_history_window.cpp、lang.strings；交付文档 docs/delivery-qqq1-q3-q10-q20.md。
- **Q1 修正（用户补充"NRFAD 跟文件夹标签同一行"后重查）**：NRFAD 不是 AG 检查点标记，而是**标签行 chip 的大写文本**（filter 标题与本地标签都 `toUpper()` 渲染）。真显示 bug = `Entry::hasChatsFilterTags(exclude)` 在"仅属于当前文件夹"时提前 return false，高度侧（Row::ComputeSt）保持 62px，而绘制侧 localTags 分支仍切 taggedDialogRow 把 chip 画在 y=52 → 底部 ~4px 被下一行背景盖掉，大写字母失下横（E→F，"UNREAD"→"NRFAD"），仅在文件夹视图触发。修复 = 排除命中时继续落到本地标签检查（1a073e20cd）；dialogs_layout 徽章重叠修复保留（另一独立覆盖源）。

## 21. 第 7 轮：QQQQ1——"NRFAD 是什么"代码指认（/grill，零应用代码改动）
- 用户问句：NRFAD（前两字母疑似 "ll"，约四分之一被隐藏）这个标记是什么来的；与"本地文件夹标记"同列。
- 本轮 UI 绘制链全读（行号实测于 HEAD 403e3c4e5c = tag v7.2.9-ayu.23）：
  - chip 绘制点：`dialogs/ui/dialogs_layout.cpp:1138-1145`（`p.drawImage(left, context.st->tagTop, *tag)`，自 nameLeft=68px 起横排）。
  - chip 生成：`ui/chat/chats_filter_tag.cpp:235-277`——图高 = `st::dialogRowFilterTagStyle.font->height`（`font(10px)`，dialogs.style:159-161 ≈13-14px）；文本 y=0 全盒绘制，大写墨迹不出盒。
  - 文本来源两类、均 `toUpper()`：①文件夹标签 chip = `filter.title()` 大写（`dialogs_inner_widget.cpp:6004-6008`，无 emoji 文件夹即标题文本 chip；颜色用 filter.colorIndex :6009-6018）；②本地标签 chip = `CacheLocalTag`（:220-244，:225 toUpper；唯一写入方 = 右键 "Set local tags" 菜单 :252-297 → ayu_settings `localPeerTags`；颜色按文本 hash 随机 :232-235）。
  - 行高判定：`dialogs/dialogs_row.cpp:420-436` ComputeSt（taggedDialogRow 72px/tagTop 52px，dialogs.style:114-118；taggedForumDialogRow 96/77）；绘制侧 :1284-1375 与高度侧同谓词（1a073e20cd 后一致，:1353 else-if 分支已死代码）。
  - 覆盖机制：行背景不透明 `fillRect(geometry, bg)`（dialogs_layout.cpp:539）→ 下一行行首盖住上一行超出自身行高的内容（Q3 前 62px 行的 ~4px 裁剪即此）。
- **指认**：NRFAD = "unread" 的 UNREAD 底部裁剪读法（U 失底弧→"ll"、E 失下横→F；"四分之一"=13px chip 的 ~4px）。来源二选一（待回执）：文件夹名 "Unread" 的标题 chip（官方端有 Unread 预设文件夹），或该会话本地标签 "unread"（delivery doc:12 记录的测试标签在案，上一轮排查遗留可能性大）。
- 几何自洽性（HEAD 静态）：52+14=66<72；forum 77+14=91<96；narrow 关 chip（:1285,:1290）；RowsScrollCache 按 rowId+physicalSize 失配即重画、仅滚动期生效 → **未发现第二静态裁剪源**。
- 残余假设（回执分流）：HA 用户 exe 早于 .23（修复不在内）；HB 分数 DPR/字体度量边界（未证实）；HC 旧截图/旧行高状态。
- 三问已发（ask_user_question）：①About 版本号 ②出现视图（文件夹视图/全部聊天/两者）③侧栏有无 "Unread" 文件夹 + 该会话 Set local tags 内容（多选）。
- grill 冻结保持；钉死根因前不动应用代码。附记：用户不满"UI 颜色"可能含 chip 随机取色（本地标签 hash 色），独立项待拍板。
- **回执（2025-11-10 当轮）**：①版本选了 ".23 或更新"（未经 About 原文核实，存疑）；②位置自定义："左边的聊天和频道列表，频道名字下面"（即标签行/预览行区域，未指明视图）；③来源自定义："不知道什么鸡巴玩意来的，**大部分聊天会话都有**"——两处都未按选项核对。
- 回执后修正：**"大部分会话都有"** → 单个本地标签（单会话）不成立；最可能 = **覆盖面很大的文件夹的标题文本 chip**（该文件夹无 emoji → 大写标题 chip 出现在几乎所有成员会话行上，全部聊天视图 exclude=0 全显示）。无 emoji 的英文/拉丁名文件夹 → 用户不认得 → "不知道什么玩意"。词形重建唯一解仍是 UNREAD（ll=失底 U，F=失底 E）。
- **决定性 10 秒判别法（已发给用户）**：进入侧栏每个文件夹视图看该标记是否消失——chip 只画"非当前文件夹"的标签（dialogs_inner_widget.cpp:1303-1306），进"Unread"文件夹视图时其 chip 必消失；任何视图都不消失 → 本地标签（右键 Set local tags 核对）；都不消失且行高与邻行相同 → 非 chip，必须截图。
- 静态结论维持：.23 几何自洽（52+14<72），若确为 .23 仍裁 → 与静态矛盾，须截图取像素真相；4 轮 Q 的共同教训 = 从未取得过一张截图。
- **来源定案（用户回执："确实是 unread"）**：标记 = 名为 unread 的条目的大写 chip；结合"大部分会话都有" → unread **文件夹**的标题文本 chip（其无 emoji）；词形反推 UNREAD（U失底→ll、E失底→F）与用户确认完全吻合。"版本核实"质疑收回（措辞失当）。
- 剩余分叉（唯一决策变量）：用户 .23 构建里该 chip 是否仍被裁四分之一。仍裁 → 违背静态几何 → 出修复改动单前必须拿到截图（标记行 + About 版本同框）；不裁 → QQQQ1 就地结案（前 3 个 Q 为旧构建时期裁剪源，已逐一修复）。
- 用户侧立即可消除项（可选）：给 unread 文件夹设 emoji（chip 变 emoji 不再是文本）或改名/删文件夹。
- 用户确认 .23 仍裁（"我已经23版本了"）→ 第 7 轮深挖，**根因实锤（静态全链路闭合，无需截图）**：
  1. 主列表 Row 创建/重算早于文件夹加载：`_tagColors` 尚空 → `ComputeSt`→`hasChatsFilterTags(0)`=false → **行高 62px 存储固化**（dialogs_list.cpp:35/178/195）。
  2. 文件夹加载/变更：`addToChatList`→fork 强制兜底取色 `setColorIndexForFilterId`（dialogs_entry.cpp:461-469，上游仅显式 colorIndex 才设）**只改 `_tagColors`，不发任何 Height 更新**（:441-452 无事件；removeFromChatList :488-493 同）。
  3. 唯一自愈管线被 premium 门死：`tagsEnabledValue()` 来自服务端 `is_tags_enabled`（data_chat_filters.cpp:478，非 premium=false）→ `dialogs_inner_widget.cpp:583-585` `if (!tags) return;` → `chatListEntryRefreshes` 的 existenceChanged→`updateHeights` 处理器**永不接线**（:588-608）；而 data_session.cpp:5790-5808 的存在变更事件照发无人消费。
  4. 绘制侧谓词已被 fork 解门（WHY 注释："chips synthesized locally…premium flag must not gate"，dialogs_entry.cpp:541-542）→ gate=true → chip 画在 y=52（dialogs_inner_widget.cpp:1294-1296 + dialogs_layout.cpp:1138-1145）于 **62px 旧高行**内 → 底 ~4px 被下行不透明背景（dialogs_layout.cpp:539）盖 → UNREAD→"llNRFAD"。每会话必现、覆盖所有文件夹成员 = "大部分会话都有" ✓。
  5. Q3 修复（1a073e20cd）只统一了"谓词函数"，没修"存储高度缓存永不刷新"——这就是第 4 个 Q 仍裁的原因；fork 解了渲染门没解高度自愈门 = 不完整解门。
- **修复改动单 F1–F3（待用户令下工）**：
  - F1 根因（dialogs_entry.cpp）：`setColorIndexForFilterId` 在 `_tagColors` **size 变化**（0↔1↔N，谓词几何全依赖 size）时补发 `updateChatListEntryHeight()`；值不变不发。所有变更路径（addToChatList/removeFromChatList/tagColorChanged:633/unread-hack）皆经此单点。
  - F2 管线（dialogs_inner_widget.cpp:583-585）：去掉 `chatListEntryRefreshes` 处理器的 `tagsEnabled` 门，与"chips 本地合成不门控"决策对齐。
  - F3 保险（dialogs_layout.cpp:1138-1145）：chip 绘制 y 钳位 `min(tagTop, 行高-chip高)`，任何未来 stale 路径都不可能再裁。
  - 验证：静态三向核验 + 外部构建机 Debug + 手测（全部聊天视图 unread 文件夹成员 chip 完整、切视图、重启）。
  - 实施状态（用户令"开工"）：**F2/F3 已落地**（dialogs_inner_widget.cpp:582-604 去门 + dialogs_layout.cpp:1141-1146 钳位）。**F1 被拦截**：dialogs_entry.cpp 被 session-05b57906 活跃认领（非 stale，不可 force；pending_write 需整文件明文、受占位符协议限制不可行）——待对方 release 后一条 edit 即落地并二次提交。F2 已使存在变更自愈对非 premium 生效（data_session.cpp:5805-5808 在 addToChatList 着色后必发存在事件）；F1 仍必要：data_chat_filters.cpp:816 批量重建直调 addToChatList、不经 refreshChatListEntry、无事件可自愈。

## 22. 第 8 轮：QQQQ3——tint 四连修全部无效，特殊限制频道永远全黑（/grill）
- 症状澄清（用户两轮回执）：第 3 轮的"颜色"不是意图分叉——**症状 = 两处（顶栏群名 Q3.1、列表频道名 Q3.2）全是黑色，tint 特性从未生效过**（"是你的修改0生效，全部都是黑色，根本没有变过色"）。用户要的是：特殊限制频道名字显示区分色。
- tint 提交史（四连修全败）：18c8fd7957（建 peer_tint，接线两处）→ 9cb8ededbe（加 AyuRestrictedContent→RegionRestricted）→ 5272ab2778（去 date!=0 守卫，自称 "never reaching forbidden channels... rows stayed plain black"）→ 0d0c001b68（AyuForbidden 持久化）。
- 静态验证（本轮全部实测）：接线 ✓（PaintRow `from` 分支 :1044-1057 + 顶栏 :748-762，均真路径）；置位点 ✓（channelForbidden→Forbidden+AyuForbidden data_session.cpp:1298-1300；is_restricted→AyuRestrictedContent :1217；noforwards→AyuNoForwards :1207）；位无冲突 ✓（上游最高 1<<48，fork 61/62/63；Chat 侧 1<<30/31）；样式 ✓（info.style:915-932 六色）；绘制链 ✓（RowPainter::Paint→PaintRow）。
- **四轮共同盲区：零运行时验证**——没有任何一轮确认过用户的频道实际带什么旗标。静态全对 + 用户全黑 ⇒ 唯一剩余解释：用户的"特殊限制频道"不落进任何判定类（region 封禁频道服务端可能不发 restricted/forbidden 旗标；或"特殊限制"是用户自定义概念，四轮全在猜错的目标集合上修）。
- 本轮动作：已发 ask_user_question（频道打开后的表现 + 主题），答案将钉死旗标类；若静态判定类全覆盖仍黑 → 下轮加运行时探针（applyPeerChanged 旗标变更走 ayu_log，一次构建周期取证）。grill 冻结保持。
- **回执定案**：①频道打开后"除了没有消息之外，其他一模一样"= 首载**空成功**（非 channelForbidden 弹窗、无限制提示页、无 restricted/noforwards 旗标）②官方默认主题（排除调色板掩蔽）。⇒ **根因定案：目标集合错误**——四轮全在修"服务器旗标可见性"（Forbidden/is_restricted/noforwards），而该类频道服务端**静默拒发消息**，peer 对象是普通频道、一个旗标都不带 → `PeerTintOf` 恒 None → 永远黑。前四轮不是"没修中点"，是"修的对象根本不存在于数据"。
- 现成信号盘点：history_widget.cpp:4760 messagesFailed 对 CHANNEL_PRIVATE 族只 closeCurrent+toast 不落状态；apiwrap.cpp:3690-3695 fail 同族发 `notifyHistoryAccessLost`（瞬态事件，data_session.cpp:2592-2597，仅 chat_section.cpp:980 消费）→ 无持久状态可复用；首载成功空历史（apiwrap.cpp:3680-3684 addSlice 空片）完全无人标记。
- **G1–G4 已实施（用户令下工）**：
  - G1 history_widget.cpp firstLoadMessages done（~:5034）：FirstLoadCameBackEmpty（四构造 match：messages/messagesSlice/channelMessages 判 vmessages 空；messagesNotModified 不算）→ MarkSilentlyRestrictedChannel。**路线图修正：首载真实路径 = HistoryWidget::firstLoadMessages（history_widget.cpp:5029 histories.sendRequest），非 apiwrap:3680 的跳转/搜索请求；apiwrap 未覆盖。**
  - G2 history_widget.cpp messagesFailed（:4766-4772）CHANNEL_PRIVATE/CHANNEL_PUBLIC_GROUP_NA/USER_BANNED_IN_CHANNEL 族 → 同 G1（close/toast 行为不动）。
  - G3 ayu_settings.h/.cpp：`std::unordered_set<uint64> _specialRestrictedPeers` + isSpecialRestrictedPeer/setSpecialRestrictedPeer（镜像 shadowBanIds 的 json 直序列化双向模式），随 settings 落盘跨重启。
  - G4 peer_tint.cpp 频道分支最前查集合 → Banned 红；MarkSilentlyRestrictedChannel（history_widget.cpp 匿名 ns）置位后 updateChatListEntry + peerUpdated(UnavailableReason)，列表行与顶栏同拍重绘。
  - 行为注记（物理上限，如实告知）：染色自该频道**首次打开之后**生效并跨重启持久；从未打开过的静默受限频道无任何客户端信号可辨，仍是黑。不新增 ChannelData 旗标（AyuForbidden 会被全量 apply 清掉，复用必死）；不动 close/toast 行为。

## 23. 第 9 轮：QQQQ10——sexcat 下载文件移动/改名后 Current path、File name 不更新（/grill）
- 症状：A. 移动下载文件到其他文件夹后 Current path 列不更新；B. 改名后行变灰、File name 不更新（sexcat_history_window 两列 lng_sexcat_col_path/name :170-172）。
- 机制实测：lost 灰显 = `!QFileInfo::exists(savedPath)`（:352，灰刷 st::windowSubTextFg :484-490，lost 单独分组 :444-453）；自动追踪 TryAutoRelocate（shell_watch.cpp:193-229）按**名字+大小**扫描固定候选根（桌面/下载/文档/旧目录/最近条目目录 :130-166），失败仅 LogFailure 无 UI 提示（:226-228）→ 移动到候选外 = 静默卡灰。右键重定位：非 lost 行走文件选择器（sha256 校验、名字无关，finishRelocation :596-624 → Database::Relink **原地 UPDATE** path+file_name sexcat_database.cpp:399-427 + _newSaved → 窗口 reload :301-304 ✓）；**lost 行被 :572-574 强制走目录扫描** relocateByDirectory → LocateByNameSize(dir, 旧名, size)（:644）——**改名文件永远找不到（死路）**，且哈希校验路径对灰行不可达。finishDirectoryRelocation → RelinkMovedEntry（shell_watch.cpp:171-190，同样走 Relink 原地更新 + ADS/sidecar 迁移）→ reload ✓。
- **根因定案（回执补全）**：用户回执 = 全部资源管理器手动操作，右键菜单从未用过。A = 手动移动 → TryAutoRelocate 候选根不含新目录 → LogFailure 静默失败（:226-228）→ 卡灰+旧路径；用户不知道右键可救援（失败无提示 = 缺陷本身）。B = 手动改名 → 灰行唯一可用通道（目录扫描）按旧名匹配永远失败，哈希通道被 :572-574 路由挡死 → 死路。H2（LocateByNameSize 排查）作废——用户没用过菜单。
- **H1 已实施（用户令开工；session-05b57906 心跳 10h 停跳判 stale 后 force 接管 sexcat_history_window.cpp + lang.strings；CONTEXT.md 一并接管）**：
  - relocateRow 撤掉 lost→relocateByDirectory 强制路由（原 :572-575），灰行直接走文件选择器（sha256 校验、名字无关——改名/移动通吃；选错弹 hashMismatch 无害）。
  - showContextMenu：灰行在 "Relocate..." 下补 "Relocate by folder scan..."（新键 lng_sexcat_relocate_dir，lang.strings:9230）→ relocateByDirectory；目录扫描保留为第二入口（只移动未改名时按名字+大小找）。
  - 生效链：finishRelocation → Database::Relink 原地 UPDATE path+file_name（sexcat_database.cpp:399-427）→ _newSaved → 窗口 reload（:301-304）→ 两列当场刷新 + 落库跨重启。
  - 此前计划"不动 lang.strings"作废：lng_sexcat_scan_folder 是工具栏全局导入扫描的键，复用会混淆，故新增专用键。

## 24. 第 10 轮：QQQQ10 续——用户嫌手动右键麻烦，H2 自动救援（/grill）
- 需求：移动/改名后零手动，窗口自己跟上。
- 缺口实测：shell watch Windows 通知只注册桌面文件夹/下载/文档三子树（shell_watch.cpp:470-511），用户收藏树不在内 → 无事件；救援扫描按旧名匹配（改名盲）+ 候选根有限 + 失败静默（:226-228）；`RelocateMissingEntries` 只挂"刷新"按钮（history_window:218），窗口打开不跑。
- **H2 已实施**：
  - `RelocateMissingEntries` 重写为单次批量遍历：灰行建 size→entries 索引，后台线程按优先级根（旧目录 → 候选根 → 丢失文件盘符根）一次走完；命中三级判定：同大小+同名（免费）→ 同大小+ADS 标记（同卷移动标记随文件走，微秒级）→ 同大小+全量哈希（≤512MB cap，kRescueHashCapBytes）。改名/移动通杀，一次遍历服务全部灰行。
  - 主线程只收结果 Relink（crl::on_main），不卡 UI；成功 toast "Auto-relocated: {count}"（新键 lng_sexcat_autorelocate_done）。
  - `RelocateMissingEntries` 挂上窗口构造（history_window ctor reload() 后）→ **打开窗口即自动救援**。
  - 哈希助手迁 `HashStore::ComputeFileSha256`（hash_store.h/.cpp 新增同步版），history_window 三处改用、本地副本删除；shell_watch 增 <map>/toasts/lang_keys includes。
- H1 的两个右键入口保留为兜底。
- **H3 已实施（全局 shell 事件，用户拍板）**：SHChangeNotifyRegister 注册面从"桌面文件夹+下载+文档三子树"换成 **CSIDL_DESKTOP 命名空间根**（SHGetFolderLocation）递归注册 = 全盘所有驱动器的改名/移动/删除事件；事件面裁掉 SHCNE_UPDATEITEM（全局范围下每个文件内容变化都触发会成 IO 风暴）；HandleDeleted → 5 秒冷却（kRescueCooldownMs）触发批量救援；HandleRename 的 miss 日志静音（全局范围下未跟踪文件改名是常态）；HandleUpdate/TryAutoRelocate/RelocateInFlight 删除（单行旧名扫描被批量三级匹配全面取代）。同卷移动/改名/Ctrl+X = RENAMEITEM 即时精确重链；跨卷 = DELETE → 批量救援兜底。
- **H4 已实施**：USN 日志方案否决（SDK 事实：卷句柄 GENERIC_READ 需提权，非管理员 ERROR_ACCESS_DENIED，Everything 装服务即为此；Telegram 不提权 → 空转死代码）；等价落法 = `RelocateMissingEntries()` 挂进 sexcat 模块启动序列（save_hook，StartShellWatch 之后）→ 应用关闭期间的移动/改名下次启动自动找回。

## 25. 第 2 轮：Q2——有下载记录的视频消息看不出标记（/grill，诊断轮）
- 症状：有些带下载记录的视频，消息上看不出标记；用户要求一个合适的视觉效果。
- 实测双徽章系统并存：
  - **SexCat::Badge**（sexcat_badge.cpp:252 PaintBadge）= 下载记录徽章：右上角裸图标（无底板，style_sexcat 的 sexcatBadgeIcon），photo.cpp:501 / document.cpp:1130 / media_grouped.cpp:574 三个视图有画；**history_view_gif.cpp 完全没有**（Gif 视图 = 视频播放态/GIF/圆视视频）。
  - **ayuDownloadedBadge**（gif.cpp:101 本地实现）= 底板圆角矩形+对勾（ayuDownloadedBadgeBg/Check），只画在 Gif 视图，四道闸门：`!isRound`（圆视永不显示）、`DownloadedBadgeFits`（小图跳过）、`loaded || loadedInMediaCache || IsDownloadedDocument`、非剧透/TTL。
- **漏洞链**：IsDownloadedDocument 基于**会话内存 set**（RememberDownloadedDocument，save_hook.cpp:1203-1218），重启即空 → 视频"未加载 + 不在媒体缓存 + 本会话没下过"时四条件全灭 → 有记录也不画（= "有些"的成因）；圆视视频永不显示；同一视频缩略图态（Document 视图 = sexcat 裸图标）与播放态（Gif 视图 = ayu 底板对勾或没有）**外观还会切换**。
- 视觉问题：sexcat 裸图标叠在视频内容上无底板，亮帧/花帧上不可读。
- 改动单（三问已答，定稿）：**视觉形态 = 媒体四周细色边框**（用户选"角标条/边框"，替代角标chip形态）；**圆视视频显示**（圆形环线）；**sexcat 徽章为主，ayuDownloadedBadge 让位**（无记录但已下载才画旧对勾）。
- 实施清单（边框色定稿：**金+暗双圈描边**，用户嫌单色会被同色系视频帧吞掉，否决流光动画——列表持续重绘性能付不起）：① sexcat.style 加 `sexcatMediaFrameGold: #f2b544`（内圈 2px）+ `sexcatMediaFrameOutline: #1f1f1f`（外圈 1px）+ 两宽度变量——外圈职责=把金圈从任何亮/花/同色帧上剥离，双圈任何背景都有分离度；② `SexCat::Badge::PaintBadge` 重绘为双圈描边（新增 radius 参数，radius<0 = 圆视环形），BadgeRect 悬停区不变；③ photo.cpp:501 / document.cpp:1130 / media_grouped.cpp:574 传各自圆角半径；④ gif.cpp 补 PaintBadge + Gif::textState:1436 悬停/tooltip（照 photo.cpp:784-806 模式），isRound 传环形，剧透/TTL 门沿用；⑤ ayuDownloadedBadge（gif.cpp:835-845）包上 `!ShouldShowBadge` 让位闸门。
- **Q2-H1 已实施**（提交见 git log）：五处全部落地；附加清理 = 旧 chip 样式变量（sexcatBadgeFg/Bg/Icon*）与 `IsHovered` 死 API 删除、badge.cpp 补 QPainterPath/<algorithm> include；边框圆角 v1 统一 st::roundRadiusLarge（分角映射待视觉验收再说）。

## 26. 第 26 节：系列攻坚战 QQQQQ49 / QQQQQ50 / Q11 / Q12 / Q13（/grill，诊断轮）
- 前轮底盘：§16 已有 Q49/Q50 审计；实现提交 `fbf2354d38`（captioned names + batch save-as + cancel-all）与 `20b0f94388`（write-point 改名 + 相册视频挑选）已在 HEAD。
- **Q13 根因已坐实**（"下载历史"= sexcat 历史窗口——tdesktop 下载页根本不显示批量直写照片，方向反推锁定）：图片链 `saveImages` 直写 + 显式 `RecordPhotoSaveAttempt`（menu_item_download_files.cpp:245-249）→ 有记录；视频链 `document->save(origin, target)`（:281）在**视频已在缓存**时走 `saveFromData` 瞬时落盘，不触发 loaded 事件 → `HandleLoadedDownload` 永不执行 → `MarkDocumentSaveAs`（save_hook.cpp:953-962）pending 永不解析 → **无 sexcat 记录**。同类洞：data_file_click_handler.cpp:114-125 单条快速保存已缓存文件同样不记录（:101-104 WHY 注释的"must not record"门与现需求冲突，需改语义）。
- **Q11 根因已坐实**：图片名是客户端生成的 `photo_N.jpg`（:221-224），无增补；且增补门 `IsAllDigitsName`（save_hook.cpp:702/:733）按设计排除非数字名——图片增补需绕过该门（图片无原始名可保，photo_N 作尾段保证唯一性）。
- **Q12 初判**：批量选文件夹已改名（:276-281 AdjustDocumentSaveTarget）；批量快速保存也改名（:283 SaveAndTrack → addLoading{item} → HandleLoadedDownload:771 MaybeAugmentedSavePath，item 携带 ✓）——HEAD 静态看已实现，疑用户构建旧于 `20b0f94388`，待答问确认。
- **Q49 未在 HEAD 静态复现**：三条活链全部带 original（BuildAugmentedName:665 `text + '-' + original`；MaybeAugmentedSavePath:705/BuildCaptionedNameFor:753/AdjustDocumentSaveTarget:1069）——用户实测丢 `516156156.mp4` 的形状 = original 传空（尾杠后空）。死代码 `AugmentedDocumentName`（:1054-1058，document->filename() 可空）无调用方，排除。待用户提供**实际产出的完整文件名**与所用流程，字符串形状直接定位断点。
- **Q50**：一键取消已存在于下载页顶栏菜单（info_downloads_widget.cpp:106-114，`cancelAllLoading`，`loadingInProgress()` 门控）——用户 5 轮未找到/不在其操作流。待问期望位置（顶栏常显/批量 toast 入口）。
- 修复清单（待三答定稿）：Q13 = saveDocuments 写点 + data_file_click_handler saveFromData 分支补即时记录（共享 helper）；Q11 = 图片名走 captioned 增补（绕 digits 门，photo_N 尾段）；Q12/Q49 = 按答出的流程与字符串定断点；Q50 = 按所选位置落取消入口。
- **三问答复定稿**：Q50 = **下载页顶栏常显 + 批量启动 toast 提示**；Q11 格式 = **`#tags -描述-photo_N.jpg`**（保留 photo_N 尾段保唯一性）；Q49/Q12 = 用户答 **"都不行"**——三条流程全部复现丢原名，不再追问流程与产出串，修复必须**流程无关**（choke-point 硬化）。
- 追加根因（第二层）：`DocumentFileNameForSave`（data_document.cpp:298-300）对无服务器名视频产出 `.mp4` 纯扩展名或空名 → 增补门 `IsAllDigitsName` 不触发；批量链 `menu_item_download_files.cpp:274-275/:359-360` 用 `document->filename()` 造名，空名时 target 退化为目录路径。修复核心 = `FallbackDocumentName`（filename() 空 → `<documentId><mimeExt>` 兜底，516156156 即文档 id 的形状）+ `BuildAugmentedName` 空 original 守卫（永不再产出尾杠空名）。
- **定稿改动单**（等"开工"）：① save_hook.cpp/.h：`FallbackDocumentName` 公开 helper；`BuildAugmentedName` 空 original 早退；`AdjustDocumentSaveTarget` 目录式 target + 兜底 original；新增 `AugmentedPhotoSavePath(dir, index, item)`（Q11：绕 digits 门、photo_N 尾段、UniqueAugmentedTarget 去重）；② menu_item_download_files.cpp：两处批量造名改用 FallbackDocumentName；saveDocuments/savePicked 在 `document->loaded()` 时直呼 `RecordSaveAttempt(target, document, origin, 0, batchTag)`（Q13 补记录 + 改名从磁盘名再保险）；saveToFiles 用 AugmentedPhotoSavePath；批量启动 toast（Q50，photos+documents>1）；③ data_file_click_handler.cpp:114-125：快速保存已缓存文件也记录（去掉 explicitSaveAs 门），WHY 注释语义同步更新；④ info_downloads_widget.cpp:109-114：去掉 `loadingInProgress()` 门控常显取消项；⑤ lang.strings 新增 `ayu_BatchDownloadStarted`。注释预算：常规 1 行（FallbackDocumentName）+ WHY 例外 1 次（Q13 记录时机）+ data_file_click_handler 注释原位改写（净 0）。
- **Q49 实锤样例（用户给定）**：服务器名 `6.mp4` → 期望 `#抖音-哈哈哈-6.mp4`，实测 `#抖音-哈哈哈.mp4`（stem `6` 被吞、扩展名保留 = `文本 + 扩展名` 形状）。**HEAD 全链路逐环验证无此生成点**：BuildAugmentedName:665 `text + '-' + original`（保 stem）✓；UniqueAugmentedTarget:680-686 碰撞重排保 stem ✓；FileNameUnsafe（data_document.cpp:229-241）nameStart+extension 恒等重组 ✓；DocumentData::save:1289-1295 直写 toFile 原名 ✓；下载管理器仅透传 `loadingFilePath()`（data_download_manager.cpp:265/:455），无任何名字合成 ✓；`9a7dc4709c` 之前无 BuildAugmentedName（builder 随该提交带 append 引入）✓。**结论：用户测试构建 = 命名链"半实现"中间态（不含 original-append 或写点接线）**；QQQQQ49/QQQQQ50(11-12 轮) 的修复 = 从当前 dev 头重建 + 本改动单硬化（空名兜底/空 original 守卫消灭任何半实现态形状）。不改 judge 死代码补丁（HEAD 无法产生该形状，repair pass 即死代码）。
- **已实施（本轮，待构建验证）**：① save_hook：`FallbackDocumentName`（filename() 空 → `<id><mimeExt>`）+ `BuildAugmentedName` 空 original 守卫 + `AdjustDocumentSaveTarget` 目录式 target 兜底 + `AugmentedPhotoSavePath(dir, index, item)`（Q11：photo_N 尾段、UniqueAugmentedTarget 去重，存盘时经 contextId 现查 item 防悬垂）；② menu_item_download_files：`quickPath` 收敛（saveImages 复用 + savePicked 自带解析）；批量快速保存**已缓存视频改直写默认下载目录**（原 `SaveAndTrack → Save:93 saveToCache` 分支对缓存文件零动作 = Q13 真洞）；两处造名用 FallbackDocumentName；批量启动 toast（>1 项）；③ data_file_click_handler：saveFromData 分支记录门去掉 explicitSaveAs（净 0 注释改写）；④ info_downloads_widget：取消全部常显；⑤ lang.strings `ayu_BatchDownloadStarted`。**记录契约澄清**：`DocumentData::save:1310` 瞬时写分支自带 `NotifyDocumentSaved` → pending → `RecordSaveAttempt`——直写路径记录由 MarkDocumentSaveAs pending 全覆盖，**显式 RecordSaveAttempt 调用会双记录，已撤**；注释预算最终 = 2 行常规 + 1 处原位 WHY 改写。**遗留已知洞（未修，不在本轮报告范围）**：单条快速保存已缓存文件仍走 `Save:93 saveToCache` 零动作分支（upstream 语义，改动半径大，待单独立项）。

## 27. 第 11 轮：QQQQQ49（.24 实测）——简介/tag 整体消失 + 相册批量全不改名（/grill，只读取证）
- 发布对账：远端 tag `v7.2.9-ayu.24` = `[R:30fd382c]` = `e32d3d8748` = 本地 HEAD（git ls-remote 实测）→ **用户二进制与本 checkout 源码一致，§26 的"旧构建中间态"论作废**，缺口在运行时分支。
- 增补链全景（HEAD 静态，全部在位）：单条快存注入 data_file_click_handler.cpp:144-147（`!explicitSaveAs` 门）；流缓存写点 saveFromDataChecked（data_document.cpp:1564-1569）；Save As 对话框默认名 :135-137（仅 ToNewFile）；批量直写 menu_item_download_files.cpp:285-290 / savePicked :394-399；管理器兜底 HandleLoadedDownload→MaybeAugmentedSavePath（save_hook.cpp:775）；落盘后改名 NotifyDocumentSaved→RecordSaveAttempt→MaybeAugmentedSavePath（:968-981→:905-930→:695-726）。BuildCaptionedNameFor 四门 = digits stem / item 解析 / Enabled() / caption 非空（:731-763）。
- **N1 实锤新洞（.24 引入，quickPath 重构副产物）**：saveDocuments（menu_item_download_files.cpp:279-286）与 savePicked（:388-395）的 `dir + name` 裸字符串拼接。quickPath = 原样 `settings().downloadPath()`（:154-166/:360-371），而自定义路径落库**不带尾斜杠**（download_path_box.cpp:127-138 `computePath()` 原样 `_path`；仅 mac specific_mac_p.mm:352 补 '/'；默认目录与 Tmp 分支同理无保证）。全仓其他 join 全部归一化（FileNameUnsafe data_document.cpp:225-227、filedialogNextFilename file_utilities.cpp:113-117、ChooseFolderAndSave :94-96；saveImages 走 `QDir(dir).filePath` 不受影响）。后果（自定义/临时目录 + 快速批量下载）：文件落 `<父目录><文件夹名><原名>.mp4`，stem 混入文件夹名 → `IsAllDigitsName` 失败 → 增补跳过 + 文件错位。命中「Download all album files」；「Save all album files as...」目录框路径恒尾斜杠（ChooseFolderAndSave :94-96）不受 N1 影响。
- **N2 ask-every-time 单条链（.23 既有，未修）**：askDownloadPath=开 时 FileNameUnsafe 对话框条件 `askDownloadPath() || savingAs`（data_document.cpp:165）→ 单条快存（ToCacheOrFile）弹对话框且默认名 = **裸原名**（`already` 仅 ToNewFile 才注入 AugmentedDocumentName，data_file_click_handler.cpp:135-137）；:147 注入被 `!explicitSaveAs` 门跳过 → 唯一命名机制 = 落盘后 MaybeAugmentedSavePath 的 `QFile::rename`，失败仅 LogFailure（H2 老洞）→ 单条保存可永久裸名。
- **N3 去重静默跳过（§18.4 HA 复活面）**：`SexCatShouldSkipDuplicate`（data_document.cpp:1329-1340）对已记录 (document,toFile) 直接 return + toast「Already in library, download skipped」（lng_sexcat_already_saved，save_hook.cpp:1119-1126）→ **测试期反复重下同一相册时 .24 链路什么都没写，用户看到的是旧构建时代的裸名遗留文件**；save-as 直写分支的 not-loaded 情况也走此分支（activeMediaView 空时）→ N3 可同时解释「save all album files as 没生效」的复现体感。
- **N4 未静态复现**：全新相册 + 「Save all album files as...」在 HEAD 源码静态全链路正确 → 用户实测失败需 AyuLog 分流（Enabled/来源 item/digits/caption 空 四门哪个先断）。
- 取证面（已内建，无需新构建）：`<exe目录>/tdata/logs/ayu-YYYYMMDD.log`（ayu_log.cpp:14-17），既有埋点 = `save: mode/explicit/local`（data_file_click_handler.cpp:110）、`augment ... skipped: disabled|no source item|no caption text`（save_hook.cpp:750/754/761）、`save target augmented`/`failed to rename`/`save name augmented`、`duplicate download skipped`。
- 本轮三问（ask_user_question 已发）：①下载设置形态（每次询问/自定义路径/默认目录）②产出确切形状+目录+already-saved toast+是否新相册（多选）③修复面：先 AyuLog 取证再修 vs 直接修 N1+N2+改名失败显式提示。grill 冻结保持，等回执。
- **回执（当轮，三问均未按选项作答，改给自由文本）**：①两个相册动作（Download all album files 无文件夹选择 / Save all album files as 可选文件夹）下载到的都是原名；单条另存为本次没试，但**对话框/已有文件看到的是原名**就没继续。②"我的实例是裸数字名，但实际下载的都是服务器下发的文件名"——原始名是服务器下发名，不一定全数字。③"上次是可以拿到简介和tag的"——.23 行为为基准，.24 是回归，要求直接修，不再取证。
- **定案（第 11 轮收束，歧义预算用尽）**：不再追问配置形态；按"上一版能拿到、这一版丢了"= .24 回归处理。
  - 增量锁定：.23 tag（token `34a6cafc`）与 .24（`e32d3d8748`）之间触及命名链的提交 = **仅 `9d3c146463` + `e32d3d8748`**（403e3c4e…08f87928 八连均为 tint/tag chip/rescue/gold-ring，不碰命名）。回归面必在其中 = N1 直写分支重路由 + 裸拼接（实锤）。
  - ".23 能拿到"的机理：.23 快速批量对未缓存视频走 SaveAndTrack→loader 注入（:147）拿增补名；对已缓存视频是零动作（旧增补文件保留）。.24 把已缓存视频改道进直写分支，撞 N1 拼接 bug（自定义/临时路径无尾斜杠 → `IsAllDigitsName` 失败 → 增补跳过）→ 看过的视频越多，.24 落裸名越多 = "上次能、这次不能"。
- **定稿改动单 N1–N5（等"开工"令，grill 冻结中）**：
  1. **N1 join 归一化**（menu_item_download_files.cpp saveDocuments/savePicked）：`AdjustDocumentSaveTarget(QDir(dir).filePath(name), …)` 替换 `dir + name`；quickPath 出口统一归一化（补尾斜杠）。saveImages 链已走 QDir().filePath 不动。
  2. **N2 对话框默认名全模式注入**（data_file_click_handler.cpp:135）：`mode == Mode::ToNewFile` 门放宽为 `already.isEmpty()`，ask-every-time 的单条快存对话框也拿增补默认名。
  3. **增补触发面放宽（拍板项，默认做）**：`BuildCaptionedNameFor`/`MaybeAugmentedSavePath` 去 `IsAllDigitsName` 硬门，改"消息有 tag/简介即增补，原名恒为尾段"（幂等：名字已带前缀则跳过）；保留 240 截断 + Win 非法字符清洗。依据：用户实测服务器下发名不全数字，口径 = 有简介和 tag 就要在文件名里。
  4. **落盘后改名失败显式化**：MaybeAugmentedSavePath rename 失败（save_hook.cpp:714-716）LogFailure 升级 toast，不再静默。
  5. **N3 去重自愈**：SexCatShouldSkipDuplicate 跳过时若现存文件名未增补，就地 QFile::rename 补增补名（重下旧相册即自愈，不再干看旧裸名文件）。
  - 验证：静态三向核验 + 外部构建机 Debug + 手测（单条快存×2 设置形态 / Download all album files / Save all album files as / 重下同相册）。
- **已实施（当轮令下即落码，commit `fa08503f28`，未 push）**：
  - N1 quickPath 归一化（menu_item_download_files.cpp:154-170/:361-377 出口补尾斜杠）+ saveDocuments/savePicked 两处 join 改 `QDir(dir).filePath(name)`（:289/:398）。
  - N2 data_file_click_handler.cpp:134-141：对话框默认名注入门 `mode == ToNewFile` → `explicitSaveAs`（ask-every-time 的单条快存对话框也拿增补默认名；已本地文件同样覆盖）。
  - N3 save_hook.cpp：`IsAllDigitsName` 全删；`BuildAugmentedName` 改"parts 非空即增补 + 前缀 startsWith 幂等守卫"（:645-678）；`BuildCaptionedNameFor`/`MaybeAugmentedSavePath` 数字门移除；头文件 AdjustDocumentSaveTarget 注释同步。
  - N4 MaybeAugmentedSavePath rename 失败 → toast `lng_sexcat_rename_failed`（lang.strings:9218 新键）。
  - N5 `HealSkippedDuplicate`（save_hook.cpp:1118-1141，WHY 注释）：FindRecordedForDocument 命中的旧记录文件就地改名 + `Database::Relink(hit->id, renamed)`；`NotifyDuplicateSkipped(toFile, document)` 签名扩展（h:98-101 + data_document.cpp:1338 调用点），toast 显示治愈后的实际路径。
  - 静态验证全绿：`git diff --check` 干净；`IsAllDigitsName`/`dir + name` 全仓 0 残留；NotifyDuplicateSkipped 三向一致（h/cpp/call）；lng key 定义↔使用配对；`DocumentFileNameForSave` 默认参重载确认（data_document.h:437-441）；注释预算 = 1 常规 + 1 WHY 例外 + 1 处原位改写。
  - 遗留边界（如实）：mid-loading 命中 :147 注入时 loader 可能 setFileName 失败→cancel 重启下载（upstream 既有语义，产增补名，代价是重下流量）；幂等守卫按首 tag 前缀判定，服务器名恰好以 "#tag" 开头时保守跳过。

## §28 Grill Q31–Q33：相册菜单视频化 + source 多 tag —— 调查完成，待裁决
- **Q31/Q32 事实面**：`AddDownloadAlbumAction`（menu_item_download_files.cpp:533-563）用 `Collected` 同时收集 docs+photos，`AddAction` 的 saveImages+saveDocuments 都跑 → 相册两动作确实连图片一起下载。现有文案 `ayu_DownloadAlbumAll`="Download all album files"、`ayu_DownloadAlbumAs`="Save all album files as..."（lang.strings:9182-9183）；lang.strings 全仓 CJK=0（自定义文案纯英文，无中文默认值机制）。"Select videos to download..." 选择器本就只收视频，不受影响。多选路径 `ayu_DownloadFilesAs`="Save selected as..." 不在本次范围。
- **Q31/Q32 拟改**：AddDownloadAlbumAction 只收 documents、`docs.empty()` 早退；两动作文案改"视频"语义。
- **Q33 事实面**：`SourceTagForItem`（sexcat_save_hook.cpp:1049-1051）= `AugmentNameParts(item).tags.value(0)` —— 只取首个 hashtag（bug 根源）。4 个调用点（menu batchTag ×3 :135/:144/:383 → MarkDocumentSaveAs → OriginFromItem；OriginFromItem 回退 :333）全部喂 `Entry.src_tag`（写入点 :346/:406/:439/:490）；`RecordTag(origin.tag)` 在 :414/:496 单 tag 入注册表。读侧：`Recent()` 整串精确 ==（sexcat_database.cpp:460-469）；`TagSummaries` GROUP BY src_tag（sexcat_database.cpp:635-660）喂 retro-tag 候选菜单（save_hook:1183）；sow 入口 = SaveSowTag（history_inner_widget.cpp + history_view_context_menu.cpp）。文件名增补本就带全部 tag（BuildAugmentedName join 全部），不受影响。
- **Q33 拟改**：SourceTagForItem 返回全部 tag 空格连接；写入点与签名不动；RecordTag 逐 tag 循环注册。读侧形态待裁决：(a) joined + Recent 改 token 匹配 + TagSummaries 客户端拆分聚合（零 schema 变更，旧单 tag 行天然兼容，推荐）vs (b) 多对多新表 source_tags(source_id, tag)（读取面最干净但 schema+全部读写改）。
- **用户裁决（当轮）**：① 文案 = 英文等价；② 纯图片相册 = 补图片兜底入口；③ Q33 = 多对多新表。
- **补充事实（裁决后核验）**：AddAction 对空 docs 完全容忍（文案/图标/toast 三处 fallback，overrideText 非空时无碍）；saveImages 的 batchTag 只遍历 documents —— 图片批量经 `RecordPhotoSaveAttempt(..., sourceTag)` 单独带 tag（:253-258），Q33 管线必须覆盖该参数；`Recent()` 的 tag 过滤当前无任何 UI 调用方（仅 4 写点 + :1020 读回）；storage 有现成 make_index 模式（:53-69）；`RetagSource` = update_all 整体替换语义。
- **定稿改动清单**：
  - **A. Q31/Q32 相册菜单视频化 + 图片兜底**：
    - A1 AddDownloadAlbumAction：两动作 + 视频选择器收进 `!docs.empty()` 分支且只传 docs；新增 `!photos.empty()` 分支 → 单一图片快存入口（无对话框）。
    - A2 lang.strings：`ayu_DownloadAlbumAll`="Download all videos"、`ayu_DownloadAlbumAs`="Save all videos to folder..."、新键 `ayu_DownloadAlbumPhotos`="Download all photos"。
    - A3 多选 "Save selected as..." 与视频选择器不动。
  - **B. Q33 多对多 source tag**：
    - B1 新表 source_tags(source_id, tag) + make_index（tag / source_id）；sync_schema 自动建表，旧库零迁移。
    - B2 一次性回填：初始化时 source_tags 空且存在 src_tag 非空行 → 逐行 {id, src_tag}。
    - B3 写链：`SourceTagsForItem(item)` 返回全部 tag（SourceTagForItem 保留首 tag 喂 src_tag 列）；SourceOrigin 增 tags（tag 派生 value(0)）；OriginFromItem/menu batchTag ×3/PendingSaveAs/RecordPhotoSaveAttempt 的 tag 参数全部改 QStringList；4 个 Entry 写点后 ReplaceSourceTags(entry.id, origin.tags)；RecordTag 逐 tag 循环。
    - B4 读面：Recent() tag 过滤两段查询（source_tags → id in）；TagSummaries 改 source_tags 聚合；RetagSource 保持替换语义（update_all + 命中 ids ReplaceSourceTags(id,{tag})）；save_hook:1020 读回改 SourceTags(id) 优先、src_tag 兜底。
    - B5 验证：静态三向 + 外部构建机 + 手测（多 tag 消息 → retro-tag 候选列全 tag；tag 过滤命中任一；纯图片相册出图片入口；混合相册两视频动作不再带图片）。
- **已实施（commit `42d79820cd`，未 push）**：
  - A 落码：AddDownloadAlbumAction 两视频动作 + 选择器收进 `!docs.empty()` 分支只传 docs；`!photos.empty()` 分支出单一图片快存入口（默认 menuIconSaveImage 图标）；lang 三键（两改一新）。多选路径与选择器未动。
  - B 落码：source_tag 表（两索引先于表，沿用"索引先于表"约定）+ `BackfillLegacySourceTags`（ctor 内独立 try/catch，count==0 守卫）+ `ReplaceSourceTags`（事务内 remove+insert，去重滤空）/`SourceTags` 读法；写链 QStringList 化：PendingSaveAs.sourceTags、SourceOrigin.tags、OriginFromItem 签名、SourceTagsForItem（替代 SourceTagForItem，全仓唯一来源）、RecordSaveAttempt/RecordPhotoSaveAttempt/MarkDocumentSaveAs 参数、menu batchTags ×3；写点三处（DedupeOrRecordHashed fresh 分支接住 RecordSaved 返回 id + pendingId 分支、ResolveHashAndPersist rowId 处）；RecordTag 循环 ×2；RehashPendingRows 重建 origin 补 `.tags = SourceTags(entry.id)`；读面：Recent tag 分支改 `join<SourceTagRow>`（INNER JOIN 保分页下推）、TagSummaries 改 join 聚合、RetagSource 事务化重写命中 ids、RenameTag 逐 id 改写防 (source_id,tag) 冲突、DeleteTag 同步清行。
  - 计划外修正（如实）：saveImages 移除 `folderPath.isEmpty() continue` —— 快存图片此前完全不落记录（Q52 时代遗留，与文档路径不一致），移除后快存图片与文件夹保存同样记录（顺带兑现图片入口的 tag 承诺）。
  - 静态验证全绿：`git diff --check` 干净；`SourceTagForItem`/`batchTag`/`sourceTag` 全仓 0 残留；新符号三方一致；外部调用方核验（history_inner_widget/context_menu/admin_log/message_history 的 RecordPhotoSaveAttempt 3 参、media_view_overlay/data_file_click_handler 的 RecordSaveAttempt 3 参，均走默认 `{}` 兼容）；menu 文件 QStringList 按 AyuGram includes 规则补 `<QStringList>`；注释预算 = 1 常规 + 1 WHY 例外。
  - 构建机关注点（如实）：`get_all<T>(join<B>(on(...)))` 返回左表对象与 `select(columns(...), join(...))` 为 sqlite_orm 文档化 API，本机无头文件可查，若版本差异报错按两段查询回退。

## §29 Grill QQQQQQ10：移动文件后记录路径不更新 —— 调查完成，待裁决
- **现状三链路**（sexcat_shell_watch.cpp 570 行）：
  1. **shell 监听**（RegisterShellWatch :523，Win only）：desktop pidl 递归注册 `SHChangeNotifyRegister`，订阅 `SHCNE_RENAMEITEM | SHCNE_RENAMEFOLDER | SHCNE_DELETE`；消息泵（ShellWatchWindow::nativeEvent）实现正确。`HandleRename` 精确路径匹配 Relink + ads 兜底（RelinkLostByAds）；`HandleRenameFolder` 前缀批量 Relink；`HandleDeleted` 限流 5s 触发救援。
  2. **自动救援**（RelocateMissingEntries :258）：扫描 `Recent(kRelocateScanLimit=2000)` 内丢失记录；roots = RelocateCandidates（**AppendCandidate 有 SameDrive 硬门**，只认旧路径所在盘）+ DriveRoot(旧路径)；有界扫描（20000 文件 / 深度 4 / 10s）。触发点：首次保存/下载事件、下载历史窗口打开 ×2。
  3. **手动**：逐条"选文件回填"（lng_sexcat_relocate :745）+ 逐条"选目录按文件名+大小扫描"（relocateByDirectory :610 → LocateByNameSize，同 20000/深度4/10s 预算）。
- **失败面推断**：跨盘移动（Explorer 跨盘 = create+delete，DELETE 有订阅、CREATE 无 → 新盘文件 ads 永不被读；救援 SameDrive 硬门挡住新盘）→ 自动链路必然失效，只剩逐条手动；同盘移动理论上 rename 事件应命中（若用户同盘也失败则存在未定位 bug 或移动方式差异）。丢失扫描窗口仅最近 2000 条，更老记录无救援。
- **候选修复面**：(a) 订阅 SHCNE_CREATE + ads 匹配回链（跨盘落盘即自动回链，代价是全系统文件创建事件做一次轻量探测）；(b) 救援扫描扩全部固定盘（有界）；(c) 历史窗口批量找回（选目录一次回链全部丢失记录）；(d) 丢失扫描窗口扩容。
- **用户裁决（当轮）**：失败面 = 仅跨盘移动（同盘监听工作正常）；修复形态 = 全套（自动 + 批量手动兜底）；回链时机 = 实时。
- **定稿改动清单**：
  - **C. 跨盘自动回链**：C1 RegisterShellWatch 事件掩码 +`SHCNE_CREATE`；C2 nativeEvent CREATE 分支读 pidls[0]；C3 ProcessChangeEvent → `HandleCreated(path)`（空/已跟踪跳过 → ReadAds 命中 → 现成 RelinkLostByAds 只回链丢失记录）；C4 RelocateMissingEntries roots 追加全部 `QDir::drives()` 盘根（排在旧盘 roots 之后，预算耗尽自然跳过）。
  - **D. 批量找回**：D1 shell_watch 导出 `RelocateLostInFolder(root)`（复用 MissingIndex 构建 + ScanForMissing + RelinkMovedEntry，返回回链数）；D2 历史窗口 lost 行右键新项 `lng_sexcat_relocate_lost`="Relocate all lost files in folder..." → 选目录 → async 回链 → toast 计数。
  - **边界（如实）**：丢失窗口 2000 条不扩（范围外）；CREATE 探测 = 全系统创建事件一次轻量 ads 读（拍板实时）；"复制"（非移动）不回链（新副本走既有新增记录语义）。
- **已实施（commit `18f9972b2c`，未 push）**：
  - C 落码：事件掩码 +SHCNE_CREATE；nativeEvent DELETE 分支合并 CREATE 读 pidls[0]；`HandleCreated`（空/目录/已跟踪跳过 → ReadAds 命中 → 现成 RelinkLostByAds，WHY 两行）；RelocateMissingEntries roots 追加全部 `QDir::drives()` 盘根（排在旧盘候选后，预算耗尽自然跳过）。Guard 边界核验：HandleCreated/ProcessChangeEvent 落在 Q_OS_WIN 区（:375-605）内，RelinkLostByAds 同区可见；RelocateLostInFolder 在跨平台区（:329）且只依赖跨平台符号（CollectMissingIndex/ScanForMissing/RelinkMovedEntry）。
  - D 落码：导出 `RelocateLostInFolder(root)` 返回回链数；抽出 `CollectMissingIndex()`（anon ns）消除 RelocateMissingEntries 的 missing+index 双重构建，两处复用；历史窗口 lost 行右键新项 `lng_sexcat_relocate_lost`="Relocate all lost files in folder..." → relocateLostInFolder()（anyLost 预检 → 选目录 → crl::async 回链 → reload + "Relocated %1 files" toast，QPointer 模式与现有一致）。
  - 注释预算：1 常规（"cross-volume moves land on other drives; scan them last."）+ 1 WHY 例外（HandleCreated 两行）✓。
  - 静态验证全绿：`git diff --check` 干净；SHCNE_CREATE 三处（掩码/nativeEvent/分支）+ HandleCreated/RelocateLostInFolder/CollectMissingIndex 声明↔定义↔调用三方一致；lng 键双侧接线；HistoryWindow/reload()/_rows 成员与 QPointer 模式沿用现形。

## §30 Grill QQQQQQ50 + Q34：取消入口分裂 / Open 回存旧路径 —— 调查完成，待裁决
- **QQQQQQ50 事实**：全局取消入口有两处，均为 ayu 加、都调 `DownloadManager::cancelAllLoading()`（= `loadingStop(nullptr)` 全局杀）：①主窗口下载条右键菜单（dialogs_widget.cpp:1695，Q50 轮加，`menuRequests` 注释 "Q50: one-click cancel for everything still downloading"）；②下载页顶栏菜单（info_downloads_widget.cpp:110）。原生 DownloadManager 按 peer 分组（:880），无按"这条消息"维度的取消 API。批量下载 toast 指向下载页顶栏。用户诉求 = 一键取消这条消息的全部下载；痛点 = 现在有两个取消地方且都是全局语义。
- **Q34 事实**："Sow list" = sexcat 下载历史窗口（HistoryWindow）；Refresh 按钮 = `RelocateMissingEntries() + reload()`（用户旧构建仅同盘救援）。窗口内 openFolder 对丢失行只 toast（不回存）——"文件又回来了"不在本窗口产生。推断来源：**原生下载页每行右侧 Open 按钮**（文件缺失时原生行为 = 重新下载到原路径），重下走 sexcat save hook → 旧路径生成重复记录，移动后的文件仍在另一盘。用户诉求 = Open 定位到移动后的新位置，不复制回旧路径。未上 `18f9972b2c` 构建前：跨盘 CREATE 回链不存在，下载页 Open 必然重下。
- **待裁决**：①取消入口收敛成哪个（合并两处为一个全局 or 按批次/来源分组取消）；②Open 的期望行为（自动定位新位置 / 明确丢失提示不重下 / 接受重下但防重复记录）；③Open 拦截面（原生下载页 or sexcat 历史窗口 or 两者）。
- **用户裁决（当轮）**：Q34 纠正 = 无重复记录 + 秒回 ⇒ 不是网络重下，需深挖其他原因，且非 100% 复现；Open 定位策略 = 只用已回链结果（定位不到立即提示丢失）；QQQQQQ50 = 清理后期加的取消口 + 新增一条消息的总取消口。
- **深挖结论（机制钉死）**：`SessionController::openDocument`（window_session_controller.cpp:3452）聊天点击/下载页行点击（经 Overview delegate → 同一汇合点）都走到：`document->filepath()`（存在性校验，文件被移走后为空）→ `DocumentSaveClickHandler::Save(mode ToFile)` → `data->saveFromData(contextId)` → `filepath(true)` 校验仍失败 → `saveFromDataChecked(contextId)`：**从媒体缓存（activeMediaView()->bytes()）把字节瞬间写回 `DocumentFileNameForSave(this)` = 旧保存路径**。全程无网络（解释"秒回"）、不产生新记录（写入不经 sexcat 通知链，解释"无重复记录"）、依赖 activeMediaView 有缓存字节（解释"非 100% 复现"——缓存被驱逐后 bytes 空 → 不复活）。
- **定稿改动清单**：
  - **A. QQQQQQ50 取消口收敛**：A1 删 dialogs_widget.cpp:1695-1710 下载条右键取消菜单块（后期加的那个）；A2 DownloadManager 新增 `loadingInProgressForItem(FullMsgId)` + `cancelLoadingForItem(FullMsgId)`（遍历 _sessions.downloading，匹配 `i->object.item->fullId()`，私有 cancel(data,i) 同步移除元素 → while 重启模式，先例 cancelAllLoading/clearLoading）；A3 消息右键菜单（下载相册入口旁）+ lang `ayu_CancelMessageDownloading`="Cancel downloads of this message"，loadingInProgressForItem 为真才显示。
  - **B. Q34 Open 定位**：B1 DB `LookupBySource(int64 peerId, int64 msgId)` → 候选 entries（sqlite_orm get_all where src_peer_id+src_msg_id）；B2 save_hook 导出 `std::optional<QString> ResolveRelocatedDocument(document, contextId)`：LookupBySource → 存在性过滤（回链后的新路径唯一存活）→ src_attach（内部 ResolveAttachIndex 直用，无需导出）→ file_size/fileName 精确化；三态 = nullopt 未跟踪 / "" 已跟踪但丢失 / 路径；B3 openDocument 的 `filepath.isEmpty()` 分支前插拦截：nullopt 落穿（未跟踪文件行为不变）/ "" → `lng_sexcat_lost` toast + return（不复活）/ 路径 → `File::Launch` + return（openDocument 已 include file_utilities ✓）。
  - **边界（如实）**：媒体查看器 overlay 的 open 路径（:3393-3406 File::Launch(existing)）不在本轮拦截面；未上 `18f9972b2c` 的构建回链不存在时 B3 语义 = 提示丢失（已回链结果依赖自动回链先发生）。

## §31 Grill Q35：下载历史按消息分组 + 组级清理 —— 调查完成，待裁决
- **用户需求原话（编号保留）**：方便清理不需要的下载历史记录（saved by source 同步清理）；0. 同一条消息的文件一组显示，显示设计委托我来定；1. 可折叠、可全选、可取消全选；2. 两种口径：只删除下载历史记录 / 删除记录和删除源文件（括注"不是删除是单独折叠"＝折叠本身只是显示行为，与删除无关）。
- **现状事实**：DB 无记录删除 API（仅 DeleteTag；需新建 DeleteRecords，事务内 SavedFileRow + SourceTagRow 同步清，孤儿 tag 经 TagSummaries join 聚合自动消失 = "saved by source 同步清理"的 DB 面）；HistoryWindow 树 = QTreeWidget + ExtendedSelection，已有可折叠大组先例（lost 组，默认折叠，fillItem 挂 UserRole=row.id）；筛选 combo = all + 每来源一行（rebuildFilterBox）；行无 checkbox；上下文菜单无删除项。QFile::moveToTrash 可用（Qt6 静态方法）。删文件需同步处理 .sexcat.json 侧车（RelinkMovedEntry 已有搬运先例）。
- **设计面（委托已接）**：按 (src_peer_id, src_msg_id) 分组；组头 = srcTitle + 文件数 + 总大小 + 日期范围；无来源记录（folder-scan 导入 peerId==0）归入 "No source" 组；组头交互 + 删除入口形态 + lost 行去处 = 待裁决；删除确认框（数量/大小/是否含源文件）纳入实现。
- **待裁决**：①删除入口形态（组头右键菜单 / 组头小按钮 / 勾选+统一删除）；②源文件删除方式（永久 / 回收站 / 询问）；③lost 行去处（并入消息组 vs 保留底部大组）。
- **用户裁决（当轮）**：①组头交互集中（左键全选/取消全选切换，右键菜单两删除口径）；②删源文件每次询问（确认框内选回收站/永久）；③lost 行并入各自消息组（lost 样式标记），底部 lost 大组取消。
- **定稿改动清单**：
  - **F. 分组显示重构（HistoryWindow）**：F1 applyFilter 后按 (srcPeerId, srcMsgId) 分组，组头 = QTreeWidgetItem（UserRole 组标记）显示 srcTitle + "N files · 总大小 · 日期范围"，无来源归 "No source" 组，lost 行留在组内沿用 lost 样式，默认展开；F2 组头左键 toggle 全选/取消全选子行（selectionModel，复用现有 ExtendedSelection 与 verifySelected 通路），组头右键菜单 = "Delete group records" / "Delete group records and files"；行级上下文菜单对称补两项；底部 lost 大组代码移除；F3 确认框 = QMessageBox（窗口纯 Qt 风格一致）列出条数+总大小，含源文件口径时再选 moveToTrash/remove；F4 导出 `DeleteRecords(ids)`（DB 事务删 saved_file+source_tag 行）+ `DeleteRecordFile(entry, toTrash)`（文件+侧车，先删文件再删行，失败可中断）；F5 删除后 reload + rebuildFilterBox（来源 combo 重建 = saved by source 同步清理 UI 面）+ toast 计数。
  - 注释预算照旧 2 常规 + 1 WHY 例外；手测口径 = 组折叠/全选切换、组级两口径删除（含回收站可恢复验证）、孤儿 tag 从 sow 列表消失、来源 combo 同步、lost 行组内显示。
- **已实施（commit `9264c57b44`，未 push）**：
  - F1 落码：applyFilter 改分组输出（std::map<(int64 peer, int64 msg)>，peer id 显式 int64 防窄化）；`fillGroupItem` = 组头（UserRole 0 哨兵）Time=最新行日期 / Name=srcTitle（空→"No source"）/ Source="N · 大小 · [M lost]"；组默认展开；底部 lost 大组代码移除，lost 行留组内沿用 lost 前景样式。
  - F2 落码：itemClicked 组头分支 → `toggleGroupSelection`（QItemSelection 全子行 + Select/Deselect | Rows 标志）；组头右键菜单（select-all + 两删除口径）；行级右键补 "Delete record" / "Delete record and file"。
  - F3/F4 落码：`deleteRows(ids, withFiles)` —— 仅记录 = QMessageBox::question Yes/Cancel；含文件 = 三按钮（回收站 ActionRole / 永久 DestructiveRole / Cancel）；先 DeleteRecordFile 后 DB.DeleteRecords（WHY 注释：失败留记录可重试）；DB `DeleteRecords` 事务删 saved_file+source_tag；顺手补齐 Q81 `DropRecordById` 的 source_tag 清理（同步清理要求）；shell_watch 导出 `DeleteRecordFile(entry, toTrash)`（moveToTrash/remove + 侧车清理，丢失行如实成功）。
  - F5 落码：toast 双键（records / records+files，如实报告文件删除成功数）；reload 内已含 rebuildFilterBox（来源 combo 重建）。
  - 计划外：抽 `EntryFromRow(row)` helper，finishDirectoryRelocation 的 Entry 字面量同步复用。
  - 注释预算实况：database.h 1 常规（Q35）+ shell_watch.h 1 常规 + deleteRows 1 WHY = 2 常规 + 1 例外 ✓。
  - 静态验证全绿：`git diff --check` 干净；10 个新 lang 键 lang↔code 双侧接线（每键精确 grep）；新符号三方一致；EntryFromRow 字段序沿用现成编译面。
- **编译审计（f46cd36..HEAD）与阻断修复（commit `e765684bed`，已 push）**：审计 0 编译错之外的 2 阻断已清——①save_hook.cpp:806 OriginFromItem 漏迁移 QString→QStringList（C2664）改传 `{}`；②4 个非复数 lang 键用保留占位符 `{count}`（lang_values.h 把 count 标签无条件路由进复数流水线）改 `{count1}` + 调用点 `lt_count1`。非阻断挂账（后续轮次清）：RetagSource 缺 rollback / BackfillLegacySourceTags 无事务 / DropRecordByPath 缺 source_tag 清理 / 两处 [[nodiscard]] 丢弃 / 组头 "No source"/"lost" 未本地化 / menu include 块位置。
- **偶发启动失败诊断（用户报 G:/AyuGram_7.2.9 log，未动码）**：上游单实例×日志轮转竞态——旧实例退出窗口内连发多个新进程，一个次实例还握着 log_start0.txt 句柄（5×1s 应答等待），另一进程因旧实例死透被判为主实例，改名失败 → NotStartedWindow。良性（无崩溃无 tdata 风险，重试即开）。修复选项挂账：(b) instanceChecked 失败回落继续用 start 日志（推荐，listen 成功即为主实例）；(a) 改名有界重试。
