# 交付：QQQQ10 丢失记录（灰行）重定位路由修复（H1）

## 症状

A. 手动把下载文件移到其他文件夹后，窗口 Current path 列不更新（行灰显、路径停留旧值）；
B. 手动改名后行变灰，File name 不更新为新名字。

## 根因

- 灰行判定：`row.lost = !QFileInfo::exists(savedPath)`（sexcat_history_window.cpp:352）——手动移动/改名后旧路径失效，灰显本身正确。
- **死路（B）**：`relocateRow` 对 lost 行强制走 `relocateByDirectory`（原 :572-575），而目录扫描按 `LocateByNameSize(dir, 旧文件名, size)` 匹配（:644）——改名文件永远找不到；唯一与名字无关的通道（文件选择器 + sha256 校验，`finishRelocation` :596-624）被路由挡死。
- **静默失败（A）**：自动追踪 `TryAutoRelocate`（sexcat_shell_watch.cpp:193-229）只扫固定候选根（桌面/下载/文档/旧目录/其他收藏所在目录），失败仅写日志不提示（:226-228）。
- 用户回执：移动/改名全部在资源管理器手动做，右键菜单从未用过——救援通道存在但用户无从得知。

## 改动

- `sexcat_history_window.cpp` `relocateRow`：撤掉 lost 行强制目录扫描路由，灰行可直接用文件选择器重定位——sha256 校验兜底（选错文件弹"Hash mismatch"，无害），哈希匹配则 `Database::Relink` 原地更新 path+file_name（sexcat_database.cpp:399-427）→ `_newSaved` → 窗口 `reload()`，两列当场刷新并落库。
- `showContextMenu`：灰行新增 "Relocate by folder scan..."（`relocateByDirectory`）第二入口；目录扫描保留（只移动未改名时按名字+大小批量找回更方便）。
- `lang.strings`：新增 `lng_sexcat_relocate_dir`。

## 外部构建机手测清单

1. 手动改名某个已收藏文件 → 窗口该行灰显 → 右键 "Relocate..." 选中新名字的文件 → 行恢复颜色，File name 显示新名字，Current path 同步。
2. 手动移动（不改名）→ 灰行 → 右键 "Relocate..." 直接选中新位置的文件 → 恢复；或右键 "Relocate by folder scan..." 选新文件夹 → 按名字+大小找到并恢复。
3. 右键 "Relocate..." 选一个错误文件 → 提示 "Hash mismatch"，行状态不变（防错选）。
4. 重启应用 → 恢复过的行保持正确路径/名字（落库）。
5. 正常未丢失行 → 右键菜单与原先一致（"Relocate..." 行为不变）。

## H2：自动救援（零手动）

### 缺口

shell watch 的 Windows 通知只注册了桌面文件夹/下载/文档三个子树（:470-511），用户自己的收藏树不在监听内 → 移动/改名无事件；窗口"刷新"按钮的救援扫描按**旧名字**匹配（改名盲）且候选根有限、失败仅写日志。结果：一切救援都要用户手动右键。

### 改动

- `sexcat_shell_watch.cpp` `RelocateMissingEntries` 重写为**单次批量遍历**：收集全部灰行建索引（size → entries），后台线程按优先级根（旧目录 → 桌面/下载/文档/其他收藏所在目录 → **丢失文件所在盘符根**）一次走完，命中判定三级：**同大小 → 同名**（免费）→ **同大小 → ADS 标记**（同卷移动标记随文件走，微秒级）→ **同大小 → 全量哈希**（≤512MB 上限防大文件卡死）——改名/移动/改名+移动通吃，一次遍历服务所有灰行。
- 主线程只收结果做 Relink（`crl::on_main`），不再卡 UI；成功后 toast "Auto-relocated: {count}"（新键 `lng_sexcat_autorelocate_done`），并经 Relink 落库。
- `RelocateMissingEntries` 挂上**窗口构造**（原先只在"刷新"按钮）：打开窗口即自动救援，灰行数秒内自愈，零手动。
- 哈希助手迁入 `HashStore::ComputeFileSha256`（hash_store.h/.cpp），history_window 三处调用点改用，删除本地副本。

### 手测补充

6. 手动改名/移动若干文件 → 打开 sexcat 窗口 → 数秒内灰行自动恢复 + toast "Auto-relocated: N"。
7. 跨盘移动（C: → D:）→ 打开窗口自动找回（走盘符根遍历兜底）。
8. 恢复过的行重启后仍正确（落库）。

### H4：启动时批量救援（USN 提权阻塞的等价替代）

- SDK 事实：读 NTFS USN 变更日志需 `CreateFile(L"\\\\.\\C:", GENERIC_READ)` 卷句柄，非提权进程 `ERROR_ACCESS_DENIED`（Everything 全盘搜索必须装系统服务即此原因）；Telegram 不提权运行 → USN 层只会是空转死代码，不落。
- 等价保证：`RelocateMissingEntries()`（异步、有界预算）挂进 sexcat 模块启动序列（save_hook :1045 StartShellWatch 之后）——**应用关闭期间**发生的移动/改名，下次启动自动找回，不再依赖用户开窗口。
- USN 仍是唯一能精确回放历史路径的 API；若未来确有提权运行场景再议。

### H3：全局 shell 事件监听（用户拍板）

- 注册面换成 `SHGetFolderLocation(CSIDL_DESKTOP)` 命名空间根 + 递归 = **全盘**改名/移动/删除事件（原先只有桌面/下载/文档三子树，用户收藏树收不到任何事件）。
- Ctrl+X 剪切粘贴在同一卷上就是 SHCNE_RENAMEITEM → HandleRename 即时精确重链；跨卷移动 = 源目录 DELETE → 5 秒冷却触发批量救援（三级匹配 + 盘符根兜底）。
- 裁掉 SHCNE_UPDATEITEM：全局范围下每个文件内容变化都触发会成 IO 风暴；其职责（按标记找回）由改名兜底 + 批量救援覆盖。
- rename miss 日志静音；HandleUpdate/TryAutoRelocate/RelocateInFlight 删除（-102 行）。

### 手测补充

9. 应用开着，在**任意盘任意文件夹**改名/移动/Ctrl+X 剪切已收藏文件 → 无需任何操作，行即时恢复（同卷）；跨卷移动后打开窗口即自动恢复 + toast。
10. 全盘监听下日常用机（浏览器/编译器/临时文件）不应有卡顿或日志刷屏（miss 静音 + 冷却）。
