#!/usr/bin/env bash
# 把開發 repo 裡「對外公開的子集」同步到公開 repo（GitHub 上的 mini-painter），並在那邊 commit。
# 開發 repo 的四份管理文件、reflection.md、CLAUDE.md、docs/ref 的參考截圖不進公開 repo；歷史也分開（公開 repo 從 v0.6.0 起算）。
#
# 用法：scripts/publish.sh "<commit 訊息>" [--push]
#   公開 repo 的位置預設是本資料夾旁邊的 mini-painter-pub，可用環境變數 PUBLIC_REPO 覆蓋。
#   第一次：先 git init -b main 那個資料夾並加好 origin，再跑這支。
#   發新版：改版本號 → commit → 這支 --push → 到公開 repo 打 vX.Y.Z 標籤推上去，CI 自動開 Release（步驟見 project_info.md「發新版」）。
# 做法：先把開發 repo 追蹤中、且不在排除清單的檔案照路徑複製到暫存資料夾；複製成功才清掉公開 repo 裡
#   除了 .git 以外的所有檔案、把暫存資料夾的內容搬進去，git add -A 後 commit。刪檔、改名都會正確反映。
# 防呆：公開 repo 的路徑不能是開發 repo、不能互相包含，裡面也不能有開發 repo 才有的檔案（表示路徑設錯了）。
# 測試：scripts/tests/test_publish.sh
set -euo pipefail
cd "$(dirname "$0")/.."
DEV="$PWD"
PUB="${PUBLIC_REPO:-$(dirname "$DEV")/mini-painter-pub}"
MSG="${1:-}"
[[ -z "$MSG" ]] && { echo "用法：scripts/publish.sh \"<commit 訊息>\" [--push]"; exit 1; }
[[ -d "$PUB/.git" ]] || { echo "公開 repo 不存在：$PUB（先 git init -b main）"; exit 1; }

# 下面會清空 $PUB 的工作目錄，先確定它不是開發 repo。Windows 路徑不分大小寫，轉小寫再比。
dev_real=$(cd "$DEV" && pwd -P); dev_real=${dev_real,,}
pub_real=$(cd "$PUB" && pwd -P); pub_real=${pub_real,,}
case "$pub_real/" in "$dev_real/"*) echo "公開 repo 不能是開發 repo 或在它裡面：$PUB"; exit 1;; esac
case "$dev_real/" in "$pub_real/"*) echo "開發 repo 不能在公開 repo 裡面：$PUB"; exit 1;; esac
for f in CLAUDE.md plan.md history.md reflection.md; do
    [[ -e "$PUB/$f" ]] && { echo "$PUB 裡有 $f，看起來不是公開 repo。"; exit 1; }
done

# 排除清單（開發 repo 內部用的東西）
EXCLUDE='^(project_info\.md|file_info\.md|plan\.md|history\.md|reflection\.md|CLAUDE\.md|bunny-size-preview\.png|docs/)'

if [[ -n "$(git status --porcelain)" ]]; then
    echo "開發 repo 有未 commit 的變更，先 commit 再發佈。"; exit 1
fi

# 1. 先複製到暫存資料夾：這一步失敗的話公開 repo 還沒被動到
STAGE=$(mktemp -d)
trap 'rm -rf "$STAGE"' EXIT
git ls-files -z | grep -zEv "$EXCLUDE" | tar --null -T - -cf - | (cd "$STAGE" && tar xf -)
[[ -n "$(ls -A "$STAGE")" ]] || { echo "沒有可以發佈的檔案。"; exit 1; }
# 2. 清空公開 repo 的工作目錄（.git 留著），把暫存資料夾的內容搬進去
find "$PUB" -mindepth 1 -maxdepth 1 ! -name .git -exec rm -rf {} +
cp -a "$STAGE"/. "$PUB"/
# 3. commit
cd "$PUB"
git add -A
if git diff --cached --quiet; then
    echo "公開 repo 沒有變更。"
else
    git commit -q -m "$MSG"
    git log --oneline -1
fi
if [[ "${2:-}" == "--push" ]]; then git push origin HEAD; fi
