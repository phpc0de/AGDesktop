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
