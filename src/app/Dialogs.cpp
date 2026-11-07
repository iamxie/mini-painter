#include "app/Dialogs.h"

#include <commctrl.h>
#include <shobjidl.h>
#include <windowsx.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <string>
#include <thread>

#include "app/resource.h"
#include "app/version.h"
#include "core/Lang.h"
#include "core/Metadata.h"
#include "core/TempDir.h"

namespace mp {

namespace {

// ---------------------------------------------------------------------------
// 對話框比螢幕工作區高時的備案：縮到工作區高度，加垂直捲軸捲內容。
// 1366×768 @125% 或 1920×1080 @175% 的筆電會遇到（設定、調整顏色都超過 600 邏輯像素）。

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
// 設定：PropertySheet，四個 tab（一般 / 模式 / 壓縮檔 / Windows 整合），底部 取消 / 確定 / 套用。
// 每個 tab 一張英文對話框範本，WM_INITDIALOG 時翻譯。按「套用」或「確定」時每個已建立的頁面收到 PSN_APPLY，
// 各自把控制項讀回共用的 Settings 副本；最後一頁收完才呼叫 info.onApply（沒打開過的頁面沒建立，值維持原樣）。

// 選資料夾（IFileOpenDialog 的挑資料夾模式）。取消回傳空字串。
std::wstring browseFolder(HWND owner, const std::wstring& initial) {
    std::wstring out;
    IFileOpenDialog* fd = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&fd)))) return out;
    DWORD opts = 0;
    fd->GetOptions(&opts);
    fd->SetOptions(opts | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
    if (!initial.empty()) {
        IShellItem* si = nullptr;
        if (SUCCEEDED(SHCreateItemFromParsingName(initial.c_str(), nullptr, IID_PPV_ARGS(&si)))) {
            fd->SetFolder(si);
            si->Release();
        }
    }
    if (SUCCEEDED(fd->Show(owner))) {
        IShellItem* item = nullptr;
        if (SUCCEEDED(fd->GetResult(&item))) {
            PWSTR p = nullptr;
            if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &p))) {
                out = p;
                CoTaskMemFree(p);
            }
            item->Release();
        }
    }
    fd->Release();
    return out;
}

// 多行編輯框 <-> 清單：一行一項，空行略過。trimSpaces 為 true 時前後空白去掉（密碼框不去：空白是密碼的一部分）。
std::wstring joinLines(const std::vector<std::wstring>& items) {
    std::wstring out;
    for (const auto& it : items) out += (out.empty() ? L"" : L"\r\n") + it;
    return out;
}

std::vector<std::wstring> readLines(HWND dlg, int id, bool trimSpaces) {
    HWND h = GetDlgItem(dlg, id);
    int len = GetWindowTextLengthW(h);
    std::wstring text(static_cast<size_t>(len) + 1, L'\0');
    GetWindowTextW(h, text.data(), len + 1);
    text.resize(static_cast<size_t>(len));
    std::vector<std::wstring> out;
    size_t pos = 0;
    while (pos <= text.size()) {
        size_t nl = text.find(L'\n', pos);
        std::wstring line = text.substr(pos, nl == std::wstring::npos ? std::wstring::npos : nl - pos);
        pos = nl == std::wstring::npos ? text.size() + 1 : nl + 1;
        const wchar_t* ws = trimSpaces ? L" \t\r" : L"\r";
        size_t a = line.find_first_not_of(ws), b = line.find_last_not_of(ws);
        if (a == std::wstring::npos) continue;
        out.push_back(line.substr(a, b - a + 1));
    }
    return out;
}

std::wstring trimWs(const std::wstring& v) {
    size_t a = v.find_first_not_of(L" \t"), b = v.find_last_not_of(L" \t");
    return a == std::wstring::npos ? L"" : v.substr(a, b - a + 1);
}

std::wstring dlgText(HWND dlg, int id) {
    HWND h = GetDlgItem(dlg, id);
    int len = GetWindowTextLengthW(h);
    std::wstring text(static_cast<size_t>(len) + 1, L'\0');
    GetWindowTextW(h, text.data(), len + 1);
    text.resize(static_cast<size_t>(len));
    return text;
}

