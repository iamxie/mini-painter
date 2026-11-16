#include "app/Popups.h"

#include <commctrl.h>
#include <commdlg.h>
#include <windowsx.h>

#include <algorithm>
#include <cmath>
#include <iterator>
#include <memory>
#include <vector>

#include "core/Fonts.h"
#include "core/Lang.h"
#include "render/Surface.h"

namespace mp {

namespace {

constexpr wchar_t kPopupClass[] = L"MiniPainterPopup";

// 一個可點的格子。
struct Cell {
    D2D1_RECT_F rect;
    int group;  // 自訂
    int value;  // 自訂
};

} // namespace

// D2D 繪製的彈出選單基底。物件由 PopupMenus 握著，活得比它的視窗久（見 Popups.h）。
struct PopupBase {
    HWND hwnd = nullptr; // 視窗關掉後是 nullptr
    HWND owner = nullptr;
    ULONGLONG* closedAt = nullptr; // 視窗關掉時把時刻記在這裡（PopupMenus::lastClose_）
    bool modalChild = false;  // 開著系統對話框（選色）：這時不要因為失去焦點而關閉
    bool deactivated = false; // 因為別的視窗被啟用而關閉，就不把焦點搶回主視窗
    RenderDevice* device = nullptr;
    UiScale px;
    ComPtr<ID2D1HwndRenderTarget> rt;
    Surface surface; // rt 與它的畫刷；選單關掉時跟著這個物件一起釋放
    std::vector<Cell> cells;
    int hover = -1;
    // click / onCommand 還沒回來的層數。它們可以直接 DestroyWindow(hwnd)（選完就關），視窗也可能因為失去焦點被關；
    // 物件要等這些呼叫都回來才能放掉（PopupMenus::reap 會看）。
    int inCall = 0;

    virtual ~PopupBase() = default;
    // 排好 cells，回傳選單視窗要多大（實際像素）。開視窗之前呼叫一次：排版和視窗大小只算這一次。
    virtual D2D1_SIZE_F layout() = 0;
    virtual void draw() = 0;
    virtual void click(const Cell& c) = 0;
    virtual void onCreated() {}
    virtual bool onCommand(WPARAM, LPARAM) { return false; }

    ID2D1SolidColorBrush* br(D2D1_COLOR_F c) { return surface.brush(c); }
    ID2D1SolidColorBrush* br(Color c) { return br(toD2D(c)); }

