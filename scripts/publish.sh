#!/usr/bin/env bash
# 把開發 repo 裡「對外公開的子集」同步到公開 repo（GitHub 上的 mini-painter），並在那邊 commit。
# 開發 repo 的四份管理文件、CLAUDE.md、docs/ref 的參考截圖不進公開 repo；歷史也分開（公開 repo 從 v0.6.0 起算）。
#
# 用法：scripts/publish.sh "<commit 訊息>" [--push]
#   公開 repo 的位置預設是本資料夾旁邊的 mini-painter-pub，可用環境變數 PUBLIC_REPO 覆蓋。
#   第一次：先 git init -b main 那個資料夾並加好 origin，再跑這支。
#   發新版：改版本號 → commit → 這支 --push → 到公開 repo 打 vX.Y.Z 標籤推上去，CI 自動開 Release（步驟見 project_info.md「發新版」）。
# 做法：清掉公開 repo 裡除了 .git 以外的所有檔案，把開發 repo 追蹤中、且不在排除清單的檔案照路徑複製過去，
#   git add -A 後 commit。刪檔、改名都會正確反映。
set -euo pipefail
cd "$(dirname "$0")/.."
DEV="$PWD"
PUB="${PUBLIC_REPO:-$(dirname "$DEV")/mini-painter-pub}"
MSG="${1:-}"
[[ -z "$MSG" ]] && { echo "用法：scripts/publish.sh \"<commit 訊息>\" [--push]"; exit 1; }
[[ -d "$PUB/.git" ]] || { echo "公開 repo 不存在：$PUB（先 git init -b main）"; exit 1; }

# 排除清單（開發 repo 內部用的東西）
EXCLUDE='^(project_info\.md|file_info\.md|plan\.md|history\.md|CLAUDE\.md|bunny-size-preview\.png|docs/)'

if [[ -n "$(git status --porcelain)" ]]; then
    echo "開發 repo 有未 commit 的變更，先 commit 再發佈。"; exit 1
fi

# 1. 清空公開 repo 的工作目錄（.git 留著）
find "$PUB" -mindepth 1 -maxdepth 1 ! -name .git -exec rm -rf {} +
# 2. 複製
git ls-files -z | grep -zEv "$EXCLUDE" | tar --null -T - -cf - | (cd "$PUB" && tar xf -)
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
