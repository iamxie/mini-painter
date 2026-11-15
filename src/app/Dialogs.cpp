#include "app/Dialogs.h"

#include <commctrl.h>
#include <windowsx.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <string>
#include <thread>

#include "app/UiText.h"
#include "app/resource.h"
#include "app/version.h"
#include "core/AppInfo.h"
#include "core/Lang.h"
#include "core/PathUtil.h"
#include "core/TempDir.h"

namespace mp {

namespace {

// ---------------------------------------------------------------------------
// 對話框比螢幕工作區高時的備案：縮到工作區高度，加垂直捲軸捲內容。
// 1366×768 @125% 或 1920×1080 @175% 的筆電會遇到（調整顏色超過 600 邏輯像素）。設定面板是 PropertySheet，不走這裡。

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

// ---------------------------------------------------------------------------
// 調整尺寸

struct ResizeState {
    int origW, origH;
    double origDpi;
    ResizeResult out;
    bool updating = false;
    bool lock = true;
    int unit = 0;    // 0 像素 1 % 2 英寸 3 公分 4 公釐 5 點
    int dpiUnit = 0; // 0 像素/英寸 1 像素/公分
    double w, h;     // 目前欄位值（以像素為準）
    double dpi;      // 像素/英寸
};

const int kPresets[] = {0, 320, 640, 800, 1024, 1280, 1920};

double pxToUnit(double pxv, int unit, double dpi, int total) {
    switch (unit) {
    case 1: return pxv * 100.0 / total;
    case 2: return pxv / dpi;
    case 3: return pxv / dpi * 2.54;
    case 4: return pxv / dpi * 25.4;
    case 5: return pxv / dpi * 72.0;
    default: return pxv;
    }
}

double unitToPx(double v, int unit, double dpi, int total) {
    switch (unit) {
    case 1: return v * total / 100.0;
    case 2: return v * dpi;
    case 3: return v / 2.54 * dpi;
    case 4: return v / 25.4 * dpi;
    case 5: return v / 72.0 * dpi;
    default: return v;
    }
}

std::wstring fmtNum(double v, int unit) {
    wchar_t buf[32];
    if (unit == 0) swprintf(buf, 32, L"%d", static_cast<int>(std::lround(v)));
    else swprintf(buf, 32, L"%.3g", v);
    return buf;
}

double readNum(HWND dlg, int id) {
    wchar_t buf[32];
    GetDlgItemTextW(dlg, id, buf, 32);
    return _wtof(buf);
}

void resizeSync(HWND dlg, ResizeState& s) {
    s.updating = true;
    double dpi = s.dpi;
    SetDlgItemTextW(dlg, IDC_W, fmtNum(pxToUnit(s.w, s.unit, dpi, s.origW), s.unit).c_str());
    SetDlgItemTextW(dlg, IDC_H, fmtNum(pxToUnit(s.h, s.unit, dpi, s.origH), s.unit).c_str());
    SetDlgItemTextW(dlg, IDC_DPI, fmtNum(s.dpiUnit == 0 ? dpi : dpi / 2.54, s.dpiUnit == 0 ? 0 : 5).c_str());
    wchar_t buf[64];
    swprintf(buf, 64, tr(L"%d × %d pixels"), std::max(1, static_cast<int>(std::lround(s.w))), std::max(1, static_cast<int>(std::lround(s.h))));
    SetDlgItemTextW(dlg, IDC_RESULT, buf);
    s.updating = false;
}

INT_PTR CALLBACK resizeProc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp) {
    auto* s = reinterpret_cast<ResizeState*>(GetWindowLongPtrW(dlg, DWLP_USER));
    switch (msg) {
    case WM_INITDIALOG: {
        s = reinterpret_cast<ResizeState*>(lp);
        SetWindowLongPtrW(dlg, DWLP_USER, lp);
        translateDialog(dlg);
        HWND fit = GetDlgItem(dlg, IDC_FIT);
        ComboBox_AddString(fit, tr(L"Custom"));
        for (int i = 1; i < 7; ++i) {
            wchar_t b[32];
            swprintf(b, 32, L"%d × %d", kPresets[i], kPresets[i]);
            ComboBox_AddString(fit, b);
        }
        ComboBox_SetCurSel(fit, 0);
        HWND unit = GetDlgItem(dlg, IDC_UNIT);
        for (auto* u : {tr(L"pixels"), L"%", tr(L"inches"), tr(L"cm"), tr(L"mm"), tr(L"points")}) ComboBox_AddString(unit, u);
        ComboBox_SetCurSel(unit, 0);
        HWND du = GetDlgItem(dlg, IDC_DPIUNIT);
        ComboBox_AddString(du, tr(L"pixels/inch"));
        ComboBox_AddString(du, tr(L"pixels/cm"));
        ComboBox_SetCurSel(du, 0);
        Button_SetCheck(GetDlgItem(dlg, IDC_LOCK), BST_CHECKED);
        Button_SetCheck(GetDlgItem(dlg, IDC_RESAMPLE), BST_CHECKED);
        resizeSync(dlg, *s);
        return TRUE;
    }
    case WM_COMMAND: {
        if (!s || s->updating) return FALSE;
        int id = LOWORD(wp), code = HIWORD(wp);
        switch (id) {
        case IDC_FIT:
            if (code == CBN_SELCHANGE) {
                int sel = ComboBox_GetCurSel(GetDlgItem(dlg, IDC_FIT));
                if (sel > 0) {
                    double box = kPresets[sel];
                    double scale = std::min(box / s->origW, box / s->origH);
                    s->w = s->origW * scale;
                    s->h = s->origH * scale;
                    resizeSync(dlg, *s);
                }
            }
            return TRUE;
        case IDC_W:
        case IDC_H:
            if (code == EN_CHANGE) {
                double v = readNum(dlg, id);
                if (v <= 0) return TRUE;
                bool isW = id == IDC_W;
                double pxv = unitToPx(v, s->unit, s->dpi, isW ? s->origW : s->origH);
                if (isW) s->w = pxv; else s->h = pxv;
                if (s->lock) {
                    if (isW) s->h = pxv * s->origH / s->origW;
                    else s->w = pxv * s->origW / s->origH;
                }
                ComboBox_SetCurSel(GetDlgItem(dlg, IDC_FIT), 0);
                s->updating = true;
                SetDlgItemTextW(dlg, isW ? IDC_H : IDC_W,
                                fmtNum(pxToUnit(isW ? s->h : s->w, s->unit, s->dpi, isW ? s->origH : s->origW), s->unit).c_str());
                wchar_t buf[64];
                swprintf(buf, 64, tr(L"%d × %d pixels"), std::max(1, static_cast<int>(std::lround(s->w))), std::max(1, static_cast<int>(std::lround(s->h))));
                SetDlgItemTextW(dlg, IDC_RESULT, buf);
                s->updating = false;
            }
            return TRUE;
        case IDC_UNIT:
            if (code == CBN_SELCHANGE) { s->unit = ComboBox_GetCurSel(GetDlgItem(dlg, IDC_UNIT)); resizeSync(dlg, *s); }
            return TRUE;
        case IDC_LOCK:
            s->lock = Button_GetCheck(GetDlgItem(dlg, IDC_LOCK)) == BST_CHECKED;
            return TRUE;
        case IDC_DPI:
            if (code == EN_CHANGE) {
                double v = readNum(dlg, IDC_DPI);
                if (v <= 0) return TRUE;
                double newDpi = s->dpiUnit == 0 ? v : v * 2.54;
                bool resample = Button_GetCheck(GetDlgItem(dlg, IDC_RESAMPLE)) == BST_CHECKED;
                if (resample && s->unit >= 2) {
                    // 實體尺寸不變，像素跟著 DPI 變
                    s->w = s->w * newDpi / s->dpi;
                    s->h = s->h * newDpi / s->dpi;
                }
                s->dpi = newDpi;
                s->updating = true;
                SetDlgItemTextW(dlg, IDC_W, fmtNum(pxToUnit(s->w, s->unit, s->dpi, s->origW), s->unit).c_str());
                SetDlgItemTextW(dlg, IDC_H, fmtNum(pxToUnit(s->h, s->unit, s->dpi, s->origH), s->unit).c_str());
                wchar_t buf[64];
                swprintf(buf, 64, tr(L"%d × %d pixels"), std::max(1, static_cast<int>(std::lround(s->w))), std::max(1, static_cast<int>(std::lround(s->h))));
                SetDlgItemTextW(dlg, IDC_RESULT, buf);
                s->updating = false;
            }
            return TRUE;
        case IDC_DPIUNIT:
            if (code == CBN_SELCHANGE) { s->dpiUnit = ComboBox_GetCurSel(GetDlgItem(dlg, IDC_DPIUNIT)); resizeSync(dlg, *s); }
            return TRUE;
        case IDC_RESAMPLE: {
            bool resample = Button_GetCheck(GetDlgItem(dlg, IDC_RESAMPLE)) == BST_CHECKED;
            if (!resample) { s->w = s->origW; s->h = s->origH; }
            EnableWindow(GetDlgItem(dlg, IDC_W), resample);
            EnableWindow(GetDlgItem(dlg, IDC_H), resample);
            EnableWindow(GetDlgItem(dlg, IDC_FIT), resample);
            resizeSync(dlg, *s);
            return TRUE;
        }
        case IDOK:
            s->out.resample = Button_GetCheck(GetDlgItem(dlg, IDC_RESAMPLE)) == BST_CHECKED;
            s->out.width = std::clamp(static_cast<int>(std::lround(s->w)), 1, 30000);
            s->out.height = std::clamp(static_cast<int>(std::lround(s->h)), 1, 30000);
            s->out.dpi = s->dpi;
            EndDialog(dlg, IDOK);
            return TRUE;
        case IDCANCEL:
            EndDialog(dlg, IDCANCEL);
            return TRUE;
        default: return FALSE;
        }
    }
    default: return FALSE;
    }
}

// ---------------------------------------------------------------------------
// 調整顏色

struct ColorPanel {
    HWND hwnd = nullptr;
    HWND owner = nullptr;
    ColorAdjustHost* host = nullptr;
    ColorAdjustParams params;
    std::array<uint32_t, 256> hist{};
    bool updating = false;
    bool done = false;
};
ColorPanel* g_panel = nullptr;

struct SliderDef {
    int id;
    float lo, hi;
    float ColorAdjustParams::*field;
};
const SliderDef kSliders[] = {
    {IDC_T_BLACK, 0, 1, &ColorAdjustParams::blackPoint},
    {IDC_T_MID, 0, 1, &ColorAdjustParams::midtone},
    {IDC_T_WHITE, 0, 1, &ColorAdjustParams::whitePoint},
    {IDC_T_EXPO, -2, 2, &ColorAdjustParams::exposure},
    {IDC_T_CONTRAST, -1, 1, &ColorAdjustParams::contrast},
    {IDC_T_HIGH, -1, 1, &ColorAdjustParams::highlights},
    {IDC_T_SHADOW, -1, 1, &ColorAdjustParams::shadows},
    {IDC_T_SAT, -1, 1, &ColorAdjustParams::saturation},
    {IDC_T_TEMP, -1, 1, &ColorAdjustParams::temperature},
    {IDC_T_TINT, -1, 1, &ColorAdjustParams::tint},
    {IDC_T_SEPIA, 0, 1, &ColorAdjustParams::sepia},
    {IDC_T_CLARITY, 0, 1, &ColorAdjustParams::clarity},
};
constexpr int kSliderMax = 1000;

void slidersFromParams(ColorPanel& p) {
    p.updating = true;
    for (const auto& s : kSliders) {
        float v = p.params.*s.field;
        int pos = static_cast<int>(std::lround((v - s.lo) / (s.hi - s.lo) * kSliderMax));
        SendDlgItemMessageW(p.hwnd, s.id, TBM_SETPOS, TRUE, std::clamp(pos, 0, kSliderMax));
    }
    p.updating = false;
}

void paramsFromSliders(ColorPanel& p) {
    for (const auto& s : kSliders) {
        int pos = static_cast<int>(SendDlgItemMessageW(p.hwnd, s.id, TBM_GETPOS, 0, 0));
        p.params.*s.field = s.lo + (s.hi - s.lo) * pos / static_cast<float>(kSliderMax);
    }
    // 黑點不能超過白點
    if (p.params.whitePoint < p.params.blackPoint + 0.01f) p.params.whitePoint = std::min(1.0f, p.params.blackPoint + 0.01f);
}

void drawHistogram(ColorPanel& p, const DRAWITEMSTRUCT* dis) {
    HDC dc = dis->hDC;
    RECT rc = dis->rcItem;
    HBRUSH bg = CreateSolidBrush(RGB(245, 245, 245));
    FillRect(dc, &rc, bg);
    DeleteObject(bg);
    uint32_t mx = 1;
    for (auto v : p.hist) mx = std::max(mx, v);
    int w = rc.right - rc.left, h = rc.bottom - rc.top;
    HPEN pen = CreatePen(PS_SOLID, 1, RGB(90, 90, 90));
    HGDIOBJ old = SelectObject(dc, pen);
    for (int x = 0; x < w; ++x) {
        int bin = x * 256 / std::max(1, w);
        // 用對數讓細節看得到
        double v = std::log1p(static_cast<double>(p.hist[bin])) / std::log1p(static_cast<double>(mx));
        int bh = static_cast<int>(v * (h - 2));
        MoveToEx(dc, rc.left + x, rc.bottom - 1, nullptr);
        LineTo(dc, rc.left + x, rc.bottom - 1 - bh);
    }
    SelectObject(dc, old);
    DeleteObject(pen);
    // 黑點 / 白點標記
    HPEN mark = CreatePen(PS_SOLID, 1, RGB(0, 120, 255));
    old = SelectObject(dc, mark);
    for (float v : {p.params.blackPoint, p.params.whitePoint}) {
        int x = rc.left + static_cast<int>(v * (w - 1));
        MoveToEx(dc, x, rc.top, nullptr);
        LineTo(dc, x, rc.bottom);
    }
    SelectObject(dc, old);
    DeleteObject(mark);
    HPEN frame = CreatePen(PS_SOLID, 1, RGB(180, 180, 180));
    old = SelectObject(dc, frame);
    SelectObject(dc, GetStockObject(NULL_BRUSH));
    Rectangle(dc, rc.left, rc.top, rc.right, rc.bottom);
    SelectObject(dc, old);
    DeleteObject(frame);
}

// 滾輪不調滑桿，交給對話框（有捲軸備案時捲動對話框）。Windows 把滾輪送到游標下的控制項，
// 捲動對話框時滑桿一移到游標下就會被改到。
LRESULT CALLBACK sliderNoWheelProc(HWND h, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR) {
    if (msg == WM_MOUSEWHEEL) return SendMessageW(GetParent(h), msg, wp, lp);
    if (msg == WM_NCDESTROY) RemoveWindowSubclass(h, sliderNoWheelProc, 1);
    return DefSubclassProc(h, msg, wp, lp);
}

INT_PTR CALLBACK colorProc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp) {
    ColorPanel* p = g_panel;
    switch (msg) {
    case WM_INITDIALOG:
        p = reinterpret_cast<ColorPanel*>(lp);
        p->hwnd = dlg;
        translateDialog(dlg);
        fitDialogToWorkArea(dlg);
        for (const auto& s : kSliders) {
            SendDlgItemMessageW(dlg, s.id, TBM_SETRANGE, FALSE, MAKELPARAM(0, kSliderMax));
            SendDlgItemMessageW(dlg, s.id, TBM_SETPAGESIZE, 0, 50);
            SetWindowSubclass(GetDlgItem(dlg, s.id), sliderNoWheelProc, 1, 0);
        }
        p->hist = luminanceHistogram(p->host->baseImage());
        slidersFromParams(*p);
        return TRUE;
    case WM_HSCROLL:
        if (p && !p->updating) {
            paramsFromSliders(*p);
            if (LOWORD(wp) == TB_THUMBTRACK) {
                // 拖曳中只更新黑白點標記，放開才算圖，避免大圖卡住
                InvalidateRect(GetDlgItem(dlg, IDC_HIST), nullptr, FALSE);
                if (p->host->baseImage().width * p->host->baseImage().height <= 4'000'000) p->host->previewChanged(p->params);
            } else {
                InvalidateRect(GetDlgItem(dlg, IDC_HIST), nullptr, FALSE);
                p->host->previewChanged(p->params);
            }
        }
        return TRUE;
    case WM_DRAWITEM:
        if (p && wp == IDC_HIST) { drawHistogram(*p, reinterpret_cast<const DRAWITEMSTRUCT*>(lp)); return TRUE; }
        return FALSE;
    case WM_COMMAND:
        if (!p) return FALSE;
        switch (LOWORD(wp)) {
        case IDC_AUTO:
            p->params = autoLevels(p->host->baseImage(), p->params);
            slidersFromParams(*p);
            InvalidateRect(GetDlgItem(dlg, IDC_HIST), nullptr, FALSE);
            p->host->previewChanged(p->params);
            return TRUE;
        case IDC_RESET:
            p->params = {};
            slidersFromParams(*p);
            InvalidateRect(GetDlgItem(dlg, IDC_HIST), nullptr, FALSE);
            p->host->previewChanged(p->params);
            return TRUE;
        case IDC_DROPPER:
            p->host->beginPickWhite();
            return TRUE;
        case IDOK:
            p->done = true;
            p->host->commitAdjust(p->params);
            DestroyWindow(dlg);
            return TRUE;
        case IDCANCEL:
            p->done = true;
            p->host->cancelAdjust();
            DestroyWindow(dlg);
            return TRUE;
        default: return FALSE;
        }
    case WM_CLOSE:
        if (p && !p->done) { p->done = true; p->host->cancelAdjust(); }
        DestroyWindow(dlg);
        return TRUE;
    case WM_DESTROY:
        if (g_panel == p) g_panel = nullptr;
        delete p;
        return TRUE;
    default: return FALSE;
    }
}

