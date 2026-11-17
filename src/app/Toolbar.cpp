#include "app/Toolbar.h"

#include <cmath>

#include "core/Lang.h"
#include "render/Surface.h"

namespace mp {

namespace {

// 版面（邏輯像素）。
constexpr float kBarMargin = 10;      // 工具列兩端到第一顆、最後一顆按鈕
constexpr float kBtnGap = 4;          // 按鈕之間
constexpr float kGroupGap = 12;       // 一組按鈕和下一組之間多空的
constexpr float kBtnRadius = 5;       // 按鈕 hover / 亮起的圓角
constexpr float kTextBtnRadius = 6;   // 文字按鈕的圓角
constexpr float kTextBtnPad = 28;     // 文字按鈕的寬度 = 字寬 + 這麼多
constexpr float kDropInset = 7;       // 下拉三角離按鈕右下角
constexpr float kDropStroke = 1.2f;

bool inRect(const D2D1_RECT_F& r, POINT p) { return p.x >= r.left && p.x < r.right && p.y >= r.top && p.y < r.bottom; }

// 這顆按鈕對應的指令；沒有回 IDM_NONE。
int commandOf(int button) {
    switch (button) {
    case BTN_ZOOMIN: return IDM_ZOOMIN;
    case BTN_ZOOMOUT: return IDM_ZOOMOUT;
    case BTN_ZOOM100: return IDM_ZOOM100;
    case BTN_ROTATE: return IDM_ROTATE;
    case BTN_MARKUP: return IDM_MARKUP;
    case BTN_INFO: return IDM_INFO;
    default: return IDM_NONE;
    }
}

} // namespace

void Toolbar::layout(const D2D1_RECT_F& toolbar, const D2D1_RECT_F& markup, const ToolbarState& state, UiScale px, RenderDevice& device) {
    buttons_.clear();
    stroke_ = state.stroke;
    fill_ = state.fill;
    const float bw = px(kBtnW), bh = px(kBtnH), gap = px(kBtnGap);
    auto add = [&](int id, float x, float y, bool dropdown = false) {
        Button b;
        b.id = id;
        b.rect = D2D1::RectF(x, y, x + bw, y + bh);
        b.dropdown = dropdown;
        buttons_.push_back(b);
        return x + bw + gap;
    };
    // 主工具列：左邊縮放與旋轉，右邊標示工具列與資訊面板的開關。
    float y = toolbar.top + (toolbar.bottom - toolbar.top - bh) / 2;
    float x = px(kBarMargin);
    x = add(BTN_ZOOMOUT, x, y);
    x = add(BTN_ZOOM100, x, y);
    x = add(BTN_ZOOMIN, x, y);
    x += px(kGroupGap);
    x = add(BTN_ROTATE, x, y);
    const float right = toolbar.right - px(kBarMargin) - bw;
    add(BTN_INFO, right, y);
    add(BTN_MARKUP, right - bw - gap, y);

    if (state.showMarkup) {
        y = markup.top + (markup.bottom - markup.top - bh) / 2;
        x = px(kBarMargin);
        x = add(BTN_FREEHAND, x, y);
        x = add(BTN_SHAPES, x, y, true);
        x = add(BTN_TEXT, x, y);
        x += px(kGroupGap);
        x = add(BTN_ADJCOLOR, x, y);
        x = add(BTN_ADJSIZE, x, y);
        x = add(BTN_CROP, x, y);
        x += px(kGroupGap);
        x = add(BTN_LINESTYLE, x, y, true);
        x = add(BTN_LINECOLOR, x, y, true);
        x = add(BTN_FILL, x, y, true);
        x = add(BTN_FORMAT, x, y, true);
        if (state.cropping) {
            // 裁切模式才有的文字按鈕，靠右。
            Button b;
            b.id = BTN_CROP_APPLY;
            const float w = device.measureUiText(tr(L"Crop"), px(kUiTextSize), false, 1e6f, nullptr) + px(kTextBtnPad);
            const float r = markup.right - px(kBarMargin);
            b.rect = D2D1::RectF(r - w, y, r, y + bh);
            buttons_.push_back(b);
        }
    }
    CommandState commands = state.commands;
    commands.dragging = false; // 按鈕的灰色不跟著拖曳變（拖曳中本來就按不到）
    for (Button& b : buttons_) {
        switch (b.id) {
        case BTN_ZOOM100: b.enabled = !state.zoomIsActual; break;
        case BTN_MARKUP: b.pressed = state.showMarkup; break;
        case BTN_INFO: b.pressed = state.showInfo; break;
        case BTN_FREEHAND: b.pressed = state.freehand; break;
        case BTN_ADJCOLOR: b.pressed = state.adjusting; break;
        case BTN_CROP: b.pressed = state.cropping; break;
        default: break;
        }
        // 對應到指令的按鈕還要指令能用（「1:1」另外要倍率不是 1）；其他的工具按鈕要有圖才能按。
        const int command = commandOf(b.id);
        if (command != IDM_NONE) b.enabled = b.enabled && commandEnabled(command, commands);
        else if (!state.hasImage) b.enabled = false;
    }
}

void Toolbar::draw(Surface& surface, UiScale px) const {
    for (const Button& b : buttons_) drawButton(surface, px, b);
}

void Toolbar::drawButton(Surface& s, UiScale px, const Button& b) const {
    ID2D1RenderTarget* rt = s.rt();
    const bool hot = hover == b.id || pressed == b.id;
    if (b.id == BTN_CROP_APPLY) {
        // 文字按鈕：灰底圓角，像 Preview 的「裁切」
        rt->FillRoundedRectangle(D2D1::RoundedRect(b.rect, px(kTextBtnRadius), px(kTextBtnRadius)),
                                 s.brush(hot ? theme::kTextBtnHot : theme::kTextBtn));
        s.drawUiText(tr(L"Crop"), b.rect, px(kUiTextSize), theme::kIcon, DWRITE_TEXT_ALIGNMENT_CENTER);
        return;
    }
    const float r = px(kBtnRadius);
    if (b.pressed) rt->FillRoundedRectangle(D2D1::RoundedRect(b.rect, r, r), s.brush(theme::kSelectedBg));
    else if (b.enabled && hot) rt->FillRoundedRectangle(D2D1::RoundedRect(b.rect, r, r), s.brush(theme::kBtnHover));
    const D2D1_COLOR_F c = b.enabled ? theme::kIcon : theme::kIconDisabled;
    // 圖示照 app/ToolbarIcons 的表畫；線條顏色與填色兩顆顯示目前的顏色。
    if (const auto* icon = toolbarIcon(b.id)) drawIcon(s, *icon, b.rect, px, c);
    else if (b.id == BTN_LINECOLOR) drawLineColorIcon(s, b.rect, px, stroke_, c);
    else if (b.id == BTN_FILL) drawFillIcon(s, b.rect, px, fill_, c);
    if (b.dropdown) {
        const float x = b.rect.right - px(kDropInset), y = b.rect.bottom - px(kDropInset);
        rt->DrawLine(D2D1::Point2F(x - px(3), y - px(1.5f)), D2D1::Point2F(x, y + px(1.5f)), s.brush(c), px(kDropStroke));
        rt->DrawLine(D2D1::Point2F(x, y + px(1.5f)), D2D1::Point2F(x + px(3), y - px(1.5f)), s.brush(c), px(kDropStroke));
    }
}

int Toolbar::hit(POINT pt, const Button** out) const {
    for (const Button& b : buttons_) {
        if (inRect(b.rect, pt)) {
            if (out) *out = &b;
            return b.id;
        }
    }
    return BTN_NONE;
}

const Button* Toolbar::find(int id) const {
    for (const Button& b : buttons_)
        if (b.id == id) return &b;
    return nullptr;
}

const wchar_t* Toolbar::name(int id) {
    switch (id) {
    case BTN_ZOOMOUT: return tr(L"Zoom Out");
    case BTN_ZOOM100: return tr(L"Actual Size");
    case BTN_ZOOMIN: return tr(L"Zoom In");
    case BTN_ROTATE: return tr(L"Rotate 90° Counterclockwise");
    case BTN_MARKUP: return tr(L"Markup Toolbar");
    case BTN_INFO: return tr(L"Info Panel");
    case BTN_FREEHAND: return tr(L"Sketch");
    case BTN_SHAPES: return tr(L"Shapes");
    case BTN_TEXT: return tr(L"Text");
    case BTN_ADJCOLOR: return tr(L"Adjust Color");
    case BTN_ADJSIZE: return tr(L"Adjust Size");
    case BTN_LINESTYLE: return tr(L"Line Style");
    case BTN_LINECOLOR: return tr(L"Line Color");
    case BTN_FILL: return tr(L"Fill Color");
    case BTN_FORMAT: return tr(L"Text Format");
    case BTN_CROP: return tr(L"Crop");
    default: return L"";
    }
}

} // namespace mp