std::wstring fmt1(const wchar_t* key, const std::wstring& a) {
    const wchar_t* f = tr(key);
    std::wstring out(wcslen(f) + a.size() + 8, L'\0');
    int n = swprintf(out.data(), out.size(), f, a.c_str());
    out.resize(n > 0 ? static_cast<size_t>(n) : 0);
    return out;
}

std::wstring fmt2(const wchar_t* key, const std::wstring& a, const std::wstring& b) {
    const wchar_t* f = tr(key);
    std::wstring out(wcslen(f) + a.size() + b.size() + 8, L'\0');
    int n = swprintf(out.data(), out.size(), f, a.c_str(), b.c_str());
    out.resize(n > 0 ? static_cast<size_t>(n) : 0);
    return out;
}

struct SheetState {
    Settings s;                       // 工作副本：套用時整份交給 onApply
    const SettingsDialogInfo* info = nullptr;
    int created = 0;                  // 已建立的頁面數（PSN_APPLY 會送給每一頁，收齊才 onApply）
    int applied = 0;
    int selProfile = -1;              // 模式頁：清單目前選的那個（s.profiles 的索引）
    bool loadingProfile = false;      // 模式頁：正在把模式填進控制項（忽略這期間的變更通知）
};

void markChanged(HWND page) { PropSheet_Changed(GetParent(page), page); }

// 每一頁的 PSN_APPLY 走這裡：收齊所有已建立的頁面後才真的套用。
void pageApplied(SheetState& st) {
    if (++st.applied < st.created) return;
    st.applied = 0;
    if (st.info->onApply) st.info->onApply(st.s);
}

// ----- 一般 -----

void updateTipState(HWND dlg, const SheetState& st) {
    wchar_t buf[256];
    if (st.s.tipShown >= kTipMaxShown) swprintf(buf, 256, tr(L"Shown %d times; it will not appear again."), kTipMaxShown);
    else swprintf(buf, 256, tr(L"Hint shown when the mouse rests on an annotation. Will be shown %d more times."), kTipMaxShown - st.s.tipShown);
    SetDlgItemTextW(dlg, IDC_TIPSTATE, buf);
}

INT_PTR CALLBACK generalPageProc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp) {
    auto* st = reinterpret_cast<SheetState*>(GetWindowLongPtrW(dlg, DWLP_USER));
    switch (msg) {
    case WM_INITDIALOG: {
        st = reinterpret_cast<SheetState*>(reinterpret_cast<PROPSHEETPAGEW*>(lp)->lParam);
        SetWindowLongPtrW(dlg, DWLP_USER, reinterpret_cast<LONG_PTR>(st));
        ++st->created;
        translateDialog(dlg);
        HWND combo = GetDlgItem(dlg, IDC_LANGUAGE);
        SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(tr(L"Follow System")));
        int sel = 0;
        for (size_t i = 0; i < st->info->languages.size(); ++i) {
            SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(st->info->languages[i].second.c_str()));
            if (!st->s.language.empty() && _wcsicmp(st->info->languages[i].first.c_str(), st->s.language.c_str()) == 0)
                sel = static_cast<int>(i) + 1;
        }
        SendMessageW(combo, CB_SETCURSEL, sel, 0);
        SetDlgItemTextW(dlg, IDC_INIPATH, fmt1(L"Settings file: %s", st->info->iniPath).c_str());
        SetDlgItemTextW(dlg, IDC_INIWARN, st->info->iniWritable ? L"" : tr(L"⚠ The settings file cannot be written. Changes here last only for this run; the next start returns to defaults."));
        updateTipState(dlg, *st);
        return TRUE;
    }
    case WM_COMMAND:
        if (LOWORD(wp) == IDC_RESETTIP) {
            st->s.tipShown = 0;
            updateTipState(dlg, *st);
            markChanged(dlg);
            return TRUE;
        }
        if (LOWORD(wp) == IDC_LANGUAGE && HIWORD(wp) == CBN_SELCHANGE) { markChanged(dlg); return TRUE; }
        break;
    case WM_NOTIFY:
        if (reinterpret_cast<NMHDR*>(lp)->code == PSN_APPLY) {
            int sel = static_cast<int>(SendDlgItemMessageW(dlg, IDC_LANGUAGE, CB_GETCURSEL, 0, 0));
            st->s.language = sel > 0 && static_cast<size_t>(sel - 1) < st->info->languages.size() ? st->info->languages[static_cast<size_t>(sel - 1)].first : L"";
            pageApplied(*st);
            SetWindowLongPtrW(dlg, DWLP_MSGRESULT, PSNRET_NOERROR);
            return TRUE;
        }
        break;
    }
    return FALSE;
}