// ---------------------------------------------------------------------------
// 關於

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

bool showResizeDialog(HWND owner, int curW, int curH, double curDpi, ResizeResult& out) {
    ResizeState s{};
    s.origW = curW;
    s.origH = curH;
    s.origDpi = curDpi > 0 ? curDpi : 72;
    s.w = curW;
    s.h = curH;
    s.dpi = s.origDpi;
    HINSTANCE inst = reinterpret_cast<HINSTANCE>(GetWindowLongPtrW(owner, GWLP_HINSTANCE));
    INT_PTR r = DialogBoxParamW(inst, MAKEINTRESOURCEW(IDD_RESIZE), owner, resizeProc, reinterpret_cast<LPARAM>(&s));
    if (r != IDOK) return false;
    out = s.out;
    return true;
}

void showColorAdjustPanel(HWND owner, ColorAdjustHost* host, float) {
    if (g_panel) { SetForegroundWindow(g_panel->hwnd); return; }
    auto* p = new ColorPanel;
    p->owner = owner;
    p->host = host;
    g_panel = p;
    HINSTANCE inst = reinterpret_cast<HINSTANCE>(GetWindowLongPtrW(owner, GWLP_HINSTANCE));
    HWND dlg = CreateDialogParamW(inst, MAKEINTRESOURCEW(IDD_COLOR), owner, colorProc, reinterpret_cast<LPARAM>(p));
    if (!dlg) { g_panel = nullptr; delete p; host->cancelAdjust(); return; }
    // 放在主視窗右上角旁邊
    RECT orc, drc;
    GetWindowRect(owner, &orc);
    GetWindowRect(dlg, &drc);
    int w = drc.right - drc.left, h = drc.bottom - drc.top;
    int x = orc.right - w - 40, y = orc.top + 120;
    MONITORINFO mi{sizeof(mi)};
    GetMonitorInfoW(MonitorFromWindow(owner, MONITOR_DEFAULTTONEAREST), &mi);
    x = std::clamp<int>(x, mi.rcWork.left, std::max<int>(mi.rcWork.left, mi.rcWork.right - w));
    y = std::clamp<int>(y, mi.rcWork.top, std::max<int>(mi.rcWork.top, mi.rcWork.bottom - h));
    SetWindowPos(dlg, nullptr, x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER);
    ShowWindow(dlg, SW_SHOW);
}

