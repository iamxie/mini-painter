#!/usr/bin/env bash
# 先在本機檢查語系檔，再把原始碼同步到 VM 的 C:\work\mini-painter 建置、跑測試。
# CI（.github/workflows/build.yml）檢查的項目這裡都會跑：語系檔、版本號一致（CMake configure）、建置、測試。
# 用法：scripts/vm-sync.sh [額外的 cmake 參數]
# 需要 tunnel 已開（本機 12222 -> VM 的 SSH）、本機有 uv。
set -euo pipefail
cd "$(dirname "$0")/.."

KEY="$HOME/.ssh/claude_agent_ed25519"
OPTS=(-i "$KEY" -o IdentitiesOnly=yes -o BatchMode=yes -o ConnectTimeout=8 -o StrictHostKeyChecking=no
      -o UserKnownHostsFile=/dev/null -o LogLevel=ERROR)
w() { ssh "${OPTS[@]}" -p 12222 Docker@127.0.0.1 "$@"; }

# 語系檔只看原始碼，不需要 VM：先跑，錯了不必等建置。
uv run scripts/lang_check.py

w 'echo tunnel-ok' >/dev/null || { echo "SSH 連不上，先重開 tunnel"; exit 1; }

# 打包：src、CMakeLists.txt、CMakePresets.json、vcpkg.json、scripts。*.cmd 要 CRLF（工作目錄是 LF 或 CRLF 都可以）。
tmp=$(mktemp -d)
mkdir -p "$tmp/pkg"
cp -r src scripts CMakeLists.txt CMakePresets.json vcpkg.json "$tmp/pkg/"
for f in "$tmp"/pkg/scripts/*.cmd; do sed -i 's/\r$//; s/$/\r/' "$f"; done
(cd "$tmp/pkg" && tar -cf ../src.tar .)
scp "${OPTS[@]}" -P 12222 "$tmp/src.tar" Docker@127.0.0.1:C:/work/src.tar
rm -rf "$tmp"

w 'powershell -NoProfile -Command "New-Item -ItemType Directory -Force C:\work\mini-painter | Out-Null; Remove-Item -Recurse -Force -ErrorAction SilentlyContinue C:\work\mini-painter\src, C:\work\mini-painter\scripts; tar -xf C:\work\src.tar -C C:\work\mini-painter"'
w "C:\\work\\mini-painter\\scripts\\vm-build.cmd $*"
