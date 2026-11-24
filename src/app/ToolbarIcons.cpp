#include "app/ToolbarIcons.h"

#include <map>

#include "render/Surface.h"

namespace mp {

namespace {

using Seg = IconSeg;
using Part = IconPart;

// ---- 寫圖示用的小工具 ----

Part line(float x0, float y0, float x1, float y1, float width = kIconStroke) { return {Part::Kind::Line, {x0, y0, x1, y1}, width}; }
Part rect(float l, float t, float r, float b) { return {Part::Kind::Rect, {l, t, r, b}}; }
Part ellipse(float cx, float cy, float rx, float ry) { return {Part::Kind::Ellipse, {cx, cy, rx, ry}}; }
Part dot(float cx, float cy, float r) { return {Part::Kind::Dot, {cx, cy, r}}; }
Part path(std::vector<Seg> segs) { return {Part::Kind::Path, {}, kIconStroke, std::move(segs)}; }
Part text(const wchar_t* s, float size, bool bold, float insetRight = 0) { return {Part::Kind::Text, {size, insetRight}, kIconStroke, {}, s, bold}; }

Seg M(float x, float y) { return {Seg::Kind::Move, {x, y}}; }
Seg L(float x, float y) { return {Seg::Kind::Line, {x, y}}; }
Seg C(float x1, float y1, float x2, float y2, float x, float y) { return {Seg::Kind::Bezier, {x1, y1, x2, y2, x, y}}; }
Seg A(float x, float y, float r) { return {Seg::Kind::Arc, {x, y, r}}; }
Seg Z() { return {Seg::Kind::Close, {}}; }

// ---- 圖示 ----

const std::map<int, std::vector<Part>>& icons() {
    static const std::map<int, std::vector<Part>> table = {
        // 放大鏡：鏡片、握把、鏡片裡的減號
        {BTN_ZOOMOUT, {ellipse(-1.5f, -1.5f, 6, 6), line(3, 3, 8, 8, kIconStroke * 1.4f), line(-5, -1.5f, 2, -1.5f)}},
        // 再加一豎變成加號
        {BTN_ZOOMIN,
         {ellipse(-1.5f, -1.5f, 6, 6), line(3, 3, 8, 8, kIconStroke * 1.4f), line(-5, -1.5f, 2, -1.5f), line(-1.5f, -5, -1.5f, 2)}},
        {BTN_ZOOM100, {text(L"1:1", 12, true)}},
        // 逆時針的弧，起點有箭頭
        {BTN_ROTATE, {path({M(0, -7), A(-4.9f, 4.9f, 7)}), line(0, -7, -4, -10), line(0, -7, -4, -4)}},
        // 2×2 的縮圖格（使用者 2026-11-24 挑的草稿 A）；往左上讓一點，右下角要畫下拉三角
        {BTN_BROWSE, {rect(-9.5f, -8, -2.5f, -1), rect(-0.5f, -8, 6.5f, -1), rect(-9.5f, 1, -2.5f, 8), rect(-0.5f, 1, 6.5f, 8)}},
        // 筆頭
        {BTN_MARKUP, {path({M(6, -7), L(8, -5), L(-4, 7), L(-8, 8), L(-7, 4), Z()})}},
        // 圓圈裡一個 i
        {BTN_INFO, {ellipse(0, 0, 8, 8), line(0, -1, 0, 5, kIconStroke * 1.3f), dot(0, -4.5f, kIconStroke * 0.8f)}},
        // 一筆彎彎的線
        {BTN_FREEHAND, {path({M(-9, 5), C(-6, -12, -2, 12, 1, -2), C(3, -10, 5, 8, 9, -4)})}},
        // 方塊與圓
        {BTN_SHAPES, {rect(-9, -7, -1, 3), ellipse(4, 1, 5, 5)}},
        {BTN_TEXT, {text(L"A", 18, false)}},
        // 三條滑桿，各有一個鈕
        {BTN_ADJCOLOR,
         {line(-9, -6, 9, -6), dot(-3, -6, 2.2f), line(-9, 0, 9, 0), dot(4, 0, 2.2f), line(-9, 6, 9, 6), dot(-6, 6, 2.2f)}},
        // 方框裡一個指向右上的箭頭
        {BTN_ADJSIZE, {rect(-9, -7, 9, 7), line(-5, 3, 5, -3), line(5, -3, 1, -3), line(5, -3, 5, 1)}},
        // 兩個交錯的 L
        {BTN_CROP, {path({M(-5, -9), L(-5, 5), L(9, 5), M(-9, -5), L(5, -5), L(5, 9)})}},
        // 三條由細到粗的線
        {BTN_LINESTYLE, {line(-9, -6, 7, -6, 1.0f), line(-9, 0, 7, 0, 2.5f), line(-9, 6, 7, 6, 4.0f)}},
        // 右下角有下拉三角形，字往左讓一點
        {BTN_FORMAT, {text(L"Aa", 14, true, 5)}},
    };
    return table;
}

// 路徑做成幾何：每個 Move 開始新的一筆，有 Close 的那一筆是封閉的。
ComPtr<ID2D1PathGeometry> pathGeometry(RenderDevice& dev, const std::vector<Seg>& segs, float cx, float cy, UiScale px) {
    auto P = [&](float x, float y) { return D2D1::Point2F(cx + px(x), cy + px(y)); };
    return dev.pathGeometry([&](ID2D1GeometrySink& sink) {
        bool open = false;
        for (size_t i = 0; i < segs.size(); ++i) {
            const Seg& s = segs[i];
            switch (s.kind) {
            case Seg::Kind::Move: {
                if (open) sink.EndFigure(D2D1_FIGURE_END_OPEN);
                bool closes = false; // 這一筆後面有沒有 Close
                for (size_t j = i + 1; j < segs.size() && segs[j].kind != Seg::Kind::Move; ++j)
                    closes = closes || segs[j].kind == Seg::Kind::Close;
                sink.BeginFigure(P(s.v[0], s.v[1]), closes ? D2D1_FIGURE_BEGIN_FILLED : D2D1_FIGURE_BEGIN_HOLLOW);
                open = true;
                break;
            }
            case Seg::Kind::Line: sink.AddLine(P(s.v[0], s.v[1])); break;
            case Seg::Kind::Bezier: sink.AddBezier(D2D1::BezierSegment(P(s.v[0], s.v[1]), P(s.v[2], s.v[3]), P(s.v[4], s.v[5]))); break;
            case Seg::Kind::Arc:
                sink.AddArc(D2D1::ArcSegment(P(s.v[0], s.v[1]), D2D1::SizeF(px(s.v[2]), px(s.v[2])), 0,
                                             D2D1_SWEEP_DIRECTION_COUNTER_CLOCKWISE, D2D1_ARC_SIZE_LARGE));
                break;
            case Seg::Kind::Close:
                if (open) sink.EndFigure(D2D1_FIGURE_END_CLOSED);
                open = false;
                break;
            }
        }
        if (open) sink.EndFigure(D2D1_FIGURE_END_OPEN);
    });
}

// 「無色」的紅斜線：從左下到右上。
void drawNoColorSlash(Surface& s, D2D1_POINT_2F from, D2D1_POINT_2F to, UiScale px) {
    s.rt()->DrawLine(from, to, s.brush(theme::kNoColorSlash), px(kIconStroke));
}

} // namespace

const std::vector<IconPart>* toolbarIcon(int buttonId) {
    auto it = icons().find(buttonId);
    return it == icons().end() ? nullptr : &it->second;
}

void drawIcon(Surface& s, const std::vector<IconPart>& icon, const D2D1_RECT_F& button, UiScale px, D2D1_COLOR_F color) {
    ID2D1RenderTarget* rt = s.rt();
    auto* br = s.brush(color);
    const float cx = (button.left + button.right) / 2, cy = (button.top + button.bottom) / 2;
    auto P = [&](float x, float y) { return D2D1::Point2F(cx + px(x), cy + px(y)); };
    for (const IconPart& p : icon) {
        const float* v = p.v;
        switch (p.kind) {
        case IconPart::Kind::Line: rt->DrawLine(P(v[0], v[1]), P(v[2], v[3]), br, px(p.width)); break;
        case IconPart::Kind::Rect:
            rt->DrawRectangle(D2D1::RectF(cx + px(v[0]), cy + px(v[1]), cx + px(v[2]), cy + px(v[3])), br, px(p.width));
            break;
        case IconPart::Kind::Ellipse: rt->DrawEllipse(D2D1::Ellipse(P(v[0], v[1]), px(v[2]), px(v[3])), br, px(p.width)); break;
        case IconPart::Kind::Dot: rt->FillEllipse(D2D1::Ellipse(P(v[0], v[1]), px(v[2]), px(v[2])), br); break;
        case IconPart::Kind::Path:
            if (auto geo = pathGeometry(s.device(), p.path, cx, cy, px)) rt->DrawGeometry(geo.Get(), br, px(p.width));
            break;
        case IconPart::Kind::Text:
            s.drawUiText(p.text, D2D1::RectF(button.left, button.top, button.right - px(v[1]), button.bottom), px(v[0]), color,
                         DWRITE_TEXT_ALIGNMENT_CENTER, p.bold);
            break;
        }
    }
}

void drawLineColorIcon(Surface& s, const D2D1_RECT_F& button, UiScale px, Color stroke, D2D1_COLOR_F iconColor) {
    ID2D1RenderTarget* rt = s.rt();
    const float cx = (button.left + button.right) / 2 - px(1), cy = (button.top + button.bottom) / 2;
    const float r = px(7);
    const D2D1_ELLIPSE ring = D2D1::Ellipse(D2D1::Point2F(cx, cy), r, r);
    if (stroke.none()) {
        rt->DrawEllipse(ring, s.brush(iconColor), px(kIconStroke));
        drawNoColorSlash(s, D2D1::Point2F(cx - r * 0.7f, cy + r * 0.7f), D2D1::Point2F(cx + r * 0.7f, cy - r * 0.7f), px);
    } else {
        rt->DrawEllipse(ring, s.brush(stroke), px(3.5f));
    }
}

void drawFillIcon(Surface& s, const D2D1_RECT_F& button, UiScale px, Color fill, D2D1_COLOR_F iconColor) {
    ID2D1RenderTarget* rt = s.rt();
    const float cx = (button.left + button.right) / 2, cy = (button.top + button.bottom) / 2;
    const D2D1_RECT_F box = D2D1::RectF(cx - px(8), cy - px(7), cx + px(6), cy + px(7));
    if (fill.none()) {
        rt->DrawRectangle(box, s.brush(iconColor), px(kIconStroke));
        drawNoColorSlash(s, D2D1::Point2F(box.left, box.bottom), D2D1::Point2F(box.right, box.top), px);
    } else {
        rt->FillRectangle(box, s.brush(fill));
        rt->DrawRectangle(box, s.brush(iconColor), px(1)); // 白色或很淺的填色也看得到邊
    }
}

} // namespace mp