bool isColorAdjustPanelOpen() { return g_panel != nullptr; }

HWND colorAdjustPanelHwnd() { return g_panel ? g_panel->hwnd : nullptr; }

void colorAdjustPanelSetParams(const ColorAdjustParams& p) {
    if (!g_panel) return;
    g_panel->params = p;
    slidersFromParams(*g_panel);
    InvalidateRect(GetDlgItem(g_panel->hwnd, IDC_HIST), nullptr, FALSE);
}

void closeColorAdjustPanel() {
    if (!g_panel) return;
    g_panel->done = true;
    DestroyWindow(g_panel->hwnd);
}

// ---------------------------------------------------------------------------
// 壓縮檔：密碼與解壓進度

namespace {

struct PasswordState {
    std::wstring name;
    bool retry = false;
    std::wstring out;
};

INT_PTR CALLBACK passwordProc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp) {
    auto* st = reinterpret_cast<PasswordState*>(GetWindowLongPtrW(dlg, DWLP_USER));
    switch (msg) {
    case WM_INITDIALOG: {
        st = reinterpret_cast<PasswordState*>(lp);
        SetWindowLongPtrW(dlg, DWLP_USER, lp);
        translateDialog(dlg);
        std::wstring prompt(wcslen(tr(L"“%s” is encrypted. Enter the password:")) + st->name.size() + 8, L'\0');
        int n = swprintf(prompt.data(), prompt.size(), tr(L"“%s” is encrypted. Enter the password:"), st->name.c_str());
        prompt.resize(n > 0 ? static_cast<size_t>(n) : 0);
        SetDlgItemTextW(dlg, IDC_PW_PROMPT, prompt.c_str());
        SetDlgItemTextW(dlg, IDC_PW_NOTE, st->retry ? tr(L"Wrong password. Try again.") : L"");
        SendDlgItemMessageW(dlg, IDC_PW_EDIT, EM_SETLIMITTEXT, 255, 0);
        SetForegroundWindow(dlg);
        SetFocus(GetDlgItem(dlg, IDC_PW_EDIT));
        return FALSE; // 焦點已自己設
    }
    case WM_COMMAND:
        if (LOWORD(wp) == IDOK) {
            wchar_t buf[256];
            GetDlgItemTextW(dlg, IDC_PW_EDIT, buf, 256);
            st->out = buf;
            EndDialog(dlg, IDOK);
            return TRUE;
        }
        if (LOWORD(wp) == IDCANCEL) { EndDialog(dlg, IDCANCEL); return TRUE; }
        break;
    }
    return FALSE;
}

