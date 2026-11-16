#include "app/ImageDialogs.h"

#include "app/Theme.h"

#include <commctrl.h>
#include <windowsx.h>

#include <algorithm>
#include <cmath>
#include <string>

#include "app/DialogUtil.h"
#include "app/resource.h"
#include "core/Lang.h"
#include "core/ResizeModel.h"

namespace mp {

namespace {

// ---------------------------------------------------------------------------
// 調整尺寸

// 對話框的狀態：算法在 core/ResizeModel，這裡只有「正在由程式改欄位」的旗標與結果。
struct ResizeState {
    ResizeModel model;
    ResizeResult out;
    bool updating = false; // 程式在改欄位：這時的 EN_CHANGE 不是使用者打的
};

// 像素顯示整數，其他單位三位有效數字。
std::wstring fmtNum(double v, bool integer) {
    wchar_t buf[32];
    if (integer) swprintf(buf, 32, L"%d", static_cast<int>(std::lround(v)));
    else swprintf(buf, 32, L"%.3g", v);
    return buf;
}

double readNum(HWND dlg, int id) {
    wchar_t buf[32];
    GetDlgItemTextW(dlg, id, buf, 32);
    return _wtof(buf);
}

bool checked(HWND dlg, int id) { return Button_GetCheck(GetDlgItem(dlg, id)) == BST_CHECKED; }

// 把 model 的值寫回欄位。fields：要寫哪幾個欄位（使用者正在打的那一格不要動）。「結果」那一行一定更新。
enum : unsigned { kFieldW = 1, kFieldH = 2, kFieldDpi = 4, kAllFields = kFieldW | kFieldH | kFieldDpi };
void resizeSync(HWND dlg, ResizeState& s, unsigned fields = kAllFields) {
    const ResizeModel& m = s.model;
    const bool pixels = m.unit == LengthUnit::Pixels;
    s.updating = true;
    if (fields & kFieldW) SetDlgItemTextW(dlg, IDC_W, fmtNum(m.widthInUnit(), pixels).c_str());
    if (fields & kFieldH) SetDlgItemTextW(dlg, IDC_H, fmtNum(m.heightInUnit(), pixels).c_str());
    if (fields & kFieldDpi) SetDlgItemTextW(dlg, IDC_DPI, fmtNum(m.dpiInUnit(), m.dpiUnit == DpiUnit::PerInch).c_str());
    wchar_t buf[64];
    swprintf(buf, 64, tr(L"%d × %d pixels"), m.previewWidth(), m.previewHeight());
    SetDlgItemTextW(dlg, IDC_RESULT, buf);
    s.updating = false;
}

void resizeInit(HWND dlg, ResizeState& s) {
    translateDialog(dlg);
    HWND fit = GetDlgItem(dlg, IDC_FIT);
    ComboBox_AddString(fit, tr(L"Custom")); // 第 0 項；之後依序是 kResizePresets
    for (int preset : kResizePresets) {
        wchar_t b[32];
        swprintf(b, 32, L"%d × %d", preset, preset);
        ComboBox_AddString(fit, b);
    }
    ComboBox_SetCurSel(fit, 0);
    HWND unit = GetDlgItem(dlg, IDC_UNIT); // 順序同 LengthUnit
    for (auto* u : {tr(L"pixels"), L"%", tr(L"inches"), tr(L"cm"), tr(L"mm"), tr(L"points")}) ComboBox_AddString(unit, u);
    ComboBox_SetCurSel(unit, 0);
    HWND du = GetDlgItem(dlg, IDC_DPIUNIT); // 順序同 DpiUnit
    ComboBox_AddString(du, tr(L"pixels/inch"));
    ComboBox_AddString(du, tr(L"pixels/cm"));
    ComboBox_SetCurSel(du, 0);
    Button_SetCheck(GetDlgItem(dlg, IDC_LOCK), BST_CHECKED);
    Button_SetCheck(GetDlgItem(dlg, IDC_RESAMPLE), BST_CHECKED);
    resizeSync(dlg, s);
}

// 一個控制項的通知。回傳有沒有處理。
bool resizeCommand(HWND dlg, ResizeState& s, int id, int code) {
    ResizeModel& m = s.model;
    switch (id) {
    case IDC_FIT:
        if (code == CBN_SELCHANGE) {
            const int sel = ComboBox_GetCurSel(GetDlgItem(dlg, IDC_FIT));
            if (sel > 0) {
                m.fitInto(kResizePresets[sel - 1]);
                resizeSync(dlg, s);
            }
        }
        return true;
    case IDC_W:
    case IDC_H:
        if (code == EN_CHANGE) {
            const double v = readNum(dlg, id);
            if (v <= 0) return true;
            if (id == IDC_W) m.setWidth(v); else m.setHeight(v);
            ComboBox_SetCurSel(GetDlgItem(dlg, IDC_FIT), 0); // 自己打了數字：不再是預設尺寸
            resizeSync(dlg, s, id == IDC_W ? kFieldH : kFieldW);
        }
        return true;
    case IDC_UNIT:
        if (code == CBN_SELCHANGE) {
            m.unit = static_cast<LengthUnit>(ComboBox_GetCurSel(GetDlgItem(dlg, IDC_UNIT)));
            resizeSync(dlg, s);
        }
        return true;
    case IDC_LOCK:
        m.lock = checked(dlg, IDC_LOCK);
        return true;
    case IDC_DPI:
        if (code == EN_CHANGE) {
            const double v = readNum(dlg, IDC_DPI);
            if (v <= 0) return true;
            m.setDpi(v);
            resizeSync(dlg, s, kFieldW | kFieldH);
        }
        return true;
    case IDC_DPIUNIT:
        if (code == CBN_SELCHANGE) {
            m.dpiUnit = static_cast<DpiUnit>(ComboBox_GetCurSel(GetDlgItem(dlg, IDC_DPIUNIT)));
            resizeSync(dlg, s);
        }
        return true;
    case IDC_RESAMPLE: {
        m.setResample(checked(dlg, IDC_RESAMPLE));
        for (int field : {IDC_W, IDC_H, IDC_FIT}) EnableWindow(GetDlgItem(dlg, field), m.resample);
        resizeSync(dlg, s);
        return true;
    }
    case IDOK:
        s.out.resample = m.resample;
        s.out.width = m.resultWidth();
        s.out.height = m.resultHeight();
        s.out.dpi = m.dpi;
        EndDialog(dlg, IDOK);
        return true;
    case IDCANCEL:
        EndDialog(dlg, IDCANCEL);
        return true;
    default: return false;
    }
}

INT_PTR CALLBACK resizeProc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp) {
    auto* s = reinterpret_cast<ResizeState*>(GetWindowLongPtrW(dlg, DWLP_USER));
    switch (msg) {
    case WM_INITDIALOG:
        SetWindowLongPtrW(dlg, DWLP_USER, lp);
        resizeInit(dlg, *reinterpret_cast<ResizeState*>(lp));
        return TRUE;
    case WM_COMMAND:
        if (!s || s->updating) return FALSE;
        return resizeCommand(dlg, *s, LOWORD(wp), HIWORD(wp)) ? TRUE : FALSE;
    default: return FALSE;
    }
}

// ---------------------------------------------------------------------------
// 調整顏色

} // namespace