// ----- 模式 -----

const wchar_t* const kViewModeKeys[] = {L"fitwindow", L"fitwidth", L"fitheight", L"actual", L"twopage"};
const wchar_t* const kViewModeNames[] = {L"Fit to Window", L"Fit to Width", L"Fit to Height", L"Actual Size", L"Two Pages Side by Side"};

// 名字對話框（新增 / 複製 / 重新命名）。回傳 false 表示取消。
struct NameState { std::wstring caption, value; };

INT_PTR CALLBACK nameProc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp) {
    auto* st = reinterpret_cast<NameState*>(GetWindowLongPtrW(dlg, DWLP_USER));
    switch (msg) {
    case WM_INITDIALOG:
        st = reinterpret_cast<NameState*>(lp);
        SetWindowLongPtrW(dlg, DWLP_USER, lp);
        translateDialog(dlg);
        SetWindowTextW(dlg, st->caption.c_str());
        SendDlgItemMessageW(dlg, IDC_NAME_EDIT, EM_SETLIMITTEXT, 64, 0);
        SetDlgItemTextW(dlg, IDC_NAME_EDIT, st->value.c_str());
        SendDlgItemMessageW(dlg, IDC_NAME_EDIT, EM_SETSEL, 0, -1);
        SetFocus(GetDlgItem(dlg, IDC_NAME_EDIT));
        return FALSE;
    case WM_COMMAND:
        if (LOWORD(wp) == IDOK) {
            st->value = trimWs(dlgText(dlg, IDC_NAME_EDIT));
            if (st->value.empty()) return TRUE; // 空名字不收
            EndDialog(dlg, IDOK);
            return TRUE;
        }
        if (LOWORD(wp) == IDCANCEL) { EndDialog(dlg, IDCANCEL); return TRUE; }
        break;
    }
    return FALSE;
}

bool askName(HWND owner, const wchar_t* caption, std::wstring& value) {
    NameState st;
    st.caption = caption;
    st.value = value;
    HINSTANCE inst = reinterpret_cast<HINSTANCE>(GetWindowLongPtrW(owner, GWLP_HINSTANCE));
    if (DialogBoxParamW(inst, MAKEINTRESOURCEW(IDD_NAME), owner, nameProc, reinterpret_cast<LPARAM>(&st)) != IDOK) return false;
    value = st.value;
    return true;
}

// 問一個不重複的名字。exceptIndex 是重新命名時自己的索引（和自己同名不算重複）。
bool askUniqueName(HWND page, SheetState& st, const wchar_t* caption, std::wstring& name, int exceptIndex) {
    for (;;) {
        if (!askName(page, caption, name)) return false;
        bool dup = false;
        for (size_t i = 0; i < st.s.profiles.size(); ++i)
            if (static_cast<int>(i) != exceptIndex && _wcsicmp(st.s.profiles[i].name.c_str(), name.c_str()) == 0) dup = true;
        if (!dup) return true;
        MessageBoxW(page, tr(L"A mode with this name already exists."), tr(L"Settings"), MB_OK | MB_ICONINFORMATION);
    }
}

void fillProfileList(HWND dlg, SheetState& st) {
    HWND list = GetDlgItem(dlg, IDC_PROFILES);
    SendMessageW(list, LB_RESETCONTENT, 0, 0);
    for (const auto& p : st.s.profiles) SendMessageW(list, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(p.name.c_str()));
    if (st.selProfile < 0 || st.selProfile >= static_cast<int>(st.s.profiles.size())) st.selProfile = 0;
    SendMessageW(list, LB_SETCURSEL, st.selProfile, 0);
    SetDlgItemTextW(dlg, IDC_PROF_INUSE, fmt1(L"In use: %s", st.s.profileName).c_str());
}

