#!/usr/bin/env bash
# scripts/publish.sh 的測試：在暫存資料夾造一個假的開發 repo 與公開 repo，跑 publish.sh 看結果。
# 用法：scripts/tests/test_publish.sh（不碰真的 repo）
set -uo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
PUBLISH="$HERE/../publish.sh"
ROOT=$(mktemp -d)
trap 'rm -rf "$ROOT"' EXIT
export GIT_AUTHOR_NAME=t GIT_AUTHOR_EMAIL=t@example.invalid GIT_COMMITTER_NAME=t GIT_COMMITTER_EMAIL=t@example.invalid

fails=0
check() { # check <說明> <指令...>
    local what="$1"; shift
    if "$@"; then echo "ok    $what"; else echo "FAIL  $what"; fails=$((fails + 1)); fi
}
refuses() { ! "$@" >/dev/null 2>&1; }

# 假的開發 repo：公開的檔案、排除清單裡的檔案、一個沒追蹤的檔案
DEV="$ROOT/dev"; PUB="$ROOT/pub"
mkdir -p "$DEV/scripts" "$DEV/src" "$DEV/docs/ref" "$PUB"
cp "$PUBLISH" "$DEV/scripts/publish.sh"
echo code > "$DEV/src/a.txt"; echo old > "$DEV/src/gone.txt"; echo readme > "$DEV/README.md"
echo plan > "$DEV/plan.md"; echo claude > "$DEV/CLAUDE.md"; echo ref > "$DEV/docs/ref/1.txt"
git -C "$DEV" init -q -b main && git -C "$DEV" add -A && git -C "$DEV" commit -q -m init
echo keep > "$DEV/untracked.txt"
git -C "$PUB" init -q -b main
run() { (cd "$DEV" && PUBLIC_REPO="${1:-$PUB}" bash scripts/publish.sh "msg"); }

check "開發 repo 有未 commit 的變更時拒絕" refuses run
rm "$DEV/untracked.txt"

check "正常發佈成功" run
check "公開的檔案複製過去" test -f "$PUB/src/a.txt" -a -f "$PUB/README.md" -a -f "$PUB/scripts/publish.sh"
check "排除清單裡的檔案沒過去" test ! -e "$PUB/plan.md" -a ! -e "$PUB/CLAUDE.md" -a ! -e "$PUB/docs"
check "公開 repo 有一個 commit" test "$(git -C "$PUB" rev-list --count HEAD)" = 1

git -C "$DEV" rm -q src/gone.txt && git -C "$DEV" commit -q -m rm
check "第二次發佈成功" run
check "刪掉的檔案在公開 repo 也刪掉" test ! -e "$PUB/src/gone.txt" -a -f "$PUB/src/a.txt"
check "沒有變更時不多 commit" test "$(run >/dev/null 2>&1; git -C "$PUB" rev-list --count HEAD)" = 2

echo keep > "$DEV/untracked.txt"; echo "/untracked.txt" > "$DEV/.git/info/exclude"
check "公開 repo 指到開發 repo 自己時拒絕" refuses run "$DEV"
check "開發 repo 沒追蹤的檔案還在" test -f "$DEV/untracked.txt" -a -f "$DEV/plan.md"
mkdir -p "$DEV/sub" && git -C "$DEV/sub" init -q -b main
echo "/sub/" >> "$DEV/.git/info/exclude"
check "公開 repo 在開發 repo 裡面時拒絕" refuses run "$DEV/sub"

OTHER="$ROOT/other"; mkdir -p "$OTHER" && git -C "$OTHER" init -q -b main && echo x > "$OTHER/plan.md" && echo y > "$OTHER/mine.txt"
check "目標裡有開發 repo 才有的檔案時拒絕" refuses run "$OTHER"
check "被拒絕的目標沒有被清空" test -f "$OTHER/mine.txt"
check "目標不是 git repo 時拒絕" refuses run "$ROOT/nowhere"

echo
if [[ $fails -eq 0 ]]; then echo "all passed"; else echo "$fails failed"; exit 1; fi
