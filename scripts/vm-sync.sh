#!/usr/bin/env bash
# 先在本機檢查語系檔，再把原始碼同步到 VM 的 C:\work\mini-painter 建置、跑測試。
# CI（.github/workflows/build.yml）檢查的項目這裡都會跑：語系檔、版本號一致（CMake configure）、
# 7z.dll 是 scripts/7zip.lock 釘住的那一個、建置、測試。
# 用法：scripts/vm-sync.sh [額外的 cmake 參數]
# 需要 tunnel 已開（本機 12222 -> VM 的 SSH）、本機有 uv。
set -euo pipefail
cd "$(dirname "$0")/.."
source scripts/vm-ssh.sh

# 語系檔只看原始碼，不需要 VM：先跑，錯了不必等建置。
uv run scripts/lang_check.py

vm 'echo tunnel-ok' >/dev/null || { echo "SSH 連不上，先重開 tunnel"; exit 1; }

# VM 上的 7z.dll 是手動放的：建置前對一次 hash，和 CI（每次照 lock 下載）用的是同一個檔才往下做。
want=$(grep '^dll_sha256=' scripts/7zip.lock | cut -d= -f2 | tr -d '\r')
got=$(vm 'certutil -hashfile C:\work\7zip\7z.dll SHA256' | tr -d '\r ' | grep -iE '^[0-9a-f]{64}$' | tr 'A-F' 'a-f' || true)
if [[ "$got" != "$want" ]]; then
    echo "VM 上的 C:\\work\\7zip\\7z.dll 不是 scripts/7zip.lock 釘住的版本"
    echo "  期望 $want"
    echo "  實際 ${got:-（讀不到）}"
    exit 1
fi

# 打包：src、CMakeLists.txt、CMakePresets.json、vcpkg.json、scripts。*.cmd 要 CRLF（工作目錄是 LF 或 CRLF 都可以）。
tmp=$(mktemp -d)
mkdir -p "$tmp/pkg"
cp -r src scripts CMakeLists.txt CMakePresets.json vcpkg.json "$tmp/pkg/"
for f in "$tmp"/pkg/scripts/*.cmd; do sed -i 's/\r$//; s/$/\r/' "$f"; done
(cd "$tmp/pkg" && tar -cf ../src.tar .)
vm_put "$tmp/src.tar" C:/work/src.tar
rm -rf "$tmp"

vm 'powershell -NoProfile -Command "New-Item -ItemType Directory -Force C:\work\mini-painter | Out-Null; Remove-Item -Recurse -Force -ErrorAction SilentlyContinue C:\work\mini-painter\src, C:\work\mini-painter\scripts; tar -xf C:\work\src.tar -C C:\work\mini-painter"'
vm "C:\\work\\mini-painter\\scripts\\vm-build.cmd $*"
