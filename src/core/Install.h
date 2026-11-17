#pragma once
#include <string>
#include <vector>

#include "core/UpdateError.h"

namespace mp {

// 自動更新的換檔：把解開的發佈包放到 exe 所在的資料夾。執行中的 exe、已載入的 7z.dll 不能覆寫但能改名，
// 所以舊檔先改名成 X.mpainter.tempold，新版啟動後再刪。只認這個名字：.old / .bak 是使用者自己備份也會用的，一律不碰。
//
// 發佈包的結構只在這裡假設一次：根目錄的檔案，加上 kPackageSubdirs（core/AppInfo.h）列的子資料夾。
// planInstall 照實際解開的內容走（任意層），applyInstall 會把缺的資料夾建出來，installTargetsWritable 檢查每個要寫的資料夾；
// 只有 removeOldFiles 是新版自己啟動時跑的，那時不知道上次換了哪些檔，所以照這一版自己的結構（根目錄 + kPackageSubdirs）找。

// 資料夾能不能建檔、改名、刪檔（試建一個暫存檔再刪）。
bool isDirWritable(const std::wstring& dir);

// 正在跑同一個 exe（完整路徑相同，不分大小寫）的其他行程，不含 selfPid。更新前要先把它們關掉。
std::vector<unsigned long> otherProcessesOfImage(const std::wstring& exePath, unsigned long selfPid);

// stagingDir 是發佈包解開的地方，appDir 是 exe 所在資料夾。
// 發佈包裡的 mpainter.exe 對應到 exeName（使用者可能把 exe 改了名）；其他照相對路徑放。
struct InstallStep {
    std::wstring src; // staging 裡的檔
    std::wstring dst; // 要放到的位置
    std::wstring old; // 執行時填：dst 原本的檔改名成什麼（原本沒有就空）
    bool placed = false;
};
std::vector<InstallStep> planInstall(const std::wstring& stagingDir, const std::wstring& appDir, const std::wstring& exeName);
// 這些步驟要寫的每個資料夾都能寫嗎（還不存在的資料夾看它最近的、存在的上層）。不能寫回 FolderNotWritable，detail 是那個資料夾。
UpdateError installTargetsWritable(const std::vector<InstallStep>& steps);
// 逐一：dst 的資料夾不在就建（任意層）；dst 已存在就改名成 dst.mpainter.tempold（被占住就 …tempold2、…tempold3），
// 再把 src 複製過去。既有的 dst.old 這類檔不碰。
// 任何一步失敗就整批還原（新放的刪掉、舊檔改回原名）並回錯誤。
UpdateError applyInstall(std::vector<InstallStep>& steps);
// 刪掉 dir 與它底下 kPackageSubdirs 裡上次更新留下的 X.mpainter.tempold / X.mpainter.tempoldN（不分大小寫）。
// v0.9.9 以前的更新程式留下的 X.old 不刪（分不出是不是使用者自己的備份）。
// 回傳刪不掉的數量（舊程式還沒結束時刪不掉，下次再試）。
int removeOldFiles(const std::wstring& dir);

} // namespace mp