void loadProfileControls(HWND dlg, SheetState& st) {
    if (st.selProfile < 0 || st.selProfile >= static_cast<int>(st.s.profiles.size())) return;
    const Profile& p = st.s.profiles[static_cast<size_t>(st.selProfile)];
    st.loadingProfile = true;
    SetDlgItemTextW(dlg, IDC_PROF_GRP, p.name.c_str());
    int vm = 0;
    for (int i = 0; i < 5; ++i) if (p.viewMode == kViewModeKeys[i]) vm = i;
    SendDlgItemMessageW(dlg, IDC_PROF_VIEWMODE, CB_SETCURSEL, vm, 0);
    CheckDlgButton(dlg, IDC_PROF_UPSCALE, p.upscaleSmall ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(dlg, IDC_PROF_HQ, p.highQuality ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(dlg, IDC_PROF_SCROLLFLIP, p.scrollFlip ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(dlg, IDC_PROF_AUTONEXT, p.autoNextBook ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(dlg, IDC_PROF_RTL, p.rightToLeft ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(dlg, IDC_PROF_FIRSTALONE, p.firstPageAlone ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(dlg, IDC_PROF_WARNMETA, p.warnMetadataLoss ? BST_CHECKED : BST_UNCHECKED);
    bool inUse = _wcsicmp(p.name.c_str(), st.s.profileName.c_str()) == 0;
    EnableWindow(GetDlgItem(dlg, IDC_PROF_USE), !inUse);
    EnableWindow(GetDlgItem(dlg, IDC_PROF_DELETE), !inUse && st.s.profiles.size() > 1);
    st.loadingProfile = false;
}

void storeProfileControls(HWND dlg, SheetState& st) {
    if (st.selProfile < 0 || st.selProfile >= static_cast<int>(st.s.profiles.size())) return;
    Profile& p = st.s.profiles[static_cast<size_t>(st.selProfile)];
    int vm = static_cast<int>(SendDlgItemMessageW(dlg, IDC_PROF_VIEWMODE, CB_GETCURSEL, 0, 0));
    if (vm >= 0 && vm < 5) p.viewMode = kViewModeKeys[vm];
    p.upscaleSmall = IsDlgButtonChecked(dlg, IDC_PROF_UPSCALE) == BST_CHECKED;
    p.highQuality = IsDlgButtonChecked(dlg, IDC_PROF_HQ) == BST_CHECKED;
    p.scrollFlip = IsDlgButtonChecked(dlg, IDC_PROF_SCROLLFLIP) == BST_CHECKED;
    p.autoNextBook = IsDlgButtonChecked(dlg, IDC_PROF_AUTONEXT) == BST_CHECKED;
    p.rightToLeft = IsDlgButtonChecked(dlg, IDC_PROF_RTL) == BST_CHECKED;
    p.firstPageAlone = IsDlgButtonChecked(dlg, IDC_PROF_FIRSTALONE) == BST_CHECKED;
    p.warnMetadataLoss = IsDlgButtonChecked(dlg, IDC_PROF_WARNMETA) == BST_CHECKED;
}

INT_PTR CALLBACK modesPageProc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp) {
    auto* st = reinterpret_cast<SheetState*>(GetWindowLongPtrW(dlg, DWLP_USER));
    switch (msg) {
    case WM_INITDIALOG: {
        st = reinterpret_cast<SheetState*>(reinterpret_cast<PROPSHEETPAGEW*>(lp)->lParam);
        SetWindowLongPtrW(dlg, DWLP_USER, reinterpret_cast<LONG_PTR>(st));
        ++st->created;
        translateDialog(dlg);
        for (auto* n : kViewModeNames) SendDlgItemMessageW(dlg, IDC_PROF_VIEWMODE, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(tr(n)));
        // 一開始選到正在使用的那個
        for (size_t i = 0; i < st->s.profiles.size(); ++i)
            if (_wcsicmp(st->s.profiles[i].name.c_str(), st->s.profileName.c_str()) == 0) st->selProfile = static_cast<int>(i);
        fillProfileList(dlg, *st);
        loadProfileControls(dlg, *st);
        return TRUE;
    }
    case WM_COMMAND: {
        int id = LOWORD(wp), code = HIWORD(wp);
        if (id == IDC_PROFILES && code == LBN_SELCHANGE) {
            storeProfileControls(dlg, *st);
            st->selProfile = static_cast<int>(SendDlgItemMessageW(dlg, IDC_PROFILES, LB_GETCURSEL, 0, 0));
            loadProfileControls(dlg, *st);
            return TRUE;
        }
        if (id == IDC_PROF_ADD || id == IDC_PROF_DUP) {
            storeProfileControls(dlg, *st);
            std::wstring name;
            if (!askUniqueName(dlg, *st, id == IDC_PROF_ADD ? tr(L"New Mode") : tr(L"Copy Mode"), name, -1)) return TRUE;
            Profile p = id == IDC_PROF_DUP && st->selProfile >= 0 ? st->s.profiles[static_cast<size_t>(st->selProfile)] : Profile{};
            p.name = name;
            st->s.profiles.push_back(p);
            st->selProfile = static_cast<int>(st->s.profiles.size()) - 1;
            fillProfileList(dlg, *st);
            loadProfileControls(dlg, *st);
            markChanged(dlg);
            return TRUE;
        }
        if (id == IDC_PROF_RENAME && st->selProfile >= 0) {
            storeProfileControls(dlg, *st);
            Profile& p = st->s.profiles[static_cast<size_t>(st->selProfile)];
            std::wstring name = p.name;
            if (!askUniqueName(dlg, *st, tr(L"Rename Mode"), name, st->selProfile)) return TRUE;
            if (_wcsicmp(p.name.c_str(), st->s.profileName.c_str()) == 0) st->s.profileName = name;
            p.name = name;
            fillProfileList(dlg, *st);
            loadProfileControls(dlg, *st);
            markChanged(dlg);
            return TRUE;
        }
        if (id == IDC_PROF_DELETE && st->selProfile >= 0) {
            const Profile& p = st->s.profiles[static_cast<size_t>(st->selProfile)];
            if (_wcsicmp(p.name.c_str(), st->s.profileName.c_str()) == 0) {
                MessageBoxW(dlg, tr(L"This mode is in use. Switch to another mode first."), tr(L"Settings"), MB_OK | MB_ICONINFORMATION);
                return TRUE;
            }
            if (st->s.profiles.size() <= 1) {
                MessageBoxW(dlg, tr(L"At least one mode must remain."), tr(L"Settings"), MB_OK | MB_ICONINFORMATION);
                return TRUE;
            }
            st->s.profiles.erase(st->s.profiles.begin() + st->selProfile);
            if (st->selProfile >= static_cast<int>(st->s.profiles.size())) st->selProfile = static_cast<int>(st->s.profiles.size()) - 1;
            fillProfileList(dlg, *st);
            loadProfileControls(dlg, *st);
            markChanged(dlg);
            return TRUE;
        }
        if (id == IDC_PROF_USE && st->selProfile >= 0) {
            storeProfileControls(dlg, *st);
            st->s.profileName = st->s.profiles[static_cast<size_t>(st->selProfile)].name;
            fillProfileList(dlg, *st);
            loadProfileControls(dlg, *st);
            markChanged(dlg);
            return TRUE;
        }
        bool pref = id == IDC_PROF_UPSCALE || id == IDC_PROF_HQ || id == IDC_PROF_SCROLLFLIP || id == IDC_PROF_AUTONEXT ||
                    id == IDC_PROF_RTL || id == IDC_PROF_FIRSTALONE || id == IDC_PROF_WARNMETA;
        if ((pref && code == BN_CLICKED) || (id == IDC_PROF_VIEWMODE && code == CBN_SELCHANGE)) {
            if (!st->loadingProfile) markChanged(dlg);
            return TRUE;
        }
        break;
    }
    case WM_NOTIFY:
        if (reinterpret_cast<NMHDR*>(lp)->code == PSN_APPLY) {
            storeProfileControls(dlg, *st);
            pageApplied(*st);
            SetWindowLongPtrW(dlg, DWLP_MSGRESULT, PSNRET_NOERROR);
            return TRUE;
        }
        break;
    }
    return FALSE;
}

// ----- 壓縮檔 -----

void updateUnzipState(HWND dlg, const std::wstring& root) {
    SetDlgItemTextW(dlg, IDC_UNZIPSTATE, fmt2(L"In use: %s (%s)", root, formatFileSize(directorySize(root))).c_str());
}

INT_PTR CALLBACK archivePageProc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp) {
    auto* st = reinterpret_cast<SheetState*>(GetWindowLongPtrW(dlg, DWLP_USER));
    switch (msg) {
    case WM_INITDIALOG: {
        st = reinterpret_cast<SheetState*>(reinterpret_cast<PROPSHEETPAGEW*>(lp)->lParam);
        SetWindowLongPtrW(dlg, DWLP_USER, reinterpret_cast<LONG_PTR>(st));
        ++st->created;
        translateDialog(dlg);
        SetDlgItemTextW(dlg, IDC_UNZIPDIR, st->s.unzipDir.c_str());
        SetDlgItemTextW(dlg, IDC_PINNED, joinLines(st->s.pinnedNames).c_str());
        SetDlgItemTextW(dlg, IDC_PASSWORDS, joinLines(st->s.passwords).c_str());
        updateUnzipState(dlg, st->info->unzipRootInUse);
        return TRUE;
    }
    case WM_COMMAND: {
        int id = LOWORD(wp), code = HIWORD(wp);
        if (id == IDC_UNZIPBROWSE) {
            std::wstring picked = browseFolder(dlg, dlgText(dlg, IDC_UNZIPDIR));
            if (!picked.empty()) SetDlgItemTextW(dlg, IDC_UNZIPDIR, picked.c_str()); // EN_CHANGE 會標記變更
            return TRUE;
        }
        if ((id == IDC_UNZIPDIR || id == IDC_PINNED || id == IDC_PASSWORDS) && code == EN_CHANGE) { markChanged(dlg); return TRUE; }
        break;
    }
    case WM_NOTIFY:
        if (reinterpret_cast<NMHDR*>(lp)->code == PSN_APPLY) {
            st->s.unzipDir = trimWs(dlgText(dlg, IDC_UNZIPDIR));
            st->s.pinnedNames = readLines(dlg, IDC_PINNED, true);
            st->s.passwords = readLines(dlg, IDC_PASSWORDS, false);
            pageApplied(*st);
            updateUnzipState(dlg, resolveUnzipRoot(st->s.unzipDir, st->info->exeDir));
            SetWindowLongPtrW(dlg, DWLP_MSGRESULT, PSNRET_NOERROR);
            return TRUE;
        }
        break;
    }
    return FALSE;
}

// ----- Windows 整合 -----

INT_PTR CALLBACK windowsPageProc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp) {
    auto* st = reinterpret_cast<SheetState*>(GetWindowLongPtrW(dlg, DWLP_USER));
    switch (msg) {
    case WM_INITDIALOG: {
        st = reinterpret_cast<SheetState*>(reinterpret_cast<PROPSHEETPAGEW*>(lp)->lParam);
        SetWindowLongPtrW(dlg, DWLP_USER, reinterpret_cast<LONG_PTR>(st));
        ++st->created;
        translateDialog(dlg);
        CheckDlgButton(dlg, IDC_CTXMENU, st->s.contextMenu ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(dlg, IDC_ASKDEFAULT, st->s.askedDefault ? BST_UNCHECKED : BST_CHECKED);
        SetDlgItemTextW(dlg, IDC_CTXNOTE, st->info->isWin11
            ? tr(L"Windows 11: this item is under “Show more options” in the context menu (or press Shift+F10).")
            : L"");
        SetDlgItemTextW(dlg, IDC_DEFNOTE, st->info->isWin11
            ? tr(L"Windows does not let apps set defaults themselves. Opens the system Settings page; on Windows 11 pick each file type one by one.")
            : tr(L"Windows does not let apps set defaults themselves. Opens the system Settings page; choose mpainter there."));
        return TRUE;
    }
    case WM_COMMAND: {
        int id = LOWORD(wp), code = HIWORD(wp);
        if (id == IDC_SETDEFAULT) {
            if (st->info->onSetDefault) st->info->onSetDefault();
            return TRUE;
        }
        if ((id == IDC_CTXMENU || id == IDC_ASKDEFAULT) && code == BN_CLICKED) { markChanged(dlg); return TRUE; }
        break;
    }
    case WM_NOTIFY:
        if (reinterpret_cast<NMHDR*>(lp)->code == PSN_APPLY) {
            st->s.contextMenu = IsDlgButtonChecked(dlg, IDC_CTXMENU) == BST_CHECKED;
            st->s.askedDefault = IsDlgButtonChecked(dlg, IDC_ASKDEFAULT) != BST_CHECKED;
            pageApplied(*st);
            SetWindowLongPtrW(dlg, DWLP_MSGRESULT, PSNRET_NOERROR);
            return TRUE;
        }
        break;
    }
    return FALSE;
}

// 底部三顆按鈕的文字跟語系走（PropertySheet 預設是系統語言的 OK / Cancel / Apply）。
int CALLBACK sheetCallback(HWND sheet, UINT msg, LPARAM) {
    if (msg == PSCB_INITIALIZED) {
        SetDlgItemTextW(sheet, IDOK, tr(L"OK"));
        SetDlgItemTextW(sheet, IDCANCEL, tr(L"Cancel"));
        SetDlgItemTextW(sheet, 0x3021 /* ID_APPLY_NOW */, tr(L"Apply"));
    }
    return 0;
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
        return TRUE;
    }
    case WM_COMMAND:
        if (LOWORD(wp) == IDOK || LOWORD(wp) == IDCANCEL) { EndDialog(dlg, IDOK); return TRUE; }
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

void showAboutDialog(HWND owner) {
    HINSTANCE inst = reinterpret_cast<HINSTANCE>(GetWindowLongPtrW(owner, GWLP_HINSTANCE));
    DialogBoxParamW(inst, MAKEINTRESOURCEW(IDD_ABOUT), owner, aboutProc, 0);
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

void showSettingsDialog(HWND owner, const Settings& s, const SettingsDialogInfo& info) {
    SheetState st;
    st.s = s;
    st.info = &info;
    HINSTANCE inst = reinterpret_cast<HINSTANCE>(GetWindowLongPtrW(owner, GWLP_HINSTANCE));
    struct PageDef { int id; DLGPROC proc; const wchar_t* title; };
    const PageDef defs[] = {
        {IDD_SET_GENERAL, generalPageProc, L"General"},
        {IDD_SET_MODES, modesPageProc, L"Modes"},
        {IDD_SET_ARCHIVE, archivePageProc, L"Archives"},
        {IDD_SET_WINDOWS, windowsPageProc, L"Windows Integration"},
    };
    PROPSHEETPAGEW pages[4] = {};
    for (int i = 0; i < 4; ++i) {
        pages[i].dwSize = sizeof(PROPSHEETPAGEW);
        pages[i].dwFlags = PSP_USETITLE;
        pages[i].hInstance = inst;
        pages[i].pszTemplate = MAKEINTRESOURCEW(defs[i].id);
        pages[i].pfnDlgProc = defs[i].proc;
        pages[i].pszTitle = tr(defs[i].title);
        pages[i].lParam = reinterpret_cast<LPARAM>(&st);
    }
    PROPSHEETHEADERW hdr = {};
    hdr.dwSize = sizeof(hdr);
    hdr.dwFlags = PSH_PROPSHEETPAGE | PSH_USECALLBACK | PSH_NOCONTEXTHELP;
    hdr.hwndParent = owner;
    hdr.hInstance = inst;
    hdr.pszCaption = tr(L"Settings");
    hdr.nPages = 4;
    hdr.nStartPage = static_cast<UINT>(std::clamp(info.startPage, 0, 3));
    hdr.ppsp = pages;
    hdr.pfnCallback = sheetCallback;
    PropertySheetW(&hdr);
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

} // namespace mp
