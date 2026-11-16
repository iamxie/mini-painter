#pragma once
#include <windows.h>

#include <string>

namespace mp {

// 把對話框標題與每個子控制項的文字當 key 查 tr()，有翻譯就換掉。
// 對話框範本（app.rc）一律寫英文，在 WM_INITDIALOG 一開始呼叫這個。
void translateDialog(HWND dlg);

// 對話框比螢幕工作區高時的備案：縮到工作區高度，加垂直捲軸捲內容。
// 1366×768 @125% 或 1920×1080 @175% 的筆電會遇到（調整顏色超過 600 邏輯像素）。
void fitDialogToWorkArea(HWND dlg);

// 確認框（TaskDialog）。確認按鈕一律叫「好」，所以不用 MessageBox 的 MB_OKCANCEL / MB_OK（系統會顯示「確定」）。
// withCancel：多一顆取消。回傳使用者是不是按了「好」。icon 是 TD_INFORMATION_ICON、TD_WARNING_ICON 這類。
bool askOk(HWND owner, const std::wstring& text, PCWSTR icon, bool withCancel);

} // namespace mp