    bool ensureRt() {
        if (rt) return true;
        rt = device->createHwndTarget(hwnd);
        surface.attach(rt);
        return rt != nullptr;
    }
    void paint() {
        if (!ensureRt()) return;
        rt->BeginDraw();
        rt->Clear(theme::kPopupBg);
        draw();
        HRESULT hr = rt->EndDraw();
        surface.endFrame();
        if (hr == D2DERR_RECREATE_TARGET) {
            surface.reset();
            rt.Reset();
            InvalidateRect(hwnd, nullptr, FALSE);
        }
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
        rt->FillRoundedRectangle(D2D1::RoundedRect(cells[i].rect, px(4), px(4)), br(theme::kPopupHover));
    }
    void drawSelectedFrame(const D2D1_RECT_F& r) {
        rt->DrawRoundedRectangle(D2D1::RoundedRect(r, px(4), px(4)), br(theme::kAccent), px(2));
    }
    // 從選單開選色對話框。開著時把選單藏起來：選單是最上層視窗會蓋住對話框；而且選單還能點，再按一次會疊出第二個對話框。
    bool pickColor(Color& inout) {
        modalChild = true; // 先設：藏起來時選單會失去焦點
        ShowWindow(hwnd, SW_HIDE);
        const bool ok = chooseColorDialog(owner, inout);
        modalChild = false;
        return ok;
    }
};

namespace {

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
        if (h < 0) return 0;
        ++self->inCall;
        self->click(self->cells[h]);
        --self->inCall;
        if (self->hwnd) InvalidateRect(hwnd, nullptr, FALSE); // click 裡可能把視窗關了
        return 0;
    }
    case WM_COMMAND: {
        if (!self) break;
        ++self->inCall;
        const bool handled = self->onCommand(wp, lp);
        --self->inCall;
        if (handled || !self->hwnd) return 0;
        break;
    }
    case WM_ACTIVATE:
        if (LOWORD(wp) == WA_INACTIVE && !self->modalChild) {
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
        if (!self) return 0;
        if (self->closedAt) *self->closedAt = GetTickCount64();
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
        self->hwnd = nullptr; // 物件還在（PopupMenus 握著），之後由 reap 放掉
        // 通知主視窗把物件放掉。選完項目關閉時，鍵盤焦點不會自動回到主視窗（Delete、Esc 會失效），要它自己拿回來。
        if (self->owner) PostMessageW(self->owner, WM_MP_POPUP_CLOSED, self->deactivated ? 1 : 0, 0);
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

// ---- 線型 ----

struct LineStylePopup : PopupBase {
    StrokeStyle cur;
    std::function<void(const StrokeStyle&)> onChange;
    D2D1_SIZE_F layout() override {
        cells.clear();
        float x0 = px(10), y = px(10);
        // 粗細：一級一列
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
        return D2D1::SizeF(px(220), y + px(30) + px(20));
    }
    void draw() override {
        drawHover(hover);
        const D2D1_COLOR_F ink = theme::kPopupInk;
        for (const auto& c : cells) {
            float cy = (c.rect.top + c.rect.bottom) / 2;
            if (c.group == 0) {
                if (strokeLevelForWidth(cur.width) == c.value) drawSelectedFrame(c.rect);
                float w = px(strokeWidthForLevel(c.value)) * 0.6f;
                rt->DrawLine(D2D1::Point2F(c.rect.left + px(14), cy), D2D1::Point2F(c.rect.right - px(14), cy), br(ink), std::max(1.0f, w),
                             device->roundStyle());
            } else if (c.group == 1) {
                if (static_cast<int>(cur.line) == c.value) drawSelectedFrame(c.rect);
                auto a = D2D1::Point2F(c.rect.left + px(8), cy), b = D2D1::Point2F(c.rect.right - px(8), cy);
                if (c.value == 0) rt->DrawLine(a, b, br(ink), px(3), device->roundStyle());
                else if (c.value == 1) rt->DrawLine(a, b, br(ink), px(3), device->dashStyle());
                else {
                    for (const FuzzyLayer& layer : kFuzzyLayers) { // 和實際畫出來的同一組參數
                        D2D1_COLOR_F f = ink;
                        f.a = ink.a * layer.alpha;
                        rt->DrawLine(a, b, br(f), px(3) * layer.widthScale, device->roundStyle());
                    }
                }
            } else {
                if (static_cast<int>(cur.arrows) == c.value) drawSelectedFrame(c.rect);
                auto a = D2D1::Point2F(c.rect.left + px(8), cy), b = D2D1::Point2F(c.rect.right - px(8), cy);
                rt->DrawLine(a, b, br(ink), px(2), device->roundStyle());
                auto head = [&](D2D1_POINT_2F tip, float dir) {
                    rt->DrawLine(tip, D2D1::Point2F(tip.x - dir * px(7), tip.y - px(5)), br(ink), px(2), device->roundStyle());
                    rt->DrawLine(tip, D2D1::Point2F(tip.x - dir * px(7), tip.y + px(5)), br(ink), px(2), device->roundStyle());
                };
                if (c.value == 1 || c.value == 3) head(a, -1);
                if (c.value == 2 || c.value == 3) head(b, 1);
            }
        }
    }
    void click(const Cell& c) override {
        if (c.group == 0) cur.width = strokeWidthForLevel(c.value);
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
    std::function<void(Color)> onChange;
    std::vector<Color> colors; // 對應 cells，group 0；group 1 是「顯示顏色…」
    static constexpr int kCols = 12, kRows = 8;

    D2D1_SIZE_F layout() override {
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
        // kCols 個色相 × kRows 級亮度（直欄是色相，橫列由淺到深）
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
        return D2D1::SizeF(x0 + kCols * (cell + gap) + px(10), y + px(28) + px(12));
    }
    void draw() override {
        drawHover(hover);
        for (size_t i = 0; i < cells.size(); ++i) {
            const auto& c = cells[i];
            if (c.group == 1) {
                rt->DrawRoundedRectangle(D2D1::RoundedRect(c.rect, px(4), px(4)), br(theme::kPopupBtnLine), 1);
                surface.drawUiText(tr(L"Show Colors…"), c.rect, px(13), theme::kPopupInk, DWRITE_TEXT_ALIGNMENT_CENTER);
                continue;
            }
            Color col = colors[c.value];
            D2D1_RECT_F r = c.rect;
            if (col.none()) {
                rt->DrawRectangle(r, br(theme::kSwatchNoneLine), 1);
                rt->DrawLine(D2D1::Point2F(r.left + 2, r.bottom - 2), D2D1::Point2F(r.right - 2, r.top + 2), br(theme::kNoColorSlash), px(1.5f));
            } else {
                rt->FillRectangle(r, br(col));
                rt->DrawRectangle(r, br(theme::kSwatchLine), 1);
            }
            if (col == cur) drawSelectedFrame(D2D1::RectF(r.left - 2, r.top - 2, r.right + 2, r.bottom + 2));
        }
    }
    void click(const Cell& c) override {
        if (c.group == 1) {
            Color pick = cur.none() ? Color::rgb(255, 0, 0) : cur;
            if (pickColor(pick)) { cur = pick; onChange(cur); }
            DestroyWindow(hwnd);
            return;
        }
        cur = colors[c.value];
        onChange(cur);
    }
};

// ---- 形狀 ----

struct ShapesPopup : PopupBase {
    std::function<void(Kind)> onPick;
    // 選單裡有哪些形狀、順序、一列幾個。加一種形狀在這裡加（drawKindIcon 也要會畫它）。
    static constexpr Kind kKinds[] = {Kind::Line, Kind::Rect,    Kind::RoundRect, Kind::Ellipse, Kind::Bubble,
                                      Kind::Star, Kind::Polygon, Kind::Magnifier, Kind::Mosaic};
    static constexpr int kCount = static_cast<int>(std::size(kKinds)), kPerRow = 5;
    D2D1_SIZE_F layout() override {
        cells.clear();
        float cell = px(44), gap = px(6), x0 = px(10), y0 = px(10);
        for (int i = 0; i < kCount; ++i) {
            int col = i % kPerRow, row = i / kPerRow;
            float x = x0 + col * (cell + gap), y = y0 + row * (cell + gap);
            cells.push_back({D2D1::RectF(x, y, x + cell, y + cell), 0, i});
        }
        const int rows = (kCount + kPerRow - 1) / kPerRow;
        return D2D1::SizeF(2 * x0 + kPerRow * (cell + gap) - gap, 2 * y0 + rows * (cell + gap) - gap);
    }
    void draw() override {
        drawHover(hover);
        for (const auto& c : cells) {
            D2D1_RECT_F box = D2D1::RectF(c.rect.left + px(8), c.rect.top + px(8), c.rect.right - px(8), c.rect.bottom - px(8));
            drawKindIcon(surface, kKinds[c.value], box, theme::kPopupInk);
        }
    }
    void click(const Cell& c) override {
        onPick(kKinds[c.value]);
        DestroyWindow(hwnd);
    }
};

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
    static constexpr float kControlFontSize = 13;

    static int CALLBACK enumFonts(const LOGFONTW* lf, const TEXTMETRICW*, DWORD, LPARAM lp) {
        auto* self = reinterpret_cast<FormatPopup*>(lp);
        if (lf->lfFaceName[0] == L'@') return 1; // 直排字型不列
        if (SendMessageW(self->fontCombo, CB_FINDSTRINGEXACT, static_cast<WPARAM>(-1), reinterpret_cast<LPARAM>(lf->lfFaceName)) == CB_ERR)
            SendMessageW(self->fontCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(lf->lfFaceName));
        return 1;
    }

    void onCreated() override {
        HINSTANCE inst = reinterpret_cast<HINSTANCE>(GetWindowLongPtrW(hwnd, GWLP_HINSTANCE));
        font = controlFont(px);
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
    // 選單裡系統控制項用的字（介面字型，kControlFontSize）。用完要 DeleteObject。
    static HFONT controlFont(UiScale scale) {
        return CreateFontW(-static_cast<int>(scale(kControlFontSize)), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0,
                           kUiFont);
    }
    // 用控制項的字量一段文字的寬，回傳邏輯像素。
    static float textWidth(const wchar_t* s, UiScale scale) {
        HDC dc = GetDC(nullptr);
        HFONT f = controlFont(scale);
        HGDIOBJ old = SelectObject(dc, f);
        SIZE sz{};
        GetTextExtentPoint32W(dc, s, static_cast<int>(wcslen(s)), &sz);
        SelectObject(dc, old);
        DeleteObject(f);
        ReleaseDC(nullptr, dc);
        return sz.cx / scale.factor;
    }
    // 依目前語言算欄寬（控制項在 onCreated 照這兩個寬度擺）。
    D2D1_SIZE_F layout() override {
        for (auto* s : {L"Font", L"Size", L"Style", L"Alignment"}) labelW = std::max(labelW, textWidth(tr(s), px) + 4);
        for (auto* s : {L"Left", L"Center", L"Right", L"Justify"}) alignW = std::max(alignW, textWidth(tr(s), px) + 14);
        const float cx = 14 + labelW;
        return D2D1::SizeF(px(std::max({262.0f, cx + 4 * (alignW + 4) + 6, cx + 200 + 12})), px(140));
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
            if (pickColor(c)) { cur.color = c; onChange(cur); }
            ShowWindow(hwnd, SW_SHOW);
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
        surface.drawUiText(tr(L"Style"), D2D1::RectF(px(10), px(72), px(10 + labelW), px(96)), px(kControlFontSize), theme::kPopupInk);
        surface.drawUiText(tr(L"Alignment"), D2D1::RectF(px(10), px(104), px(10 + labelW), px(128)), px(kControlFontSize), theme::kPopupInk);
    }
    void click(const Cell&) override {}
    ~FormatPopup() override { if (font) DeleteObject(font); }
};

} // namespace

// ---------------------------------------------------------------------------

PopupMenus::PopupMenus() = default;

PopupMenus::~PopupMenus() { close(); }

// 排版、建視窗並顯示。視窗建不出來就當成沒開。
void PopupMenus::show(std::unique_ptr<PopupBase> popup, HWND owner, RenderDevice& device, RECT anchor, UiScale px) {
    close();
    if (current_) return; // 上一個選單自己的函式還沒回來（不該發生）：不開新的
    HINSTANCE inst = reinterpret_cast<HINSTANCE>(GetWindowLongPtrW(owner, GWLP_HINSTANCE));
    registerPopupClass(inst);
    popup->owner = owner;
    popup->device = &device;
    popup->surface.setDevice(device);
    popup->px = px;
    popup->closedAt = &lastClose_;
    const D2D1_SIZE_F size = popup->layout();
    const int w = static_cast<int>(size.width), h = static_cast<int>(size.height);
    int x = anchor.left, y = anchor.bottom + 2;
    HMONITOR mon = MonitorFromRect(&anchor, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{sizeof(mi)};
    GetMonitorInfoW(mon, &mi);
    if (x + w > mi.rcWork.right) x = mi.rcWork.right - w;
    if (y + h > mi.rcWork.bottom) y = anchor.top - h - 2;
    current_ = std::move(popup);
    HWND hwnd = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST, kPopupClass, L"", WS_POPUP | WS_BORDER, x, y, w, h, owner,
                                nullptr, inst, current_.get());
    if (!hwnd) {
        current_.reset();
        return;
    }
    ShowWindow(hwnd, SW_SHOW);
    SetForegroundWindow(hwnd);
    SetFocus(hwnd);
}

void PopupMenus::showLineStyle(HWND owner, RenderDevice& device, RECT anchor, UiScale px, StrokeStyle current,
                               std::function<void(const StrokeStyle&)> onChange) {
    auto p = std::make_unique<LineStylePopup>();
    p->cur = current;
    p->onChange = std::move(onChange);
    show(std::move(p), owner, device, anchor, px);
}

void PopupMenus::showColor(HWND owner, RenderDevice& device, RECT anchor, UiScale px, Color current, std::function<void(Color)> onChange) {
    auto p = std::make_unique<ColorPopup>();
    p->cur = current;
    p->onChange = std::move(onChange);
    show(std::move(p), owner, device, anchor, px);
}

void PopupMenus::showShapes(HWND owner, RenderDevice& device, RECT anchor, UiScale px, std::function<void(Kind)> onPick) {
    auto p = std::make_unique<ShapesPopup>();
    p->onPick = std::move(onPick);
    show(std::move(p), owner, device, anchor, px);
}

void PopupMenus::showFormat(HWND owner, RenderDevice& device, RECT anchor, UiScale px, TextStyle current,
                            std::function<void(const TextStyle&)> onChange) {
    auto p = std::make_unique<FormatPopup>();
    p->cur = current;
    p->onChange = std::move(onChange);
    show(std::move(p), owner, device, anchor, px);
}

bool PopupMenus::isOpen() const { return current_ && current_->hwnd; }

void PopupMenus::close() {
    if (current_ && current_->hwnd) DestroyWindow(current_->hwnd);
    reap();
}

void PopupMenus::reap() {
    if (current_ && !current_->hwnd && current_->inCall == 0) current_.reset();
}

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

void drawKindIcon(Surface& surface, Kind kind, D2D1_RECT_F box, D2D1_COLOR_F color) {
    ID2D1RenderTarget* rt = surface.rt();
    auto* br = surface.brush(color);
    float w = box.right - box.left, h = box.bottom - box.top;
    float cx = (box.left + box.right) / 2, cy = (box.top + box.bottom) / 2;
    float sw = std::max(1.5f, w * 0.07f);
    auto P = [](float x, float y) { return D2D1::Point2F(x, y); };
    auto outline = [&](const std::vector<Pt>& pts) { // 封閉的折線
        for (size_t i = 0; i < pts.size(); ++i) rt->DrawLine(toD2D(pts[i]), toD2D(pts[(i + 1) % pts.size()]), br, sw);
    };
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
        BubbleShape& bubble = *a.as<BubbleShape>();
        bubble.tailTip = {box.left + w * 0.2f, box.bottom};
        outline(bubbleOutline(bubble));
        break;
    }
    case Kind::Star: outline(starPoints(*makeShape(kind, {cx, cy}, std::min(w, h) / 0.3f, StrokeStyle{}).as<StarShape>())); break;
    case Kind::Polygon: outline(starPoints(*makeShape(kind, {cx, cy}, std::min(w, h) / 0.3f, StrokeStyle{}).as<PolygonShape>())); break;
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
                rt->FillRectangle(D2D1::RectF(box.left + x * cw, box.top + y * ch, box.left + (x + 1) * cw, box.top + (y + 1) * ch), surface.brush(c));
            }
        break;
    }
    case Kind::Freehand:
    case Kind::Text: break; // 不在形狀選單裡
    }
}

} // namespace mp