constexpr UINT WM_PRG_POS = WM_APP + 10;  // wParam = 千分比
constexpr UINT WM_PRG_TEXT = WM_APP + 11; // lParam = new 出來的 std::wstring*，收到的人 delete
constexpr UINT WM_PRG_DONE = WM_APP + 12;

// 通用的「背景做事 + 進度對話框」：work 在 worker 執行緒跑，透過 setPos / setText 更新畫面，自己看 cancel。
struct ProgressState {
    std::wstring caption, text;
    std::function<void(ProgressState&)> work;
    std::atomic<bool> cancel{false};
    std::thread worker;
    HWND dlg = nullptr;
    bool shown = false;
    bool done = false;
    void setPos(uint64_t done, uint64_t total) {
        if (dlg) PostMessageW(dlg, WM_PRG_POS, total ? static_cast<WPARAM>(done * 1000 / total) : 0, 0);
    }
    void setText(const std::wstring& t) {
        if (dlg) PostMessageW(dlg, WM_PRG_TEXT, 0, reinterpret_cast<LPARAM>(new std::wstring(t)));
    }
};

INT_PTR CALLBACK progressProc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp) {
    auto* st = reinterpret_cast<ProgressState*>(GetWindowLongPtrW(dlg, DWLP_USER));
    switch (msg) {
    case WM_INITDIALOG: {
        st = reinterpret_cast<ProgressState*>(lp);
        st->dlg = dlg;
        SetWindowLongPtrW(dlg, DWLP_USER, lp);
        translateDialog(dlg);
        SetWindowTextW(dlg, st->caption.c_str());
        SetDlgItemTextW(dlg, IDC_PRG_TEXT, st->text.c_str());
        SendDlgItemMessageW(dlg, IDC_PRG_BAR, PBM_SETRANGE32, 0, 1000);
        st->worker = std::thread([st] {
            st->work(*st);
            PostMessageW(st->dlg, WM_PRG_DONE, 0, 0);
        });
        SetTimer(dlg, 1, 300, nullptr); // 0.3 秒內解完就不閃對話框
        return TRUE;
    }
    case WM_TIMER:
        KillTimer(dlg, 1);
        if (!st->done && !st->shown) {
            st->shown = true;
            ShowWindow(dlg, SW_SHOW);
            SetForegroundWindow(dlg);
        }
        return TRUE;
    case WM_PRG_POS:
        SendDlgItemMessageW(dlg, IDC_PRG_BAR, PBM_SETPOS, wp, 0);
        return TRUE;
    case WM_PRG_TEXT: {
        auto* t = reinterpret_cast<std::wstring*>(lp);
        if (!st->cancel) SetDlgItemTextW(dlg, IDC_PRG_TEXT, t->c_str());
        delete t;
        return TRUE;
    }
    case WM_PRG_DONE:
        st->done = true;
        return TRUE;
    case WM_COMMAND:
        if (LOWORD(wp) == IDCANCEL) {
            st->cancel = true;
            EnableWindow(GetDlgItem(dlg, IDCANCEL), FALSE);
            SetDlgItemTextW(dlg, IDC_PRG_TEXT, tr(L"Cancelling…"));
            return TRUE; // 等 worker 停下來才關
        }
        break;
    case WM_CLOSE:
        st->cancel = true;
        return TRUE;
    }
    return FALSE;
}

