#include "app/ImageDialogs.h"

#include <commctrl.h>
#include <windowsx.h>

#include <algorithm>
#include <cmath>
#include <string>

#include "app/DialogUtil.h"
#include "app/resource.h"
#include "core/Lang.h"

namespace mp {

namespace {

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

} // namespace

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

void showColorAdjustPanel(HWND owner, ColorAdjustHost* host) {
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

} // namespace mp
