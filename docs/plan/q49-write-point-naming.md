# Q49/Q50 写盘点改名 + 相圈子集多选 实现计划

> 面向执行者：按任务顺序逐条落地，每步都是精确改动。验证基准 = ADR-0004（本机零构建：codegraph+grep 交叉核验 + 外部构建机编译实测），非 TDD 红绿流程。
>
> **Goal:** 下载文件的增补命名在写盘时一次成型（不再依赖事后 rename），并给相册消息加"选择视频下载"子菜单。
>
> **Architecture:** 改名钩子从"链路节点"下沉到"写盘点"——`DocumentData::saveFromDataChecked` / `Save()` 目标生成 / 另存对话框默认名统一走 `SexCat::BuildCaptionedNameFor`；事后 `MaybeAugmentedSavePath` 降级为兜底。子集多选复用 `menu_item_download_files.cpp` 的批量管线（`RunBatchSave`），入口照抄 `sexcat_save_hook.cpp:1093-1104` 的仓内子菜单先例。
>
> **Tech Stack:** C++20 / Qt6 / tdesktop 7.x fork API。子模块 `Telegram/lib_ui` 不在盘上，菜单 API 只能用仓内既有用例（sexcat 子菜单、`FormatSizeText`）作编译地面真相。
>
> **Spec:** `docs/adr/0006-sexcat-naming-batch-download.md`（Amendment 1）+ `CONTEXT.md §18`。

## Global Constraints

- AGENTS.md：`/grill` 已解冻，本轮允许改应用代码；仍**禁止本机构建**（无工具链/子模块，ADR-0004）。
- 注释配额：一句；WHY 块最多三句，每次提交最多一个。
- `u"..."_q` 字面量；禁止 `Q_OS_LINUX`；`[[nodiscard]]` 不得 `(void)` 丢弃（用返回值）。
- AyuGram 对上游文件的 include 走独立块（现有块内追加）。
- 不改二进制序列化字段顺序；`lang.strings` 只增键不删键。
- 触发门槛不变：文件名主干全数字 + 组级 caption/tag 非空；240 上限截断保原名。

---

### Task 1: sexcat 钩子核心（`sexcat_save_hook.h` / `.cpp`）

**Files:**
- Modify: `Telegram/SourceFiles/ayu/sexcat/sexcat_save_hook.h`
- Modify: `Telegram/SourceFiles/ayu/sexcat/sexcat_save_hook.cpp`

**Interfaces:**
- Produces（供 Task 2/4 使用）：
  - `QString SexCat::AugmentedDocumentName(not_null<DocumentData*>, FullMsgId = {})`
  - `QString SexCat::AdjustDocumentSaveTarget(const QString&, not_null<DocumentData*>, FullMsgId)`（签名不变，行为新增 PendingMap 兜底）
  - `void SexCat::NotifyDuplicateSkipped(const QString &path)`
  - 头文件新增注释块与三个声明。

- [ ] **Step 1: 头文件加声明**

在 `AdjustDocumentSaveTarget` 声明之前插入：

```cpp
// Q49: augmented "#tags -description-original" name, or empty when the
// naming rules don't apply. The source item comes from contextId or the
// latest save-as marker of this document.
[[nodiscard]] QString AugmentedDocumentName(
	not_null<DocumentData*> document,
	FullMsgId contextId = {});
```

在 `AdjustDocumentSaveTarget` 声明之后、`AddSowTagMenuActions` 之前插入：

```cpp
// Toast when the dedup gate skipped a download the library already has.
void NotifyDuplicateSkipped(const QString &path);
```

- [ ] **Step 2: 删 TTL 剪除（HC 修复）**

`sexcat_save_hook.cpp`：
1. `PendingSaveAs` 删 `crl::time when = 0;` 字段。
2. 删除 `kPendingSaveAsTtl` 常量与整个 `PruneStalePending()` 函数，替换为一条 WHY 注释（`kPendingSaveAsMax` 保留）：

```cpp
// WHY: 条目只在下载完成（NotifyDocumentSaved）或失败（Forget 回调）
// 时消失；加 TTL 会让长下载丢了改名上下文，正是 Q49 复发的来源。
```