// 跑 st.work 並顯示進度對話框。建不出對話框回 false（呼叫端自己同步做）。
bool runWithProgress(HWND owner, ProgressState& st) {
    // modeless 對話框 + 自己的訊息迴圈：這樣才能決定要不要顯示（DialogBox 一定會顯示）。
    HINSTANCE inst = reinterpret_cast<HINSTANCE>(GetWindowLongPtrW(owner, GWLP_HINSTANCE));
    HWND dlg = CreateDialogParamW(inst, MAKEINTRESOURCEW(IDD_PROGRESS), owner, progressProc, reinterpret_cast<LPARAM>(&st));
    if (!dlg) return false;
    EnableWindow(owner, FALSE);
    MSG msg;
    while (!st.done && GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (!IsDialogMessageW(dlg, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    if (st.worker.joinable()) st.worker.join();
    EnableWindow(owner, TRUE);
    DestroyWindow(dlg);
    SetActiveWindow(owner);
    return true;
}

} // namespace

bool showPasswordDialog(HWND owner, const std::wstring& archiveName, bool retry, std::wstring& password) {
    PasswordState st;
    st.name = archiveName;
    st.retry = retry;
    HINSTANCE inst = reinterpret_cast<HINSTANCE>(GetWindowLongPtrW(owner, GWLP_HINSTANCE));
    INT_PTR r = DialogBoxParamW(inst, MAKEINTRESOURCEW(IDD_PASSWORD), owner, passwordProc, reinterpret_cast<LPARAM>(&st));
    if (r != IDOK) return false;
    password = st.out;
    return true;
}

ArchiveResult extractWithProgress(HWND owner, const std::wstring& archivePath, const std::wstring& destDir,
                                  const ExtractOptions& opt) {
    ProgressState st;
    st.caption = tr(L"Unpacking");
    std::wstring name = archivePath.substr(archivePath.find_last_of(L"\\/") + 1);
    st.text.resize(wcslen(tr(L"Unpacking “%s”…")) + name.size() + 8);
    int n = swprintf(st.text.data(), st.text.size(), tr(L"Unpacking “%s”…"), name.c_str());
    st.text.resize(n > 0 ? static_cast<size_t>(n) : 0);
    ArchiveResult result;
    st.work = [&](ProgressState& s) {
        ExtractOptions o = opt;
        o.progress = [&s](uint64_t done, uint64_t total) {
            if (s.cancel.load()) return false;
            s.setPos(done, total);
            return true;
        };
        result = extractImages(archivePath, destDir, o);
    };
    // 建不出對話框就同步解，沒有進度也沒有取消。
    if (!runWithProgress(owner, st)) return extractImages(archivePath, destDir, opt);
    return result;
}

void runUnpackWithProgress(HWND owner, const std::wstring& archivePath,
                           const std::function<void(const std::function<bool(uint64_t, uint64_t)>& progress)>& work) {
    ProgressState st;
    st.caption = tr(L"Unpacking");
    std::wstring name = archivePath.substr(archivePath.find_last_of(L"\\/") + 1);
    st.text.resize(wcslen(tr(L"Unpacking “%s”…")) + name.size() + 8);
    int n = swprintf(st.text.data(), st.text.size(), tr(L"Unpacking “%s”…"), name.c_str());
    st.text.resize(n > 0 ? static_cast<size_t>(n) : 0);
    st.work = [&](ProgressState& s) {
        work([&s](uint64_t done, uint64_t total) {
            if (s.cancel.load()) return false;
            s.setPos(done, total);
            return true;
        });
    };
    if (!runWithProgress(owner, st)) work(nullptr);
}

bool closeWindowsWithProgress(HWND owner, UINT closeMsg, const std::vector<HWND>& windows,
                              const std::vector<HANDLE>& processes) {
    ProgressState st;
    st.caption = tr(L"Software Update");
    st.text = tr(L"Closing the other mpainter windows…");
    bool allClosed = false;
    st.work = [&](ProgressState& s) {
        for (HWND w : windows) {
            if (s.cancel.load()) return;
            // 對方可能在問要不要存檔，給使用者足夠的時間回答。
            DWORD_PTR agreed = 0;
            // wParam 帶自己的 pid：對方按取消時讓這邊能跳到前面顯示「更新已取消」。
            LRESULT sent = SendMessageTimeoutW(w, closeMsg, GetCurrentProcessId(), 0, SMTO_ABORTIFHUNG, 10 * 60 * 1000, &agreed);
            if ((!sent || !agreed) && IsWindow(w)) return; // 使用者在那邊按了取消，或沒回應
        }
        // 等行程真的結束（存設定、刪暫存資料夾）。
        const DWORD n = static_cast<DWORD>(processes.size());
        for (int waited = 0; waited < 30000; waited += 100) {
            if (s.cancel.load()) return;
            DWORD r = n ? WaitForMultipleObjects(n, processes.data(), TRUE, 100) : WAIT_OBJECT_0;
            if (r < WAIT_OBJECT_0 + n || n == 0) { allClosed = true; return; }
        }
    };
    if (!runWithProgress(owner, st)) st.work(st);
    return allClosed;
}

int tryPasswordsWithProgress(HWND owner, const std::wstring& archivePath, const std::vector<std::wstring>& passwords,
                             bool& cancelled) {
    cancelled = false;
    if (passwords.empty()) return -1;
    const int total = static_cast<int>(passwords.size());
    auto line = [&](int i) {
        wchar_t buf[128];
        swprintf(buf, 128, tr(L"Trying saved passwords (%d/%d)…"), i, total);
        return std::wstring(buf);
    };
    ProgressState st;
    st.caption = tr(L"Password");
    st.text = line(1);
    int hit = -1;
    st.work = [&](ProgressState& s) {
        for (int i = 0; i < total; ++i) {
            if (s.cancel.load()) { cancelled = true; return; }
            s.setText(line(i + 1));
            s.setPos(static_cast<uint64_t>(i), static_cast<uint64_t>(total));
            if (testPassword(archivePath, passwords[static_cast<size_t>(i)]) == ArchiveStatus::Ok) { hit = i; return; }
        }
    };
    if (!runWithProgress(owner, st)) st.work(st); // 沒對話框就同步試，不能取消
    return hit;
}

// ---------------------------------------------------------------------------
// 新版本檢查與更新

namespace {

struct UpdateDlgState {
    const ReleaseInfo* release = nullptr;
    std::wstring current;
    HFONT bold = nullptr;
    HICON icon = nullptr;
};

INT_PTR CALLBACK updateProc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp) {
    auto* st = reinterpret_cast<UpdateDlgState*>(GetWindowLongPtrW(dlg, DWLP_USER));
    switch (msg) {
    case WM_INITDIALOG: {
        st = reinterpret_cast<UpdateDlgState*>(lp);
        SetWindowLongPtrW(dlg, DWLP_USER, lp);
        translateDialog(dlg);
        HINSTANCE inst = reinterpret_cast<HINSTANCE>(GetWindowLongPtrW(dlg, GWLP_HINSTANCE));
        int size = MulDiv(48, static_cast<int>(GetDpiForWindow(dlg)), 96);
        st->icon = static_cast<HICON>(LoadImageW(inst, MAKEINTRESOURCEW(IDI_APP), IMAGE_ICON, size, size, LR_DEFAULTCOLOR));
        if (st->icon) SendDlgItemMessageW(dlg, IDC_UPD_ICON, STM_SETICON, reinterpret_cast<WPARAM>(st->icon), 0);
        HFONT base = reinterpret_cast<HFONT>(SendMessageW(dlg, WM_GETFONT, 0, 0));
        LOGFONTW lf{};
        GetObjectW(base, sizeof(lf), &lf);
        lf.lfWeight = FW_BOLD;
        st->bold = CreateFontIndirectW(&lf);
        SendDlgItemMessageW(dlg, IDC_UPD_TITLE, WM_SETFONT, reinterpret_cast<WPARAM>(st->bold), TRUE);
        const wchar_t* f = tr(L"mpainter %s is available (you have %s). Would you like to install it now?");
        std::wstring text(wcslen(f) + st->release->version.size() + st->current.size() + 8, L'\0');
        int n = swprintf(text.data(), text.size(), f, st->release->version.c_str(), st->current.c_str());
        text.resize(n > 0 ? static_cast<size_t>(n) : 0);
        SetDlgItemTextW(dlg, IDC_UPD_TEXT, text.c_str());
        // EDIT 控制項要 \r\n 才換行；GitHub 的說明可能是 \n 或 \r\n。
        std::wstring notes;
        for (wchar_t c : st->release->notes) {
            if (c == L'\r') continue;
            if (c == L'\n') notes += L'\r';
            notes += c;
        }
        SetDlgItemTextW(dlg, IDC_UPD_NOTES, notes.empty() ? tr(L"(No release notes.)") : notes.c_str());
        SetForegroundWindow(dlg);
        SetFocus(GetDlgItem(dlg, IDC_UPD_INSTALL));
        return FALSE; // 焦點已自己設
    }
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDC_UPD_SKIP: EndDialog(dlg, static_cast<INT_PTR>(UpdateChoice::Skip)); return TRUE;
        case IDC_UPD_INSTALL: case IDOK: EndDialog(dlg, static_cast<INT_PTR>(UpdateChoice::Install)); return TRUE;
        case IDC_UPD_LATER: case IDCANCEL: EndDialog(dlg, static_cast<INT_PTR>(UpdateChoice::Later)); return TRUE;
        default: break;
        }
        break;
    case WM_DESTROY:
        if (st && st->bold) DeleteObject(st->bold);
        if (st && st->icon) DestroyIcon(st->icon);
        return TRUE;
    }
    return FALSE;
}

} // namespace

