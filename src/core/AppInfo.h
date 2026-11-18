#pragma once

namespace mp {

// 發佈包裡主程式的檔名。使用者可以把 exe 改名，所以「現在這個 exe 叫什麼」要問系統；
// 這個常數只用在認發佈包裡的那一個檔（自動更新），與清掉舊版用這個名字做的登錄。
constexpr wchar_t kPackageExeName[] = L"mpainter.exe";

// 發佈包裡的子資料夾（根目錄以外，檔案會放在哪些資料夾）。打包（.github/workflows/build.yml、scripts/package.sh）
// 多放一個資料夾時，這裡要跟著加：新版啟動時靠它找上次更新留下的 .mpainter.tempold 檔（core/Install.h 的 removeOldFiles）。
// 每個隨附的 plugin 都有一個 plugins\<id>（執行中載入的 plugin DLL 和 exe 一樣，更新時只能改名、等新版刪）。
constexpr const wchar_t* kPackageSubdirs[] = {L"lang", L"plugins\\jxl"};

// 發佈包隨附的 plugin（plugins\<id>\ 的 id）。使用者沒設定過的話，隨附的算啟用、其他的算停用。
// 加一個時 kPackageSubdirs 也要加 plugins\<id>（測試會檢查）。
constexpr const wchar_t* kBundledPlugins[] = {L"jxl"};

} // namespace mp
