#pragma once

namespace mp {

// 發佈包裡主程式的檔名。使用者可以把 exe 改名，所以「現在這個 exe 叫什麼」要問系統；
// 這個常數只用在認發佈包裡的那一個檔（自動更新），與清掉舊版用這個名字做的登錄。
constexpr wchar_t kPackageExeName[] = L"mpainter.exe";

// 發佈包裡的子資料夾（根目錄以外，檔案會放在哪些資料夾）。打包（.github/workflows/build.yml、scripts/package.sh）
// 多放一個資料夾時，這裡要跟著加：新版啟動時靠它找上次更新留下的 .old 檔（core/Install.h 的 removeOldFiles）。
constexpr const wchar_t* kPackageSubdirs[] = {L"lang"};

} // namespace mp