3. 删除全部 `PruneStalePending();` 调用（现状 4 处：`FindRecordedForDocument`、`ShouldSkipDuplicateDownload`、`MarkDocumentSaveAs`、`NotifyDocumentSaved`）。
4. `MarkDocumentSaveAs` 中 `map[document.get()] = PendingSaveAs{ contextId, sourceTag, crl::now() };` 改为 `PendingSaveAs{ contextId, sourceTag }`。

- [ ] **Step 3: 统一构名 + 写盘点函数**

替换现存 `AdjustDocumentSaveTarget`（原 :1032-1059）为两层：

```cpp
// Shared captioned-name builder; empty when the rules don't apply.
[[nodiscard]] QString BuildCaptionedNameFor(
		not_null<DocumentData*> document,
		const QString &original,
		FullMsgId contextId) {
	const auto dot = original.lastIndexOf(u'.');
	const auto stem = (dot > 0) ? original.left(dot) : original;
	if (!IsAllDigitsName(stem)) {
		return {};
	}
	auto item = contextId
		? document->owner().message(contextId)
		: nullptr;
	if (!item || !item->isRegular()) {
		const auto i = PendingMap().find(document.get());
		if (i != end(PendingMap())) {
			item = document->owner().message(i->second.contextId);
		}
	}
	if (!Enabled()) {
		LogFailure(u"augment %1 skipped: disabled"_q.arg(original));
		return {};
	}
	if (!item || !item->isRegular()) {
		LogFailure(u"augment %1 skipped: no source item"_q.arg(original));
		return {};
	}
	const auto candidate = BuildAugmentedName(
		original,
		AugmentNameParts(item));
	if (candidate.isEmpty()) {
		LogFailure(u"augment %1 skipped: no caption text"_q.arg(original));
	}
	return candidate;
}

QString AugmentedDocumentName(
		not_null<DocumentData*> document,
		FullMsgId contextId) {
	return BuildCaptionedNameFor(document, document->filename(), contextId);
}

QString AdjustDocumentSaveTarget(
		const QString &targetPath,
		not_null<DocumentData*> document,
		FullMsgId contextId) {
	if (targetPath.isEmpty()) {
		return targetPath;
	}
	const auto info = QFileInfo(targetPath);
	const auto original = info.fileName();
	const auto candidate = BuildCaptionedNameFor(document, original, contextId);
	if (candidate.isEmpty()) {
		return targetPath;
	}
	const auto target = UniqueAugmentedTarget(info, candidate);
	LogFailure(u"save target augmented: %1 -> %2"_q.arg(original, target));
	return target;
}
```

注意：`BuildCaptionedNameFor` 放在 `BuildAugmentedName`/`UniqueAugmentedTarget` 之后、`MaybeAugmentedSavePath` 之前的命名区；`MaybeAugmentedSavePath` 本身不动（降级为兜底 rename）。

- [ ] **Step 4: 去重跳过 toast（HA 用）**

在 `MaybeAugmentedSavePath` 之前的公共区定义（`ShowToastText` 之后即可）：

```cpp
void NotifyDuplicateSkipped(const QString &path) {
	LogFailure(u"duplicate download skipped: "_q + path);
	if (!Enabled()) {
		return;
	}
	ShowToastText(tr::lng_sexcat_already_saved(tr::now, lt_path, path));
}
```

- [ ] **Step 5: 静态验证**

```bash
grep -n "PruneStalePending\|kPendingSaveAsTtl" Telegram/SourceFiles/ayu/sexcat/sexcat_save_hook.cpp   # 期望：0 命中
grep -n "AugmentedDocumentName\|AdjustDocumentSaveTarget\|NotifyDuplicateSkipped" Telegram/SourceFiles/ayu/sexcat/sexcat_save_hook.h
```

---

### Task 2: data 层写盘点（`data_document.*` + `data_file_click_handler.cpp`）

**Files:**
- Modify: `Telegram/SourceFiles/data/data_document.h`
- Modify: `Telegram/SourceFiles/data/data_document.cpp`
- Modify: `Telegram/SourceFiles/data/data_file_click_handler.cpp`

**Interfaces:**
- Consumes: Task 1 的 `AugmentedDocumentName` / `AdjustDocumentSaveTarget` / `NotifyDuplicateSkipped`。
- Produces：`bool DocumentData::saveFromData(FullMsgId = {})`、`bool DocumentData::saveFromDataSilent(FullMsgId = {})`（默认参保证旧调用点零改动）。

