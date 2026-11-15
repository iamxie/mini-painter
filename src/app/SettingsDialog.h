#pragma once
#include <windows.h>

#include <functional>
#include <string>
#include <utility>
#include <vector>

#include "core/Settings.h"

namespace mp {

// 「設定」（modal PropertySheet：一般 / 模式 / 壓縮檔 / Windows 整合，取消 / 確定 / 套用）。
struct SettingsDialogInfo {
    std::wstring iniPath;
    bool iniWritable = true;
    bool isWin11 = false;
    std::function<void()> onSetDefault; // 按「設為預設圖片程式…」
    std::vector<std::pair<std::wstring, std::wstring>> languages; // (代碼, 名稱)，語言下拉的順序；「跟隨系統」由對話框自己加在最前面
    std::wstring exeDir;                // 算預設暫存位置用
    std::wstring unzipRootInUse;        // 目前實際使用中的暫存根目錄（顯示路徑與用量）
    int startPage = 0;                  // 0 一般、1 模式、2 壓縮檔、3 Windows 整合
    // 按「套用」或「確定」：s 是整份新設定（目前的檢視值照抄進來，對話框不改它們）。對話框還開著，
    // 呼叫端用 mergeDialogSettings 併進自己的設定、照回傳的變更套用。
    std::function<void(const Settings& s)> onApply;
};
void showSettingsDialog(HWND owner, const Settings& s, const SettingsDialogInfo& info);

} // namespace mp
