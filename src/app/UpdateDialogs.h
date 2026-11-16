#pragma once
#include <windows.h>

#include <string>
#include <vector>

#include "core/Update.h"

namespace mp {

// 新版本對話框（modal）：「新版 X（目前 Y）」、更新說明、三顆按鈕。關閉視窗或 Esc 等於「稍後提醒」。
enum class UpdateChoice { Skip, Later, Install };
UpdateChoice showUpdateDialog(HWND owner, const ReleaseInfo& release, const std::wstring& currentVersion);

// 手動檢查：背景查 url，0.3 秒內查完就不顯示進度。按取消馬上中斷連線（回 Cancelled），不必等伺服器逾時。
UpdateError checkUpdateWithProgress(HWND owner, const std::wstring& url, ReleaseInfo& out);

// 下載發佈包到 workDir、核對 .sha256、解到 workDir\pkg（stagingDir 回傳這個路徑）。有進度、可取消（回 Cancelled）。
// 流程本身在 core 的 downloadUpdate。
UpdateError downloadUpdateWithProgress(HWND owner, const ReleaseInfo& release, const std::wstring& workDir, std::wstring& stagingDir);

// 自動更新前請其他 mpainter 關閉：背景依序對 windows 送 closeMsg（wParam = 自己的 pid，lParam = 這個進度框的視窗；
// 對方有沒存的修改會先問，按取消回 0），再等 processes（要有 SYNCHRONIZE）都結束，最多 30 秒。
// 有一個不關、逾時或使用者在這邊按了取消就回 false。對方還在問使用者的時候這邊按取消會馬上回來：
// 進度框關掉之後，對方看到 lParam 那個視窗不在了，就不會關自己。
bool closeWindowsWithProgress(HWND owner, UINT closeMsg, const std::vector<HWND>& windows, const std::vector<HANDLE>& processes);

} // namespace mp