// 面板的狀態。由 ColorAdjustPanel 握著，對話框關掉後還在（hwnd 變回 nullptr）。
struct ColorPanelState {
    HWND hwnd = nullptr;
    HWND owner = nullptr;
    ColorAdjustHost* host = nullptr;
    ColorAdjustParams params;
    std::array<uint32_t, 256> hist{};
    bool updating = false;
    bool done = false;
};

namespace {

using ColorPanel = ColorPanelState;

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
    auto* p = reinterpret_cast<ColorPanel*>(GetWindowLongPtrW(dlg, DWLP_USER)); // WM_INITDIALOG 之前是 nullptr
    switch (msg) {
    case WM_INITDIALOG:
        p = reinterpret_cast<ColorPanel*>(lp);
        SetWindowLongPtrW(dlg, DWLP_USER, lp);
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
        if (p) p->hwnd = nullptr; // 狀態留著，由 ColorAdjustPanel 放掉
        SetWindowLongPtrW(dlg, DWLP_USER, 0);
        return TRUE;
    default: return FALSE;
    }
}

} // namespace

bool showResizeDialog(HWND owner, int curW, int curH, double curDpi, ResizeResult& out) {
    ResizeState s;
    s.model = ResizeModel::forImage(curW, curH, curDpi);
    HINSTANCE inst = reinterpret_cast<HINSTANCE>(GetWindowLongPtrW(owner, GWLP_HINSTANCE));
    INT_PTR r = DialogBoxParamW(inst, MAKEINTRESOURCEW(IDD_RESIZE), owner, resizeProc, reinterpret_cast<LPARAM>(&s));
    if (r != IDOK) return false;
    out = s.out;
    return true;
}

ColorAdjustPanel::ColorAdjustPanel() = default;

ColorAdjustPanel::~ColorAdjustPanel() { close(); }

HWND ColorAdjustPanel::hwnd() const { return state_ ? state_->hwnd : nullptr; }

void ColorAdjustPanel::show(HWND owner, ColorAdjustHost* host) {
    if (isOpen()) { SetForegroundWindow(state_->hwnd); return; }
    state_ = std::make_unique<ColorPanelState>();
    state_->owner = owner;
    state_->host = host;
    HINSTANCE inst = reinterpret_cast<HINSTANCE>(GetWindowLongPtrW(owner, GWLP_HINSTANCE));
    HWND dlg = CreateDialogParamW(inst, MAKEINTRESOURCEW(IDD_COLOR), owner, colorProc, reinterpret_cast<LPARAM>(state_.get()));
    if (!dlg) { state_.reset(); host->cancelAdjust(); return; }
    // 放在主視窗右上角旁邊
    RECT orc, drc;
    GetWindowRect(owner, &orc);
    GetWindowRect(dlg, &drc);
    int w = drc.right - drc.left, h = drc.bottom - drc.top;
    const UiScale px = UiScale::forDpi(GetDpiForWindow(owner));
    int x = orc.right - w - px.round(40), y = orc.top + px.round(120);
    MONITORINFO mi{sizeof(mi)};
    GetMonitorInfoW(MonitorFromWindow(owner, MONITOR_DEFAULTTONEAREST), &mi);
    x = std::clamp<int>(x, mi.rcWork.left, std::max<int>(mi.rcWork.left, mi.rcWork.right - w));
    y = std::clamp<int>(y, mi.rcWork.top, std::max<int>(mi.rcWork.top, mi.rcWork.bottom - h));
    SetWindowPos(dlg, nullptr, x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER);
    ShowWindow(dlg, SW_SHOW);
}

void ColorAdjustPanel::setParams(const ColorAdjustParams& p) {
    if (!isOpen()) return;
    state_->params = p;
    slidersFromParams(*state_);
    InvalidateRect(GetDlgItem(state_->hwnd, IDC_HIST), nullptr, FALSE);
}

void ColorAdjustPanel::close() {
    if (!isOpen()) return;
    state_->done = true;
    DestroyWindow(state_->hwnd);
}

} // namespace mp
