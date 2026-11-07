#include "app/Popups.h"

#include <commctrl.h>
#include <commdlg.h>
#include <windowsx.h>

#include <algorithm>
#include <cmath>
#include <memory>
#include <vector>

#include "core/Lang.h"
#include "render/Renderer.h"

namespace mp {

namespace {

constexpr wchar_t kPopupClass[] = L"MiniPainterPopup";
ULONGLONG g_lastClose = 0;
HWND g_popup = nullptr;
bool g_modalChild = false; // 開著系統對話框時不要因失去焦點而關閉

// 一個可點的格子。
struct Cell {
    D2D1_RECT_F rect;
    int group;  // 自訂
    int value;  // 自訂
};

// D2D 繪製的彈出選單基底。
struct PopupBase {
    HWND hwnd = nullptr;
    HWND owner = nullptr;
    bool deactivated = false; // 因為別的視窗被啟用而關閉，就不把焦點搶回主視窗
    Renderer* renderer = nullptr;
    float ui = 1;
    ComPtr<ID2D1HwndRenderTarget> rt;
    std::vector<Cell> cells;
    int hover = -1;
    int width = 0, height = 0;

    virtual ~PopupBase() = default;
    virtual void draw() = 0;
    virtual void click(const Cell& c) = 0;
    virtual void onCreated() {}
    virtual bool onCommand(WPARAM, LPARAM) { return false; }

    float px(float v) const { return v * ui; }
    ID2D1SolidColorBrush* br(D2D1_COLOR_F c) { return renderer->brushFor(rt.Get(), c); }
    ID2D1SolidColorBrush* br(Color c) { return br(toD2D(c)); }

