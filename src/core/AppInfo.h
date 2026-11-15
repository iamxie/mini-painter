#pragma once

namespace mp {

// 發佈包裡主程式的檔名。使用者可以把 exe 改名，所以「現在這個 exe 叫什麼」要問系統；
// 這個常數只用在認發佈包裡的那一個檔（自動更新），與清掉舊版用這個名字做的登錄。
constexpr wchar_t kPackageExeName[] = L"mpainter.exe";

} // namespace mp