UpdateChoice showUpdateDialog(HWND owner, const ReleaseInfo& release, const std::wstring& currentVersion) {
    UpdateDlgState st;
    st.release = &release;
    st.current = currentVersion;
    HINSTANCE inst = reinterpret_cast<HINSTANCE>(GetWindowLongPtrW(owner, GWLP_HINSTANCE));
    INT_PTR r = DialogBoxParamW(inst, MAKEINTRESOURCEW(IDD_UPDATE), owner, updateProc, reinterpret_cast<LPARAM>(&st));
    if (r == static_cast<INT_PTR>(UpdateChoice::Skip)) return UpdateChoice::Skip;
    if (r == static_cast<INT_PTR>(UpdateChoice::Install)) return UpdateChoice::Install;
    return UpdateChoice::Later;
}

bool checkUpdateWithProgress(HWND owner, const std::wstring& url, ReleaseInfo& out, std::wstring& error, bool& cancelled) {
    ProgressState st;
    st.caption = tr(L"Software Update");
    st.text = tr(L"Checking for updates…");
    bool ok = false;
    std::wstring badResponse = tr(L"Unexpected response from the update server.");
    st.work = [&](ProgressState&) {
        std::string body;
        if (!httpGetText(url, body, error)) return;
        if (!parseLatestRelease(body, out)) { error = badResponse; return; }
        ok = true;
    };
    if (!runWithProgress(owner, st)) st.work(st);
    cancelled = st.cancel.load();
    return ok && !cancelled;
}

