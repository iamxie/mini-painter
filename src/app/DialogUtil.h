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

// 要告訴使用者一件事，走哪個管道（2026-11-16 定的規則，新增訊息照這個選）：
//   - 要使用者回答、或操作失敗沒有完成：對話框。只有確認鈕的用 showMessage，「好 / 取消」用 askOk，
//     「是 / 否（/ 取消）」用 MessageBox 的 MB_YESNO / MB_YESNOCANCEL（按鈕文字跟系統走）。
//   - 不必回應的狀態通知（翻到底、跳本、已更新、metadata 沒寫全、設定存不進去）：畫布中央的提示（MainWindow::showToast）。
//   - 圖開不了：畫在畫布上（MainWindow::showLoadError），因為那張圖的位置本來就空著。
// 確認按鈕一律叫「好」，所以不用 MessageBox 的 MB_OK / MB_OKCANCEL（繁中系統會顯示「確定」）。

// 只有「好」一顆按鈕的訊息框（TaskDialog）。icon 是 TD_INFORMATION_ICON、TD_WARNING_ICON、TD_ERROR_ICON 這類。
// owner 可以是 nullptr（主視窗還沒有、或已經沒有了）。
void showMessage(HWND owner, const std::wstring& text, PCWSTR icon);

// 確認框（TaskDialog）：「好」，withCancel 時多一顆取消。回傳使用者是不是按了「好」。
// dontAskAgain 不是 nullptr：多一個「不再提醒」的勾選框，勾了沒有寫在這裡（按取消時勾的也算）。
bool askOk(HWND owner, const std::wstring& text, PCWSTR icon, bool withCancel, bool* dontAskAgain = nullptr);

} // namespace mp
