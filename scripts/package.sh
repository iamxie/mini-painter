#!/usr/bin/env bash
# 從 VM 抓建置成品，組出 out/ 發佈包，並準備 GitHub release 要一起掛的 7-Zip 原始碼包。
# 用法：scripts/package.sh
# 前提：scripts/vm-sync.sh 已建置成功；tunnel 已開。
# 產出：
#   out/                 mpainter.exe、7z.dll、scripts/package-files.txt 列的檔案、lang/（src/lang 的每個語系檔）
#   out/release-assets/  7z2605-src.7z（LGPL §4：原始碼要和二進位放在同一個下載處，發佈時一起上傳）
# 只給本機驗證用；正式的發佈包由 CI 產生，內容照同一份 package-files.txt 與同一個 lang 規則。
set -euo pipefail
cd "$(dirname "$0")/.."
source scripts/vm-ssh.sh

# 釘住的版本與 hash
while IFS='=' read -r k v; do
    [[ -z "$k" || "$k" == \#* ]] && continue
    declare "LOCK_$k=${v%$'\r'}"
done < scripts/7zip.lock

rm -rf out/lang
mkdir -p out/lang out/release-assets
vm_get "C:/work/mini-painter/build/mpainter.exe" out/mpainter.exe
vm_get "C:/work/mini-painter/build/7z.dll" out/7z.dll

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
    vm_get "C:/work/7zip-dl/$src_name" "out/release-assets/$src_name" 2>/dev/null ||
        curl -fsSL "$LOCK_src_url" -o "out/release-assets/$src_name"
fi
check "out/release-assets/$src_name" "$LOCK_src_sha256"

# 根目錄的檔案照清單；語系檔是 src/lang 的每個 .txt（_ 開頭的是範本，不放）。
while IFS= read -r f; do
    f=${f%$'\r'}
    [[ -z "$f" || "$f" == \#* ]] && continue
    cp "$f" out/
done < scripts/package-files.txt
for f in src/lang/*.txt; do
    [[ "$(basename "$f")" == _* ]] || cp "$f" out/lang/
done
echo "=== out/ ==="
ls -la out out/lang out/release-assets