bool downloadUpdateWithProgress(HWND owner, const ReleaseInfo& release, const std::wstring& workDir, std::wstring& stagingDir,
                                std::wstring& error, bool& cancelled) {
    ProgressState st;
    st.caption = tr(L"Software Update");
    st.text = fmt1(L"Downloading mpainter %s…", release.version);
    // 背景執行緒不碰 tr 的格式化：訊息先在這裡備好。
    const std::wstring verifying = tr(L"Verifying the download…"), unpacking = tr(L"Unpacking the update…");
    const std::wstring damaged = tr(L"The downloaded file is damaged (checksum mismatch). Nothing was changed.");
    const std::wstring noExe = tr(L"The update package does not contain mpainter.exe. Nothing was changed.");
    std::wstring zip = workDir + L"\\" + packageZipName(release.version);
    std::wstring pkg = workDir + L"\\pkg";
    bool ok = false;
    std::wstring dlFail, unpackFail; // 英文細節，回主執行緒再套翻譯
    st.work = [&](ProgressState& s) {
        if (!httpDownload(release.zipUrl, zip, [&s](uint64_t done, uint64_t total) {
                if (s.cancel.load()) return false;
                s.setPos(done, total);
                return true;
            }, dlFail))
            return;
        if (s.cancel.load()) return;
        s.setText(verifying);
        std::string shaText;
        if (!httpGetText(release.shaUrl, shaText, dlFail)) return;
        std::string want = parseSha256Text(shaText), got = sha256OfFile(zip);
        if (want.empty() || got != want) { error = damaged; return; }
        if (s.cancel.load()) return;
        s.setText(unpacking);
        if (!extractAll(zip, pkg, unpackFail)) return;
        if (GetFileAttributesW(joinPath(pkg, kPackageExeName).c_str()) == INVALID_FILE_ATTRIBUTES) { error = noExe; return; }
        ok = true;
    };
    if (!runWithProgress(owner, st)) st.work(st);
    cancelled = !ok && st.cancel.load();
    if (cancelled) { error.clear(); return false; }
    if (!dlFail.empty()) error = fmt1(L"The download failed: %s", dlFail);
    else if (!unpackFail.empty()) error = fmt1(L"Could not unpack the update: %s", unpackFail);
    if (ok) stagingDir = pkg;
    return ok;
}

} // namespace mp
