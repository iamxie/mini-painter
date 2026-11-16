#include "app/DialogUtil.h"

#include <commctrl.h>
#include <windowsx.h>

#include <algorithm>

#include "app/AppIds.h"

#include "core/Lang.h"

namespace mp {

namespace {

LRESULT CALLBACK dialogScrollProc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR) {
    auto scrollTo = [&](int pos) {
        SCROLLINFO si{sizeof(si), SIF_ALL};
        GetScrollInfo(dlg, SB_VERT, &si);
        pos = std::clamp<int>(pos, si.nMin, std::max<int>(si.nMin, si.nMax - static_cast<int>(si.nPage) + 1));
        if (pos == si.nPos) return;
        ScrollWindowEx(dlg, 0, si.nPos - pos, nullptr, nullptr, nullptr, nullptr, SW_SCROLLCHILDREN | SW_INVALIDATE | SW_ERASE);
        si.fMask = SIF_POS;
        si.nPos = pos;
        SetScrollInfo(dlg, SB_VERT, &si, TRUE);
    };
    switch (msg) {
    case WM_VSCROLL: {
        SCROLLINFO si{sizeof(si), SIF_ALL};
        GetScrollInfo(dlg, SB_VERT, &si);
        int line = static_cast<int>(GetDpiForWindow(dlg)) * 24 / 96, pos = si.nPos;
        switch (LOWORD(wp)) {
        case SB_LINEUP: pos -= line; break;
        case SB_LINEDOWN: pos += line; break;
        case SB_PAGEUP: pos -= static_cast<int>(si.nPage); break;
        case SB_PAGEDOWN: pos += static_cast<int>(si.nPage); break;
        case SB_THUMBTRACK: case SB_THUMBPOSITION: pos = si.nTrackPos; break;
        case SB_TOP: pos = si.nMin; break;
        case SB_BOTTOM: pos = si.nMax; break;
        }
        scrollTo(pos);
        return 0;
    }
    case WM_MOUSEWHEEL: {
        SCROLLINFO si{sizeof(si), SIF_POS};
        GetScrollInfo(dlg, SB_VERT, &si);
        scrollTo(si.nPos - GET_WHEEL_DELTA_WPARAM(wp) * static_cast<int>(GetDpiForWindow(dlg)) * 48 / 96 / WHEEL_DELTA);
        return 0;
    }
    case WM_NCDESTROY:
        RemoveWindowSubclass(dlg, dialogScrollProc, 1);
        break;
    }
    return DefSubclassProc(dlg, msg, wp, lp);
}

} // namespace

void fitDialogToWorkArea(HWND dlg) {
    MONITORINFO mi{sizeof(mi)};
    if (!GetMonitorInfoW(MonitorFromWindow(dlg, MONITOR_DEFAULTTONEAREST), &mi)) return;
    RECT wr, cr;
    GetWindowRect(dlg, &wr);
    GetClientRect(dlg, &cr);
    const int avail = mi.rcWork.bottom - mi.rcWork.top, h = wr.bottom - wr.top;
    if (h <= avail) return;
    const int contentH = cr.bottom, sbw = GetSystemMetricsForDpi(SM_CXVSCROLL, GetDpiForWindow(dlg));
    ShowScrollBar(dlg, SB_VERT, TRUE);
    // 捲軸占掉的寬度補回去，右邊的控制項才不會被蓋住；DS_CENTER 算出來的位置可能在工作區上方，貼齊頂端。
    const int newW = wr.right - wr.left + sbw;
    const int x = std::clamp<int>(wr.left, mi.rcWork.left, std::max<int>(mi.rcWork.left, mi.rcWork.right - newW));
    SetWindowPos(dlg, nullptr, x, mi.rcWork.top, newW, avail, SWP_NOZORDER | SWP_NOACTIVATE);
    GetClientRect(dlg, &cr);
    SCROLLINFO si{sizeof(si), SIF_RANGE | SIF_PAGE | SIF_POS};
    si.nMin = 0;
    si.nMax = contentH - 1;
    si.nPage = static_cast<UINT>(cr.bottom);
    si.nPos = 0;
    SetScrollInfo(dlg, SB_VERT, &si, TRUE);
    SetWindowSubclass(dlg, dialogScrollProc, 1, 0);
}

void translateDialog(HWND dlg) {
    wchar_t buf[512];
    auto translate = [&](HWND h) {
        if (GetWindowTextW(h, buf, 512) <= 0) return;
        const wchar_t* t = tr(buf);
        if (t != buf) SetWindowTextW(h, t);
    };
    translate(dlg);
    EnumChildWindows(dlg, [](HWND h, LPARAM lp) -> BOOL {
        (*reinterpret_cast<decltype(translate)*>(lp))(h);
        return TRUE;
    }, reinterpret_cast<LPARAM>(&translate));
}

bool askOk(HWND owner, const std::wstring& text, PCWSTR icon, bool withCancel) {
    TASKDIALOG_BUTTON ok{IDOK, tr(L"OK")};
    TASKDIALOGCONFIG td{sizeof(td)};
    td.hwndParent = owner;
    td.dwFlags = TDF_POSITION_RELATIVE_TO_WINDOW | (withCancel ? TDF_ALLOW_DIALOG_CANCELLATION : 0);
    td.dwCommonButtons = withCancel ? TDCBF_CANCEL_BUTTON : 0;
    td.cButtons = 1;
    td.pButtons = &ok;
    td.pszWindowTitle = kAppName;
    td.pszMainIcon = icon;
    td.pszContent = text.c_str();
    int pressed = IDCANCEL;
    if (FAILED(TaskDialogIndirect(&td, &pressed, nullptr, nullptr))) return false;
    return pressed == IDOK;
}

} // namespace mp