    bool ensureRt() {
        if (rt) return true;
        RECT rc;
        GetClientRect(hwnd, &rc);
        auto props = D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_DEFAULT,
                                                  D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96, 96);
        if (FAILED(renderer->d2d()->CreateHwndRenderTarget(props, D2D1::HwndRenderTargetProperties(hwnd, D2D1::SizeU(rc.right, rc.bottom)), &rt)))
            return false;
        return true;
    }
    void paint() {
        if (!ensureRt()) return;
        rt->BeginDraw();
        rt->Clear(D2D1::ColorF(0.98f, 0.98f, 0.98f));
        draw();
        if (rt->EndDraw() == D2DERR_RECREATE_TARGET) { renderer->forgetTarget(rt.Get()); rt.Reset(); InvalidateRect(hwnd, nullptr, FALSE); }
    }
    int cellAt(POINT p) const {
        for (size_t i = 0; i < cells.size(); ++i) {
            const auto& r = cells[i].rect;
            if (p.x >= r.left && p.x < r.right && p.y >= r.top && p.y < r.bottom) return static_cast<int>(i);
        }
        return -1;
    }
    void drawHover(int i) {
        if (i < 0) return;
        rt->FillRoundedRectangle(D2D1::RoundedRect(cells[i].rect, px(4), px(4)), br(D2D1::ColorF(0.85f, 0.85f, 0.85f)));
    }
    void drawSelectedFrame(const D2D1_RECT_F& r) {
        rt->DrawRoundedRectangle(D2D1::RoundedRect(r, px(4), px(4)), br(D2D1::ColorF(0.04f, 0.52f, 1.0f)), px(2));
    }
};

LRESULT CALLBACK popupProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto* self = reinterpret_cast<PopupBase*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    switch (msg) {
    case WM_NCCREATE:
        self = static_cast<PopupBase*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        self->hwnd = hwnd;
        return TRUE;
    case WM_CREATE: self->onCreated(); return 0;
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        BeginPaint(hwnd, &ps);
        self->paint();
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_MOUSEMOVE: {
        int h = self->cellAt({GET_X_LPARAM(lp), GET_Y_LPARAM(lp)});
        if (h != self->hover) { self->hover = h; InvalidateRect(hwnd, nullptr, FALSE); }
        return 0;
    }
    case WM_LBUTTONDOWN: {
        int h = self->cellAt({GET_X_LPARAM(lp), GET_Y_LPARAM(lp)});
        if (h >= 0) { self->click(self->cells[h]); if (IsWindow(hwnd)) InvalidateRect(hwnd, nullptr, FALSE); }
        return 0;
    }
    case WM_COMMAND:
        if (self->onCommand(wp, lp)) return 0;
        break;
    case WM_ACTIVATE:
        if (LOWORD(wp) == WA_INACTIVE && !g_modalChild) {
            // 焦點跑到自己的子視窗（例如下拉清單）不算
            HWND other = reinterpret_cast<HWND>(lp);
            if (!other || (GetAncestor(other, GA_ROOT) != hwnd)) {
                self->deactivated = other == nullptr || GetAncestor(other, GA_ROOT) != self->owner;
                DestroyWindow(hwnd);
            }
        }
        return 0;
    case WM_KEYDOWN:
        if (wp == VK_ESCAPE) DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        if (self->rt) self->renderer->forgetTarget(self->rt.Get());
        g_lastClose = GetTickCount64();
        if (g_popup == hwnd) g_popup = nullptr;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
        // 選完項目關閉時，鍵盤焦點不會自動回到主視窗（Delete、Esc 會失效），要主視窗自己拿回來。
        if (!self->deactivated && self->owner) PostMessageW(self->owner, WM_MP_POPUP_CLOSED, 0, 0);
        delete self;
        return 0;
    default: break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

void registerPopupClass(HINSTANCE inst) {
    static bool done = false;
    if (done) return;
    done = true;
    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = popupProc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = kPopupClass;
    RegisterClassExW(&wc);
}

// 建立並顯示。popup 的所有權交給視窗。
void showPopup(PopupBase* p, HWND owner, Renderer& renderer, RECT anchor, float ui, int w, int h) {
    closePopup();
    HINSTANCE inst = reinterpret_cast<HINSTANCE>(GetWindowLongPtrW(owner, GWLP_HINSTANCE));
    registerPopupClass(inst);
    p->owner = owner;
    p->renderer = &renderer;
    p->ui = ui;
    p->width = w;
    p->height = h;
    int x = anchor.left, y = anchor.bottom + 2;
    HMONITOR mon = MonitorFromRect(&anchor, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{sizeof(mi)};
    GetMonitorInfoW(mon, &mi);
    if (x + w > mi.rcWork.right) x = mi.rcWork.right - w;
    if (y + h > mi.rcWork.bottom) y = anchor.top - h - 2;
    HWND hwnd = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST, kPopupClass, L"", WS_POPUP | WS_BORDER, x, y, w, h, owner,
                                nullptr, inst, p);
    if (!hwnd) { delete p; return; }
    g_popup = hwnd;
    ShowWindow(hwnd, SW_SHOW);
    SetForegroundWindow(hwnd);
    SetFocus(hwnd);
}

// ---- 線型 ----

struct LineStylePopup : PopupBase {
    StrokeStyle cur;
    std::function<void(const StrokeStyle&)> onChange;
    void onCreated() override {
        cells.clear();
        float x0 = px(10), y = px(10);
        // 粗細：7 列
        for (int i = 0; i < kStrokeLevels; ++i) {
            cells.push_back({D2D1::RectF(x0, y, x0 + px(200), y + px(26)), 0, i});
            y += px(26);
        }
        y += px(10);
        for (int i = 0; i < 3; ++i) { // 線型
            cells.push_back({D2D1::RectF(x0 + i * px(66), y, x0 + i * px(66) + px(62), y + px(30)), 1, i});
        }
        y += px(40);
        for (int i = 0; i < 4; ++i) { // 箭頭
            cells.push_back({D2D1::RectF(x0 + i * px(50), y, x0 + i * px(50) + px(46), y + px(30)), 2, i});
        }
    }
    void draw() override {
        drawHover(hover);
        auto ink = D2D1::ColorF(0.15f, 0.15f, 0.15f);
        for (const auto& c : cells) {
            float cy = (c.rect.top + c.rect.bottom) / 2;
            if (c.group == 0) {
                if (cur.widthLevel == c.value) drawSelectedFrame(c.rect);
                float w = strokeWidthForLevel(c.value) * ui * 0.6f;
                rt->DrawLine(D2D1::Point2F(c.rect.left + px(14), cy), D2D1::Point2F(c.rect.right - px(14), cy), br(ink), std::max(1.0f, w),
                             renderer->roundStyle());
            } else if (c.group == 1) {
                if (static_cast<int>(cur.line) == c.value) drawSelectedFrame(c.rect);
                auto a = D2D1::Point2F(c.rect.left + px(8), cy), b = D2D1::Point2F(c.rect.right - px(8), cy);
                if (c.value == 0) rt->DrawLine(a, b, br(ink), px(3), renderer->roundStyle());
                else if (c.value == 1) rt->DrawLine(a, b, br(ink), px(3), renderer->dashStyle());
                else {
                    D2D1_COLOR_F f = ink; f.a = 0.15f; rt->DrawLine(a, b, br(f), px(9), renderer->roundStyle());
                    f.a = 0.3f; rt->DrawLine(a, b, br(f), px(6), renderer->roundStyle());
                    f.a = 0.8f; rt->DrawLine(a, b, br(f), px(3), renderer->roundStyle());
                }
            } else {
                if (static_cast<int>(cur.arrows) == c.value) drawSelectedFrame(c.rect);
                auto a = D2D1::Point2F(c.rect.left + px(8), cy), b = D2D1::Point2F(c.rect.right - px(8), cy);
                rt->DrawLine(a, b, br(ink), px(2), renderer->roundStyle());
                auto head = [&](D2D1_POINT_2F tip, float dir) {
                    rt->DrawLine(tip, D2D1::Point2F(tip.x - dir * px(7), tip.y - px(5)), br(ink), px(2), renderer->roundStyle());
                    rt->DrawLine(tip, D2D1::Point2F(tip.x - dir * px(7), tip.y + px(5)), br(ink), px(2), renderer->roundStyle());
                };
                if (c.value == 1 || c.value == 3) head(a, -1);
                if (c.value == 2 || c.value == 3) head(b, 1);
            }
        }
    }
    void click(const Cell& c) override {
        if (c.group == 0) cur.widthLevel = c.value;
        else if (c.group == 1) cur.line = static_cast<LineStyle>(c.value);
        else cur.arrows = static_cast<ArrowMode>(c.value);
        onChange(cur);
    }
};

// ---- 顏色 ----

Color hsv(float h, float s, float v) {
    float c = v * s, x = c * (1 - std::abs(std::fmod(h / 60.0f, 2.0f) - 1)), m = v - c;
    float r = 0, g = 0, b = 0;
    if (h < 60) { r = c; g = x; } else if (h < 120) { r = x; g = c; } else if (h < 180) { g = c; b = x; }
    else if (h < 240) { g = x; b = c; } else if (h < 300) { r = x; b = c; } else { r = c; b = x; }
    return Color::rgb(static_cast<uint8_t>(std::lround((r + m) * 255)), static_cast<uint8_t>(std::lround((g + m) * 255)),
                      static_cast<uint8_t>(std::lround((b + m) * 255)));
}

struct ColorPopup : PopupBase {
    Color cur;
    bool allowNone = true;
    std::function<void(Color)> onChange;
    std::vector<Color> colors; // 對應 cells，group 0；group 1 是「顯示顏色…」
    static constexpr int kCols = 12, kRows = 8;

    void onCreated() override {
        cells.clear();
        colors.clear();
        float cell = px(22), gap = px(4), x0 = px(10), y = px(10);
        // 快速色塊
        const Color quick[kCols] = {Color::rgb(255, 59, 48), Color::rgb(255, 149, 0), Color::rgb(255, 214, 10), Color::rgb(52, 199, 89),
                                    Color::rgb(0, 199, 190), Color::rgb(0, 122, 255), Color::rgb(255, 45, 85), Color::rgb(175, 82, 222),
                                    Color::rgb(162, 132, 94), Color::rgb(255, 255, 255), Color::rgb(142, 142, 147), Color::rgb(0, 0, 0)};
        for (int i = 0; i < kCols; ++i) {
            cells.push_back({D2D1::RectF(x0 + i * (cell + gap), y, x0 + i * (cell + gap) + cell, y + cell), 0, static_cast<int>(colors.size())});
            colors.push_back(quick[i]);
        }
        y += cell + px(12);
        // 第一列：無色 + 灰階
        for (int i = 0; i < kCols; ++i) {
            cells.push_back({D2D1::RectF(x0 + i * (cell + gap), y, x0 + i * (cell + gap) + cell, y + cell), 0, static_cast<int>(colors.size())});
            if (i == 0) colors.push_back(Color::noColor());
            else { uint8_t g = static_cast<uint8_t>(255 - (i - 1) * 255 / (kCols - 2)); colors.push_back(Color::rgb(g, g, g)); }
        }
        y += cell + gap;
        // 10 個色相 × 8 級亮度（直欄是色相，橫列由淺到深）
        for (int r = 0; r < kRows; ++r) {
            for (int i = 0; i < kCols; ++i) {
                cells.push_back({D2D1::RectF(x0 + i * (cell + gap), y, x0 + i * (cell + gap) + cell, y + cell), 0, static_cast<int>(colors.size())});
                float hue = i * 30.0f;
                float t = r / static_cast<float>(kRows - 1); // 0 淺 → 1 深
                float s = std::min(1.0f, 0.25f + t * 1.2f), v = t < 0.5f ? 1.0f : 1.0f - (t - 0.5f) * 1.4f;
                colors.push_back(hsv(hue, s, v));
            }
            y += cell + gap;
        }
        y += px(8);
        cells.push_back({D2D1::RectF(x0, y, x0 + kCols * (cell + gap) - gap, y + px(28)), 1, 0});
    }
    void draw() override {
        drawHover(hover);
        for (size_t i = 0; i < cells.size(); ++i) {
            const auto& c = cells[i];
            if (c.group == 1) {
                rt->DrawRoundedRectangle(D2D1::RoundedRect(c.rect, px(4), px(4)), br(D2D1::ColorF(0.6f, 0.6f, 0.6f)), 1);
                renderer->drawUiText(rt.Get(), tr(L"Show Colors…"), c.rect, px(13), D2D1::ColorF(0.15f, 0.15f, 0.15f), DWRITE_TEXT_ALIGNMENT_CENTER);
                continue;
            }
            Color col = colors[c.value];
            D2D1_RECT_F r = c.rect;
            if (col.none()) {
                rt->DrawRectangle(r, br(D2D1::ColorF(0.5f, 0.5f, 0.5f)), 1);
                rt->DrawLine(D2D1::Point2F(r.left + 2, r.bottom - 2), D2D1::Point2F(r.right - 2, r.top + 2), br(D2D1::ColorF(0.85f, 0.1f, 0.1f)), px(1.5f));
            } else {
                rt->FillRectangle(r, br(col));
                rt->DrawRectangle(r, br(D2D1::ColorF(0, 0, 0, 0.15f)), 1);
            }
            if (col == cur) drawSelectedFrame(D2D1::RectF(r.left - 2, r.top - 2, r.right + 2, r.bottom + 2));
        }
    }
    void click(const Cell& c) override {
        if (c.group == 1) {
            Color pick = cur.none() ? Color::rgb(255, 0, 0) : cur;
            g_modalChild = true;
            bool ok = chooseColorDialog(owner, pick);
            g_modalChild = false;
            if (ok) { cur = pick; onChange(cur); }
            if (IsWindow(hwnd)) DestroyWindow(hwnd);
            return;
        }
        cur = colors[c.value];
        onChange(cur);
    }
};

// ---- 形狀 ----

struct ShapesPopup : PopupBase {
    std::function<void(Kind)> onPick;
    static const Kind kKinds[9];
    void onCreated() override {
        cells.clear();
        float cell = px(44), gap = px(6), x0 = px(10), y0 = px(10);
        for (int i = 0; i < 9; ++i) {
            int col = i % 5, row = i / 5;
            float x = x0 + col * (cell + gap), y = y0 + row * (cell + gap);
            cells.push_back({D2D1::RectF(x, y, x + cell, y + cell), 0, i});
        }
    }
    void draw() override {
        drawHover(hover);
        for (const auto& c : cells) {
            D2D1_RECT_F box = D2D1::RectF(c.rect.left + px(8), c.rect.top + px(8), c.rect.right - px(8), c.rect.bottom - px(8));
            drawKindIcon(*renderer, rt.Get(), kKinds[c.value], box, D2D1::ColorF(0.15f, 0.15f, 0.15f));
        }
    }
    void click(const Cell& c) override {
        Kind k = kKinds[c.value];
        HWND h = hwnd;
        onPick(k);
        if (IsWindow(h)) DestroyWindow(h);
    }
};
const Kind ShapesPopup::kKinds[9] = {Kind::Line, Kind::Rect, Kind::RoundRect, Kind::Ellipse, Kind::Bubble,
                                     Kind::Star, Kind::Polygon, Kind::Magnifier, Kind::Mosaic};

// ---- 格式（用系統控制項） ----

struct FormatPopup : PopupBase {
    TextStyle cur;
    std::function<void(const TextStyle&)> onChange;
    HWND fontCombo = nullptr, sizeEdit = nullptr, sizeSpin = nullptr, colorBtn = nullptr;
    HWND bold = nullptr, italic = nullptr, underline = nullptr;
    HWND align[4] = {};
    HFONT font = nullptr;
    bool updating = false;
    // 標籤欄與對齊按鈕的寬度依翻譯後的文字量出來（日文的「スタイル」「中央揃え」比中英文寬）。
    float labelW = 36, alignW = 46;
    enum { ID_FONT = 10, ID_SIZE, ID_SPIN, ID_COLOR, ID_B, ID_I, ID_U, ID_AL, ID_AC, ID_AR, ID_AJ };

    static int CALLBACK enumFonts(const LOGFONTW* lf, const TEXTMETRICW*, DWORD, LPARAM lp) {
        auto* self = reinterpret_cast<FormatPopup*>(lp);
        if (lf->lfFaceName[0] == L'@') return 1; // 直排字型不列
        if (SendMessageW(self->fontCombo, CB_FINDSTRINGEXACT, static_cast<WPARAM>(-1), reinterpret_cast<LPARAM>(lf->lfFaceName)) == CB_ERR)
            SendMessageW(self->fontCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(lf->lfFaceName));
        return 1;
    }

    void onCreated() override {
        HINSTANCE inst = reinterpret_cast<HINSTANCE>(GetWindowLongPtrW(hwnd, GWLP_HINSTANCE));
        font = CreateFontW(-static_cast<int>(px(13)), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Microsoft JhengHei UI");
        auto mk = [&](const wchar_t* cls, const wchar_t* text, DWORD style, int id, float x, float y, float w, float h) {
            HWND c = CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style, static_cast<int>(px(x)), static_cast<int>(px(y)),
                                     static_cast<int>(px(w)), static_cast<int>(px(h)), hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), inst, nullptr);
            SendMessageW(c, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
            return c;
        };
        const float cx = 14 + labelW; // 控制項欄的左緣
        mk(L"STATIC", tr(L"Font"), 0, 0, 10, 12, labelW, 18);
        fontCombo = mk(L"COMBOBOX", L"", CBS_DROPDOWNLIST | CBS_SORT | WS_VSCROLL | WS_TABSTOP, ID_FONT, cx, 10, 200, 300);
        HDC dc = GetDC(hwnd);
        LOGFONTW lf{};
        lf.lfCharSet = DEFAULT_CHARSET;
        EnumFontFamiliesExW(dc, &lf, enumFonts, reinterpret_cast<LPARAM>(this), 0);
        ReleaseDC(hwnd, dc);
        mk(L"STATIC", tr(L"Size"), 0, 0, 10, 42, labelW, 18);
        sizeEdit = mk(L"EDIT", L"", WS_BORDER | ES_NUMBER | ES_AUTOHSCROLL, ID_SIZE, cx, 40, 60, 22);
        sizeSpin = mk(UPDOWN_CLASSW, L"", UDS_SETBUDDYINT | UDS_ALIGNRIGHT | UDS_ARROWKEYS, ID_SPIN, cx + 60, 40, 16, 22);
        SendMessageW(sizeSpin, UDM_SETBUDDY, reinterpret_cast<WPARAM>(sizeEdit), 0);
        SendMessageW(sizeSpin, UDM_SETRANGE32, 4, 400);
        colorBtn = mk(L"BUTTON", tr(L"Text Color…"), BS_PUSHBUTTON, ID_COLOR, cx + 90, 40, 110, 22);
        bold = mk(L"BUTTON", L"B", BS_AUTOCHECKBOX | BS_PUSHLIKE, ID_B, cx, 72, 30, 24);
        italic = mk(L"BUTTON", L"I", BS_AUTOCHECKBOX | BS_PUSHLIKE, ID_I, cx + 34, 72, 30, 24);
        underline = mk(L"BUTTON", L"U", BS_AUTOCHECKBOX | BS_PUSHLIKE, ID_U, cx + 68, 72, 30, 24);
        const wchar_t* alName[4] = {tr(L"Left"), tr(L"Center"), tr(L"Right"), tr(L"Justify")};
        for (int i = 0; i < 4; ++i)
            align[i] = mk(L"BUTTON", alName[i], BS_AUTOCHECKBOX | BS_PUSHLIKE, ID_AL + i, cx + i * (alignW + 4), 104, alignW, 24);
        syncControls();
    }
    // 控制項用的字（13px JhengHei UI）量一段文字的寬，回傳未乘 ui 的邏輯像素。
    static float textWidth(const wchar_t* s, float ui) {
        HDC dc = GetDC(nullptr);
        HFONT f = CreateFontW(-static_cast<int>(13 * ui), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Microsoft JhengHei UI");
        HGDIOBJ old = SelectObject(dc, f);
        SIZE sz{};
        GetTextExtentPoint32W(dc, s, static_cast<int>(wcslen(s)), &sz);
        SelectObject(dc, old);
        DeleteObject(f);
        ReleaseDC(nullptr, dc);
        return sz.cx / ui;
    }
    // 依目前語言算欄寬，回傳整個彈出選單需要的寬度（邏輯像素）。
    float measure(float ui) {
        for (auto* s : {L"Font", L"Size", L"Style", L"Alignment"}) labelW = std::max(labelW, textWidth(tr(s), ui) + 4);
        for (auto* s : {L"Left", L"Center", L"Right", L"Justify"}) alignW = std::max(alignW, textWidth(tr(s), ui) + 14);
        const float cx = 14 + labelW;
        return std::max({262.0f, cx + 4 * (alignW + 4) + 6, cx + 200 + 12});
    }
    void syncControls() {
        updating = true;
        SendMessageW(fontCombo, CB_SELECTSTRING, static_cast<WPARAM>(-1), reinterpret_cast<LPARAM>(cur.font.c_str()));
        SetWindowTextW(sizeEdit, std::to_wstring(static_cast<int>(std::lround(cur.size))).c_str());
        Button_SetCheck(bold, cur.bold);
        Button_SetCheck(italic, cur.italic);
        Button_SetCheck(underline, cur.underline);
        for (int i = 0; i < 4; ++i) Button_SetCheck(align[i], static_cast<int>(cur.align) == i);
        updating = false;
    }
    bool onCommand(WPARAM wp, LPARAM) override {
        if (updating) return true;
        int id = LOWORD(wp), code = HIWORD(wp);
        switch (id) {
        case ID_FONT:
            if (code == CBN_SELCHANGE) {
                wchar_t buf[LF_FACESIZE];
                int sel = static_cast<int>(SendMessageW(fontCombo, CB_GETCURSEL, 0, 0));
                if (sel >= 0 && SendMessageW(fontCombo, CB_GETLBTEXT, sel, reinterpret_cast<LPARAM>(buf)) != CB_ERR) {
                    cur.font = buf;
                    onChange(cur);
                }
            }
            return true;
        case ID_SIZE:
            if (code == EN_CHANGE) {
                wchar_t buf[16];
                GetWindowTextW(sizeEdit, buf, 16);
                int v = _wtoi(buf);
                if (v >= 4 && v <= 400) { cur.size = static_cast<float>(v); onChange(cur); }
            }
            return true;
        case ID_COLOR: {
            Color c = cur.color;
            g_modalChild = true;
            bool ok = chooseColorDialog(owner, c);
            g_modalChild = false;
            if (ok) { cur.color = c; onChange(cur); }
            SetFocus(hwnd);
            return true;
        }
        case ID_B: cur.bold = Button_GetCheck(bold) == BST_CHECKED; onChange(cur); return true;
        case ID_I: cur.italic = Button_GetCheck(italic) == BST_CHECKED; onChange(cur); return true;
        case ID_U: cur.underline = Button_GetCheck(underline) == BST_CHECKED; onChange(cur); return true;
        case ID_AL: case ID_AC: case ID_AR: case ID_AJ:
            cur.align = static_cast<TextAlign>(id - ID_AL);
            syncControls();
            onChange(cur);
            return true;
        default: return false;
        }
    }
    void draw() override {
        renderer->drawUiText(rt.Get(), tr(L"Style"), D2D1::RectF(px(10), px(72), px(10 + labelW), px(96)), px(13), D2D1::ColorF(0.15f, 0.15f, 0.15f));
        renderer->drawUiText(rt.Get(), tr(L"Alignment"), D2D1::RectF(px(10), px(104), px(10 + labelW), px(128)), px(13), D2D1::ColorF(0.15f, 0.15f, 0.15f));
    }
    void click(const Cell&) override {}
    ~FormatPopup() override { if (font) DeleteObject(font); }
};

} // namespace

// ---------------------------------------------------------------------------

void showLineStylePopup(HWND owner, Renderer& renderer, RECT anchor, float ui, StrokeStyle current,
                        std::function<void(const StrokeStyle&)> onChange) {
    auto* p = new LineStylePopup;
    p->cur = current;
    p->onChange = std::move(onChange);
    showPopup(p, owner, renderer, anchor, ui, static_cast<int>(220 * ui), static_cast<int>(kStrokeLevels * 26 * ui + 110 * ui));
}

void showColorPopup(HWND owner, Renderer& renderer, RECT anchor, float ui, Color current, bool allowNone,
                    std::function<void(Color)> onChange) {
    auto* p = new ColorPopup;
    p->cur = current;
    p->allowNone = allowNone;
    p->onChange = std::move(onChange);
    int w = static_cast<int>((20 + 12 * 26) * ui), h = static_cast<int>((10 + 22 + 12 + 9 * 26 + 8 + 28 + 12) * ui);
    showPopup(p, owner, renderer, anchor, ui, w, h);
}

void showShapesPopup(HWND owner, Renderer& renderer, RECT anchor, float ui, std::function<void(Kind)> onPick) {
    auto* p = new ShapesPopup;
    p->onPick = std::move(onPick);
    showPopup(p, owner, renderer, anchor, ui, static_cast<int>((20 + 5 * 50 - 6) * ui), static_cast<int>((20 + 2 * 50 - 6) * ui));
}

void showFormatPopup(HWND owner, Renderer& renderer, RECT anchor, float ui, TextStyle current,
                     std::function<void(const TextStyle&)> onChange) {
    auto* p = new FormatPopup;
    p->cur = current;
    p->onChange = std::move(onChange);
    float w = p->measure(ui);
    showPopup(p, owner, renderer, anchor, ui, static_cast<int>(w * ui), static_cast<int>(140 * ui));
}

bool isPopupOpen() { return g_popup != nullptr; }

void closePopup() {
    if (g_popup && IsWindow(g_popup)) DestroyWindow(g_popup);
    g_popup = nullptr;
}

ULONGLONG lastPopupCloseTick() { return g_lastClose; }

bool chooseColorDialog(HWND owner, Color& inout) {
    static COLORREF custom[16] = {};
    CHOOSECOLORW cc{sizeof(cc)};
    cc.hwndOwner = owner;
    cc.lpCustColors = custom;
    cc.rgbResult = RGB(inout.r, inout.g, inout.b);
    cc.Flags = CC_RGBINIT | CC_FULLOPEN | CC_ANYCOLOR;
    if (!ChooseColorW(&cc)) return false;
    inout = Color::rgb(GetRValue(cc.rgbResult), GetGValue(cc.rgbResult), GetBValue(cc.rgbResult));
    return true;
}

void drawKindIcon(Renderer& renderer, ID2D1RenderTarget* rt, Kind kind, D2D1_RECT_F box, D2D1_COLOR_F color) {
    auto* br = renderer.brushFor(rt, color);
    float w = box.right - box.left, h = box.bottom - box.top;
    float cx = (box.left + box.right) / 2, cy = (box.top + box.bottom) / 2;
    float sw = std::max(1.5f, w * 0.07f);
    auto P = [](float x, float y) { return D2D1::Point2F(x, y); };
    switch (kind) {
    case Kind::Line:
        rt->DrawLine(P(box.left, box.bottom), P(box.right, box.top), br, sw);
        rt->DrawLine(P(box.right, box.top), P(box.right - w * 0.35f, box.top), br, sw);
        rt->DrawLine(P(box.right, box.top), P(box.right, box.top + h * 0.35f), br, sw);
        break;
    case Kind::Rect: rt->DrawRectangle(box, br, sw); break;
    case Kind::RoundRect: rt->DrawRoundedRectangle(D2D1::RoundedRect(box, w * 0.2f, h * 0.2f), br, sw); break;
    case Kind::Ellipse: rt->DrawEllipse(D2D1::Ellipse(P(cx, cy), w / 2, h / 2), br, sw); break;
    case Kind::Bubble: {
        Annotation a = makeShape(Kind::Bubble, {cx, cy - h * 0.12f}, h * 2.4f, StrokeStyle{});
        a.tailTip = {box.left + w * 0.2f, box.bottom};
        auto pts = bubbleOutline(a);
        for (size_t i = 0; i + 1 < pts.size(); ++i) rt->DrawLine(toD2D(pts[i]), toD2D(pts[i + 1]), br, sw);
        rt->DrawLine(toD2D(pts.back()), toD2D(pts.front()), br, sw);
        break;
    }
    case Kind::Star:
    case Kind::Polygon: {
        Annotation a = makeShape(kind, {cx, cy}, std::min(w, h) / 0.3f, StrokeStyle{});
        auto pts = starPoints(a);
        for (size_t i = 0; i < pts.size(); ++i) rt->DrawLine(toD2D(pts[i]), toD2D(pts[(i + 1) % pts.size()]), br, sw);
        break;
    }
    case Kind::Magnifier:
        rt->DrawEllipse(D2D1::Ellipse(P(cx, cy), w * 0.42f, h * 0.42f), br, sw);
        rt->DrawLine(P(cx - w * 0.15f, cy), P(cx + w * 0.15f, cy), br, sw);
        rt->DrawLine(P(cx, cy - h * 0.15f), P(cx, cy + h * 0.15f), br, sw);
        break;
    case Kind::Mosaic: {
        float cw = w / 4, ch = h / 4;
        for (int y = 0; y < 4; ++y)
            for (int x = 0; x < 4; ++x) {
                D2D1_COLOR_F c = color;
                c.a = ((x + y) % 2) ? 0.9f : 0.35f;
                rt->FillRectangle(D2D1::RectF(box.left + x * cw, box.top + y * ch, box.left + (x + 1) * cw, box.top + (y + 1) * ch), renderer.brushFor(rt, c));
            }
        break;
    }
    case Kind::Freehand: {
        ComPtr<ID2D1PathGeometry> geo;
        renderer.d2d()->CreatePathGeometry(&geo);
        ComPtr<ID2D1GeometrySink> sink;
        geo->Open(&sink);
        sink->BeginFigure(P(box.left, cy + h * 0.3f), D2D1_FIGURE_BEGIN_HOLLOW);
        sink->AddBezier(D2D1::BezierSegment(P(box.left + w * 0.2f, box.top - h * 0.3f), P(box.left + w * 0.5f, box.bottom + h * 0.3f), P(cx + w * 0.1f, cy)));
        sink->AddBezier(D2D1::BezierSegment(P(box.right - w * 0.3f, box.top - h * 0.2f), P(box.right - w * 0.2f, box.bottom), P(box.right, cy - h * 0.2f)));
        sink->EndFigure(D2D1_FIGURE_END_OPEN);
        sink->Close();
        rt->DrawGeometry(geo.Get(), br, sw);
        break;
    }
    case Kind::Text:
        renderer.drawUiText(rt, L"A", box, h, color, DWRITE_TEXT_ALIGNMENT_CENTER);
        break;
    }
}

} // namespace mp
