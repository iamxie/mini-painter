#pragma once
#include <windows.h>

namespace mp {

// 訊息框與主視窗的標題。
inline constexpr wchar_t kAppName[] = L"mpainter";
// 主視窗的視窗類別（自動更新要找出別的 mpainter 的主視窗）。
inline constexpr wchar_t kMainWindowClass[] = L"MiniPainterMain";
// 發佈頁：不能自動更新時開給使用者。
inline constexpr wchar_t kReleasesPage[] = L"https://github.com/iamxie/mini-painter/releases";

// 主視窗收的自訂訊息。WM_APP 起的編號全專案只有這些，加上進度對話框自己內部用的 WM_APP + 10 起（ProgressDialog.cpp）。
constexpr UINT WM_MP_EDIT_LOST_FOCUS = WM_APP + 1; // 文字編輯框把焦點交回主視窗
constexpr UINT WM_MP_ASK_DEFAULT = WM_APP + 2;     // 第一次開圖後詢問預設程式
// 彈出選單關閉了：把選單物件放掉（PopupMenus::reap）。wParam 是 0 時還要把鍵盤焦點拿回去
// （選完項目關閉的；編輯中就還給編輯框）；1 是別的視窗被啟用而關的，不必搶焦點。
constexpr UINT WM_MP_POPUP_CLOSED = WM_APP + 3;
constexpr UINT WM_MP_UPDATE_FOUND = WM_APP + 4;    // 背景查到新版：lParam = new 出來的 ReleaseInfo*，收到的人 delete

} // namespace mp
