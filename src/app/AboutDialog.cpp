#include "app/AboutDialog.h"

#include <cwchar>

#include "app/DialogUtil.h"
#include "app/resource.h"
#include "app/version.h"
#include "core/Lang.h"

namespace mp {

namespace {

INT_PTR CALLBACK aboutProc(HWND dlg, UINT msg, WPARAM wp, LPARAM) {
    switch (msg) {
    case WM_INITDIALOG: {
        translateDialog(dlg);
        HINSTANCE inst = reinterpret_cast<HINSTANCE>(GetWindowLongPtrW(dlg, GWLP_HINSTANCE));
        UINT dpi = GetDpiForWindow(dlg);
        int size = MulDiv(64, static_cast<int>(dpi), 96);
        HICON icon = static_cast<HICON>(LoadImageW(inst, MAKEINTRESOURCEW(IDI_APP), IMAGE_ICON, size, size, LR_DEFAULTCOLOR));
        if (icon) SendDlgItemMessageW(dlg, IDC_ABOUT_ICON, STM_SETICON, reinterpret_cast<WPARAM>(icon), 0);
        // 程式名放大加粗；字型掛在視窗上，WM_DESTROY 釋放。
        HFONT base = reinterpret_cast<HFONT>(SendMessageW(dlg, WM_GETFONT, 0, 0));
        LOGFONTW lf{};
        GetObjectW(base, sizeof(lf), &lf);
        lf.lfHeight = MulDiv(lf.lfHeight, 3, 2);
        lf.lfWeight = FW_BOLD;
        HFONT big = CreateFontIndirectW(&lf);
        SetWindowLongPtrW(dlg, DWLP_USER, reinterpret_cast<LONG_PTR>(big));
        SendDlgItemMessageW(dlg, IDC_ABOUT_NAME, WM_SETFONT, reinterpret_cast<WPARAM>(big), TRUE);
        SetDlgItemTextW(dlg, IDC_ABOUT_NAME, L"mpainter"); // 程式名不翻譯
        wchar_t ver[64];
        swprintf(ver, 64, tr(L"Version %s"), MP_VERSION_WSTRING);
        SetDlgItemTextW(dlg, IDC_ABOUT_VERSION, ver);
        SetDlgItemTextW(dlg, IDC_ABOUT_COPYRIGHT, MP_COPYRIGHT_W);
        // 焦點放「好」：Enter 是關掉，不是檢查更新（檢查更新是 tab 順序的第一個）。
        SetFocus(GetDlgItem(dlg, IDOK));
        return FALSE;
    }
    case WM_COMMAND:
        if (LOWORD(wp) == IDOK || LOWORD(wp) == IDCANCEL) { EndDialog(dlg, IDOK); return TRUE; }
        if (LOWORD(wp) == IDC_ABOUT_CHECKUPDATE) { EndDialog(dlg, IDC_ABOUT_CHECKUPDATE); return TRUE; }
        return FALSE;
    case WM_DESTROY: {
        HICON icon = reinterpret_cast<HICON>(SendDlgItemMessageW(dlg, IDC_ABOUT_ICON, STM_GETICON, 0, 0));
        if (icon) DestroyIcon(icon);
        if (HFONT f = reinterpret_cast<HFONT>(GetWindowLongPtrW(dlg, DWLP_USER))) DeleteObject(f);
        return TRUE;
    }
    default: return FALSE;
    }
}

} // namespace

bool showAboutDialog(HWND owner) {
    HINSTANCE inst = reinterpret_cast<HINSTANCE>(GetWindowLongPtrW(owner, GWLP_HINSTANCE));
    return DialogBoxParamW(inst, MAKEINTRESOURCEW(IDD_ABOUT), owner, aboutProc, 0) == IDC_ABOUT_CHECKUPDATE;
}

} // namespace mp