- [ ] **Step 1: 头文件签名**

`data_document.h`：

```cpp
bool saveFromData(FullMsgId contextId = {});
bool saveFromDataSilent(FullMsgId contextId = {});
```

同文件 `saveFromDataChecked` 声明补 `FullMsgId contextId` 参数（`grep -n "saveFromDataChecked" data_document.h` 定位；实现处同步，见 Step 3）。

- [ ] **Step 2: `data_document.cpp` 三处实现**

```cpp
bool DocumentData::saveFromData(FullMsgId contextId) {
	return !filepath(true).isEmpty() || saveFromDataChecked(contextId);
}

bool DocumentData::saveFromDataSilent(FullMsgId contextId) {
	return !filepath(true).isEmpty()
		|| (Core::App().canSaveFileWithoutAskingForPath()
			&& saveFromDataChecked(contextId));
}

bool DocumentData::saveFromDataChecked(FullMsgId contextId) {
	if (forbidsFileSave()) {
		return false;
	}
	const auto media = activeMediaView();
	if (!media) {
		return false;
	}
	const auto bytes = media->bytes();
	if (bytes.isEmpty()) {
		return false;
	}
	// Q49: the cached bytes are written to the final captioned name here.
	const auto rawPath = DocumentFileNameForSave(this);
	if (rawPath.isEmpty()) {
		return false;
	}
	const auto path = SexCat::AdjustDocumentSaveTarget(
		rawPath,
		this,
		contextId);
	auto file = QFile(path);
	if (!file.open(QIODevice::WriteOnly)
		|| file.write(bytes) != bytes.size()) {
		return false;
	}
	file.close();
	_location = Core::FileLocation(path);
	session().local().writeFileLocation(mediaKey(), _location);
	return true;
}
```

（`rawPath` 保留给 AdjustDocumentSaveTarget 前的判空，语义与旧 `path.isEmpty()` 一致。）

- [ ] **Step 3: 去重虚指修复（HA）**

`data_document.cpp` 去重分支（现 :1330-1336）替换为：

```cpp
	if (!toFile.isEmpty()
		&& SexCatShouldSkipDuplicate(this, toFile)) {
		if (QFileInfo::exists(toFile)) {
			setLocation(Core::FileLocation(toFile));
			session().local().writeFileLocation(
				mediaKey(),
				Core::FileLocation(toFile));
			owner().requestDocumentViewRepaint(this);
		}
		SexCat::NotifyDuplicateSkipped(toFile);
		return;
	}
```

- [ ] **Step 4: `data_file_click_handler.cpp` 写盘点注入**

在 `:106` 的 `explicitSaveAs` 定义后、`saveFromData` 调用处改三处：

```cpp
	auto already = filename;
	if (mode == Mode::ToNewFile && already.isEmpty()) {
		// Q49: the save dialog opens with the captioned default name.
		already = SexCat::AugmentedDocumentName(data, contextId);
	}
	if (mode != Mode::ToNewFile && data->saveFromData(contextId)) {
		...
	}
	auto savename = DocumentFileNameForSave(
		data,
		(mode == Mode::ToNewFile),
		already,
		filedir);
```

并把 `:140` 的守卫：

```cpp
		if (explicitSaveAs && !data->loading()) {
```

改为：

```cpp
		if (explicitSaveAs) {
			// WHY: loading state must not eat the marker; the write-point
			// name and the fallback rename both key off it.
```

- [ ] **Step 5: 链路诊断日志（R5 第 1/2 点）**

在 `explicitSaveAs` 计算完成后加一行：

```cpp
	Ayu::Log::Write(
		u"sexcat"_q,
		u"save: mode=%1 explicit=%2 local=%3"_q.arg(
			int(mode)).arg(explicitSaveAs).arg(data->filepath(true).size()));
```

文件 AyuGram include 块补 `#include "ayu/utils/ayu_log.h"`。

- [ ] **Step 6: 静态验证**

```bash
grep -n "saveFromData(" Telegram/SourceFiles --include=*.cpp -r          # 仅 click_handler:109 带 contextId，其余默认参
grep -n "explicitSaveAs && !data->loading()" Telegram/SourceFiles -r     # 期望 0 命中
grep -n "NotifyDuplicateSkipped" Telegram/SourceFiles -r
```

