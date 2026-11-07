#!/usr/bin/env bash
# 把本機原始碼同步到 VM 的 C:\work\mini-painter 並建置。
# 用法：scripts/vm-sync.sh [額外的 cmake 參數]
# 需要 tunnel 已開（本機 12222 -> VM 的 SSH）。
set -euo pipefail
cd "$(dirname "$0")/.."

KEY="$HOME/.ssh/claude_agent_ed25519"
OPTS=(-i "$KEY" -o IdentitiesOnly=yes -o BatchMode=yes -o ConnectTimeout=8 -o StrictHostKeyChecking=no
      -o UserKnownHostsFile=/dev/null -o LogLevel=ERROR)
w() { ssh "${OPTS[@]}" -p 12222 Docker@127.0.0.1 "$@"; }

w 'echo tunnel-ok' >/dev/null || { echo "SSH 連不上，先重開 tunnel"; exit 1; }

# 打包：src、CMakeLists.txt、vcpkg.json、scripts。build.cmd 要 CRLF。
tmp=$(mktemp -d)
mkdir -p "$tmp/pkg"
cp -r src CMakeLists.txt vcpkg.json "$tmp/pkg/"
mkdir -p "$tmp/pkg/scripts"
sed 's/$/\r/' scripts/vm-build.cmd > "$tmp/pkg/scripts/vm-build.cmd"
(cd "$tmp/pkg" && tar -cf ../src.tar .)
scp "${OPTS[@]}" -P 12222 "$tmp/src.tar" Docker@127.0.0.1:C:/work/src.tar
rm -rf "$tmp"

w 'powershell -NoProfile -Command "New-Item -ItemType Directory -Force C:\work\mini-painter | Out-Null; if (Test-Path C:\work\mini-painter\src) { Remove-Item -Recurse -Force C:\work\mini-painter\src }; tar -xf C:\work\src.tar -C C:\work\mini-painter"'
w "C:\\work\\mini-painter\\scripts\\vm-build.cmd $*"
