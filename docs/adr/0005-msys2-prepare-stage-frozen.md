# ADR-0005 — msys64 prepare stage 凍結：不自作聰明"加固" pacman 步驟

- 狀態: Accepted（2025-10-29 用戶定案："人家502只是偶發錯誤你越改越亂"）
- 關聯提交: `6ea3f1743f`（鏡像置頂+重試，已回滾）→ `3d36509876`（$repo 修正，已回滾）→ `2a883a5291`（撤鏡像留重試）→ `dea340750d`（逐字節恢復原版）

## Context

CI `prepare` 的 `[2/34](ThirdParty/msys64)` 三次失敗，三次根因各不相同，其中兩次是本會話自己引入的：

1. **第一次（真·環境瞬斷）**: pacman 從第三方鏡像 `ca.mirrors.cicku.me` 拉 `pinentry` 的 `.sig` 返回 502。
   官方源 `repo.msys2.org/mingw/mingw64/mingw64.db` 實測 `HTTP 200`，基礎鏡像與默認 mirrorlist 本身沒有問題。
2. **第二次（本會話引入）**: 往 `mirrorlist.mingw` 置頂 `Server = https://repo.msys2.org/mingw/x86_64`。
   該文件被全部 mingw 倉庫（mingw32/64/ucrt64/clang64/clangarm64）**共用**，路徑寫死 → ucrt64.db 等 404。
   改用 `$repo` 佔位符後仍然 404——基礎鏡像裡的舊 pacman 不展開 `$repo`，拼出 `mingw//xx.db`。
3. **第三次（本會話引入）**: 把兩條 pacman 包進 retry 循環。msys2 核心升級（msys2-runtime/pacman）完成後有
   **自殺重啟**機制——殺掉自己的終端進程樹，腳本靠"繼續執行第二條 pacman 命令"完成兩段式升級。
   retry 循環改變了進程樹，重啟機制把 bash 連同外層一起殺掉 → FAILED（日誌特徵：同步成功、6 個核心包裝完、
   `To complete this update all MSYS2 processes...` + 兩個 `SUCCESS: The process with PID ... has been terminated` 後直接 FAILED）。

## Decision

1. **`stage('msys64')` 凍結**：與倉庫原版逐字節一致（以 `git show a34f44e68b:Telegram/build/prepare/prepare.py` 的同段為基準，
   用腳本 diff 驗證 `IDENTICAL`），禁止再加鏡像置頂、retry 循環、或任何改變進程樹的包裝。
2. **瞬發性鏡像錯誤（502 / too slow / 偶發 404）的標準處置 = 原樣重跑 prepare**，不改腳本。
3. 若未來必須動這一步，先滿足兩個前提：a) 理解 msys2 兩段式升級的自殺重啟流程；b) 在真實 Windows 環境驗證過進程樹行為，而不是只看日誌猜。
4. 診斷紀律：改共享配置文件（如被多倉庫共用的 mirrorlist）之前，先用 `curl -I` 實測目標 URL，並確認文件被哪些倉庫 Include；證據優先於記憶。

## Consequences

- 正向：恢復已知可用配置；瞬斷重跑即過；避免再次把環境偶發錯誤誤診為腳本缺陷。
- 負向：偶發 502 仍會讓單次 prepare 失敗（重跑成本可接受）；若官方鏡像將來真的大面積故障，需要重新走本 ADR 的前提流程再動手。
