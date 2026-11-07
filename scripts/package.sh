#!/usr/bin/env bash
# 從 VM 抓建置成品，組出 out/ 發佈包，並準備 GitHub release 要一起掛的 7-Zip 原始碼包。
# 用法：scripts/package.sh
# 前提：scripts/vm-sync.sh 已建置成功；tunnel 已開。
# 產出：
#   out/                 mpainter.exe、7z.dll、readme 中英、LICENSE、NOTICE、THIRD-PARTY-NOTICES.txt、lang/
#   out/release-assets/  7z2605-src.7z（LGPL §4：原始碼要和二進位放在同一個下載處，發佈時一起上傳）
set -euo pipefail
cd "$(dirname "$0")/.."

KEY="$HOME/.ssh/claude_agent_ed25519"
OPTS=(-i "$KEY" -o IdentitiesOnly=yes -o BatchMode=yes -o ConnectTimeout=8 -o StrictHostKeyChecking=no
      -o UserKnownHostsFile=/dev/null -o LogLevel=ERROR)
fetch() { scp "${OPTS[@]}" -P 12222 "Docker@127.0.0.1:$1" "$2"; }

# 釘住的版本與 hash
while IFS='=' read -r k v; do
    [[ -z "$k" || "$k" == \#* ]] && continue
    declare "LOCK_$k=$v"
done < scripts/7zip.lock

mkdir -p out/lang out/release-assets
fetch "C:/work/mini-painter/build/mpainter.exe" out/mpainter.exe
fetch "C:/work/mini-painter/build/7z.dll" out/7z.dll

check() { # check <file> <sha256>
    local got
    got=$(sha256sum "$1" | cut -d' ' -f1)
    if [[ "$got" != "$2" ]]; then echo "hash 不符：$1"; echo "  期望 $2"; echo "  實際 $got"; exit 1; fi
    echo "ok  $1"
}
check out/7z.dll "$LOCK_dll_sha256"

# 7-Zip 原始碼包：VM 上有就抓，沒有就下載
src_name=$(basename "$LOCK_src_url")
if [[ ! -f "out/release-assets/$src_name" ]]; then
    fetch "C:/work/7zip-dl/$src_name" "out/release-assets/$src_name" 2>/dev/null ||
        curl -fsSL "$LOCK_src_url" -o "out/release-assets/$src_name"
fi
check "out/release-assets/$src_name" "$LOCK_src_sha256"

cp readme.txt readme_en.txt LICENSE NOTICE THIRD-PARTY-NOTICES.txt out/
cp src/lang/ja-JP.txt src/lang/zh-TW.txt out/lang/
echo "=== out/ ==="
ls -la out out/lang out/release-assets