---

### Task 3: 静默落盘调用点（GIF/图片扩面）

**Files:**
- Modify: `Telegram/SourceFiles/media/view/media_view_overlay_widget.cpp:4812`
- Modify: `Telegram/SourceFiles/data/data_document_resolver.cpp:310`

- [ ] **Step 1: overlay 静态内容分支**

`_document->saveFromDataSilent();` → `_document->saveFromDataSilent(_message ? _message->fullId() : FullMsgId());`（同文件 `:3547` 已有同款判空写法）。

- [ ] **Step 2: resolver**

`data_document_resolver.cpp:310` 的 `document->saveFromDataSilent();` → `document->saveFromDataSilent(item ? item->fullId() : FullMsgId());`（先核对该作用域 `item` 可空性；若为 `not_null` 则直接 `item->fullId()`）。

- [ ] **Step 3: 其余调用点不动**

`history_view_draw_to_reply.cpp:39`、`iv_cached_media.cpp:693` 默认参走 PendingMap 兜底，保持零改动。

- [ ] **Step 4: 静态验证**

```bash
grep -rn "saveFromDataSilent(" Telegram/SourceFiles --include=*.cpp
```

---

### Task 4: Q50 相圈子集多选（`menu_item_download_files.cpp`）

**Files:**
- Modify: `Telegram/SourceFiles/menu/menu_item_download_files.cpp`

**Interfaces:**
- Consumes: 现有 `Documents/Photos`（`std::vector<std::pair<not_null<DocumentData*>, FullMsgId>>`）与 Task 1/2 的写盘点（透明生效）。
- Produces：匿名命名空间内 `RunBatchSave` + `AddDownloadVideosSelector`。

- [ ] **Step 1: 抽出批量管线**

把 `AddAction` 内 `menu->addAction(text, [=] { ... }, icon)` 的整个 lambda 体（含 batchTag 计算、saveImages、saveDocuments、文件夹对话框/快速保存分流）抽成文件顶部新函数：

```cpp
void RunBatchSave(
		base::weak_ptr<Window::SessionController> weak,
		const Documents &documents,
		const Photos &photos,
		bool forceFolderDialog,
		Fn<void()> callback) {
	const auto controller = weak.get();
	if (!controller) {
		return;
	}
	[原 lambda 体逐字搬入：batchTag 扫描 → saveImages → saveDocuments → folder 对话框分支]
}
```

`AddAction` 尾部改成：

```cpp
	menu->addAction(text, [=] {
		RunBatchSave(weak, documents, photos, forceFolderDialog, callback);
	}, icon);
```

（`weak` 原变量名与 `AddAction` 内 `const auto weak = base::make_weak(controller);` 保持。）

- [ ] **Step 2: 子集选择子菜单**

在匿名命名空间加（照抄 sexcat 钩子 :1093-1104 的子菜单写法）：

```cpp
void AddDownloadVideosSelector(
		not_null<Ui::PopupMenu*> menu,
		not_null<Window::SessionController*> window,
		const Documents &documents) {
	if (documents.size() < 2) {
		return;
	}
	const auto weak = base::make_weak(window);
	const auto state = std::make_shared<std::vector<bool>>(
		documents.size(),
		false);
	const auto anchor = menu->addAction(
		base::make_unique_q<Ui::Menu::Action>(
			menu->menu(),
			menu->menu()->st(),
			Ui::Menu::CreateAction(
				menu->menu().get(),
				tr::ayu_SelectVideosToDownload(tr::now),
				nullptr),
			&st::menuIconDownload,
			&st::menuIconDownload));
	anchor->setMenu(new QMenu(menu->menu().get()));
	const auto sub = menu->ensureSubmenu(anchor, st::popupMenuWithIcons);
	const auto showCount = [=](int count) {
		Ui::Toast::Show(Ui::Toast::Config{
			.text = TextWithEntities{
				tr::lng_sexcat_videos_selected(tr::now, lt_count, count),
			},
		});
	};
	for (auto i = 0; i < int(documents.size()); i++) {
		const auto index = i;
		const auto label = u"%1 · %2"_q.arg(i + 1).arg(
			FormatSizeText(documents[i].first->size));
		sub->addAction(label, [=] {
			auto &on = (*state)[index];
			on = !on;
			auto count = 0;
			for (const auto value : *state) {
				count += (value ? 1 : 0);
			}
			showCount(count);
		});
	}
	sub->addAction(
		tr::ayu_DownloadSelectedVideos(tr::now),
		[=] {
			auto picked = Documents();
			for (auto i = 0; i < int(documents.size()); i++) {
				if ((*state)[i]) {
					picked.push_back(documents[i]);
				}
			}
			if (picked.empty()) {
				Ui::Toast::Show(Ui::Toast::Config{
					.text = TextWithEntities{
						tr::lng_sexcat_nothing_selected(tr::now),
					},
				});
				return;
			}
			RunBatchSave(weak, picked, {}, false, [] {});
		},
		&st::menuIconDownload);
}
```

- [ ] **Step 3: 挂到相册菜单**

`AddDownloadAlbumAction` 末尾（两个 `AddAction` 之后）加：

```cpp
	AddDownloadVideosSelector(menu, window, docs);
```

- [ ] **Step 4: include 补齐（AyuGram 块）**

```cpp
#include "ui/widgets/menu/menu_action.h"
#include "ui/text/format_values.h"
#include <QMenu>
```

`base/unique_qptr.h` 已在 `menu/` 内其它 include 链可用；若编不过再加。

- [ ] **Step 5: 静态验证**

```bash
grep -n "RunBatchSave\|AddDownloadVideosSelector" Telegram/SourceFiles/menu/menu_item_download_files.cpp
grep -n "ayu_SelectVideosToDownload\|ayu_DownloadSelectedVideos" Telegram/SourceFiles/menu/menu_item_download_files.cpp
```

---

### Task 5: lang 键（`Telegram/Resources/langs/lang.strings`）

**Files:**
- Modify: `Telegram/Resources/langs/lang.strings`

- [ ] **Step 1: ayu 组（:9185 之后）**

```
"ayu_SelectVideosToDownload" = "Select videos to download...";
"ayu_DownloadSelectedVideos" = "Download selected videos";
```

- [ ] **Step 2: sexcat 组（:9218 之后）**

```
"lng_sexcat_videos_selected#one" = "{count} video selected";
"lng_sexcat_videos_selected#other" = "{count} videos selected";
"lng_sexcat_already_saved" = "Already in library, download skipped: {path}";
"lng_sexcat_nothing_selected" = "No videos selected";
```

- [ ] **Step 3: 验证**

```bash
grep -c "ayu_SelectVideosToDownload\|ayu_DownloadSelectedVideos\|lng_sexcat_videos_selected#\|lng_sexcat_already_saved\|lng_sexcat_nothing_selected" Telegram/Resources/langs/lang.strings   # 期望 6
grep -n "lng_sexcat_videos_selected#one" Telegram/Resources/langs/lang.strings
```

---

### Task 6: 交付收尾

- [ ] **Step 1: 全量静态检查**

```bash
git diff --check                                                # 空白干净
grep -rn "PruneStalePending" Telegram/SourceFiles               # 0
grep -rn "AugmentedDocumentName" Telegram/SourceFiles           # h + 2 cpp
grep -rn "Modify.*data_document" /dev/null 2>/dev/null; true
```

- [ ] **Step 2: 行尾与 BOM**

```bash
file Telegram/SourceFiles/ayu/sexcat/sexcat_save_hook.cpp Telegram/SourceFiles/data/data_document.cpp | head
git diff --stat
```

- [ ] **Step 3: 交付文档**

`docs/delivery-q49-q50-write-point.md`：改动点表（文件:行 + 一句话）、四缺口对应关系、外部构建机手测清单（5 条）、已知限制（lib_ui 未在盘 → 子菜单/Toast 以仓内先例为准；`askDownloadPath=开` 的长下载不再依赖 rename）。

- [ ] **Step 4: CONTEXT.md 追加 §18.6 并提交**

```bash
git add -A && git commit -m "Rename downloads at write point, add album video picker"
```

Commit 体：一句话说明（含 ADR-0006 Amendment 1 引用），不带任何 co-author 尾缀。
