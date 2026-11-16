#include "render/AnnotationPainter.h"

#include <algorithm>
#include <cmath>

#include "core/Fonts.h"

namespace mp {

namespace {

// 圓角矩形的圓角半徑佔短邊的比例。
constexpr float kRoundRectRadiusRatio = 0.2f;
// 放大鏡沒有設線條顏色時的框線顏色。
constexpr Color kMagnifierDefaultRing = {80, 80, 80, 255};

// ---- 描邊與填色 ----

void strokeGeometry(Surface& s, ID2D1Geometry* geo, const StrokeStyle& style) {
    if (!geo || style.stroke.none()) return;
    ID2D1RenderTarget* rt = s.rt();
    RenderDevice& dev = s.device();
    const float w = style.width;
    const D2D1_COLOR_F c = toD2D(style.stroke);
    switch (style.line) {
    case LineStyle::Solid:
        rt->DrawGeometry(geo, s.brush(c), w, dev.roundStyle());
        break;
    case LineStyle::Dashed:
        rt->DrawGeometry(geo, s.brush(c), w, dev.dashStyle());
        break;
    case LineStyle::Fuzzy:
        for (const FuzzyLayer& layer : kFuzzyLayers) {
            D2D1_COLOR_F faint = c;
            faint.a = c.a * layer.alpha;
            rt->DrawGeometry(geo, s.brush(faint), w * layer.widthScale, dev.roundStyle());
        }
        break;
    }
}

void fillAndStroke(Surface& s, ID2D1Geometry* geo, const StrokeStyle& style) {
    if (!geo) return;
    if (!style.fill.none()) s.rt()->FillGeometry(geo, s.brush(style.fill));
    strokeGeometry(s, geo, style);
}

ComPtr<ID2D1PathGeometry> polygonGeometry(RenderDevice& dev, const std::vector<Pt>& pts, bool closed) {
    if (pts.empty()) return nullptr;
    return dev.pathGeometry([&](ID2D1GeometrySink& sink) {
        sink.BeginFigure(toD2D(pts[0]), closed ? D2D1_FIGURE_BEGIN_FILLED : D2D1_FIGURE_BEGIN_HOLLOW);
        for (size_t i = 1; i < pts.size(); ++i) sink.AddLine(toD2D(pts[i]));
        sink.EndFigure(closed ? D2D1_FIGURE_END_CLOSED : D2D1_FIGURE_END_OPEN);
    });
}

// ---- 文字排版 ----

ComPtr<IDWriteTextLayout> textLayout(RenderDevice& dev, const TextShape& t, float width) {
    IDWriteFactory* dw = dev.dwrite();
    if (!dw) return nullptr;
    ComPtr<IDWriteTextFormat> fmt;
    HRESULT hr = dw->CreateTextFormat(t.style.font.c_str(), nullptr,
                                      t.style.bold ? DWRITE_FONT_WEIGHT_BOLD : DWRITE_FONT_WEIGHT_NORMAL,
                                      t.style.italic ? DWRITE_FONT_STYLE_ITALIC : DWRITE_FONT_STYLE_NORMAL,
                                      DWRITE_FONT_STRETCH_NORMAL, std::max(1.0f, t.style.size), kTextLocale, &fmt);
    if (FAILED(hr)) return nullptr;
    // 空字串也排一個空白：方塊至少有一行高。
    const std::wstring& text = t.value.empty() ? std::wstring(L" ") : t.value;
    ComPtr<IDWriteTextLayout> layout;
    hr = dw->CreateTextLayout(text.c_str(), static_cast<UINT32>(text.size()), fmt.Get(), std::max(1.0f, width), 1e6f, &layout);
    if (FAILED(hr)) return nullptr;
    DWRITE_TEXT_ALIGNMENT al = DWRITE_TEXT_ALIGNMENT_LEADING;
    switch (t.style.align) {
    case TextAlign::Left: break;
    case TextAlign::Center: al = DWRITE_TEXT_ALIGNMENT_CENTER; break;
    case TextAlign::Right: al = DWRITE_TEXT_ALIGNMENT_TRAILING; break;
    case TextAlign::Justify: al = DWRITE_TEXT_ALIGNMENT_JUSTIFIED; break;
    }
    layout->SetTextAlignment(al);
    layout->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
    if (t.style.underline) {
        DWRITE_TEXT_RANGE range{0, static_cast<UINT32>(text.size())};
        layout->SetUnderline(TRUE, range);
    }
    return layout;
}

// ---- 每種物件一個多載（drawAnnotation 用 std::visit 呼叫；加一種物件時漏寫編譯不過）。id 只有馬賽克的快取要用。----

void draw(const PaintCtx& ctx, uint32_t, const StrokeStyle& style, const FreehandShape& f) {
    if (f.points.empty() || style.stroke.none()) return;
    Surface& s = *ctx.surface;
    ID2D1RenderTarget* rt = s.rt();
    // 在原始座標畫，用 transform 做非等比縮放，線寬跟著變。
    D2D1_MATRIX_3X2_F old;
    rt->GetTransform(&old);
    float sx = f.rect.width() / f.origW, sy = f.rect.height() / f.origH;
    D2D1_MATRIX_3X2_F local = D2D1::Matrix3x2F::Scale(sx, sy) * D2D1::Matrix3x2F::Translation(f.rect.left, f.rect.top);
    rt->SetTransform(local * old);
    if (f.points.size() == 1) {
        float r = style.width * 0.5f;
        rt->FillEllipse(D2D1::Ellipse(toD2D(f.points[0]), r, r), s.brush(style.stroke));
    } else {
        strokeGeometry(s, polygonGeometry(s.device(), f.points, false).Get(), style);
    }
    rt->SetTransform(old);
}

void draw(const PaintCtx& ctx, uint32_t, const StrokeStyle& style, const LineShape& l) {
    Surface& s = *ctx.surface;
    ID2D1RenderTarget* rt = s.rt();
    RenderDevice& dev = s.device();
    const Pt c = lineControlPoint(l);
    const bool straight = dist(l.mid, lerp(l.p0, l.p1, 0.5f)) < 0.01f;
    auto geo = dev.pathGeometry([&](ID2D1GeometrySink& sink) {
        sink.BeginFigure(toD2D(l.p0), D2D1_FIGURE_BEGIN_HOLLOW);
        if (straight) sink.AddLine(toD2D(l.p1));
        else sink.AddQuadraticBezier(D2D1::QuadraticBezierSegment(toD2D(c), toD2D(l.p1)));
        sink.EndFigure(D2D1_FIGURE_END_OPEN);
    });
    strokeGeometry(s, geo.Get(), style);
    if (style.stroke.none()) return;
    auto drawHead = [&](Pt tip, Pt dir) {
        auto tri = polygonGeometry(dev, arrowHead(tip, dir, style.width), true);
        if (!tri) return;
        ID2D1SolidColorBrush* b = s.brush(style.stroke);
        rt->FillGeometry(tri.Get(), b);
        // 描一圈圓角的邊，箭頭的三個角才不會太尖。
        rt->DrawGeometry(tri.Get(), b, std::max(1.0f, style.width * 0.5f), dev.roundStyle());
    };
    if (style.arrows == ArrowMode::End || style.arrows == ArrowMode::Both) drawHead(l.p1, straight ? l.p1 - l.p0 : l.p1 - c);
    if (style.arrows == ArrowMode::Start || style.arrows == ArrowMode::Both) drawHead(l.p0, straight ? l.p0 - l.p1 : l.p0 - c);
}

void draw(const PaintCtx& ctx, uint32_t, const StrokeStyle& style, const RectShape& r) {
    ComPtr<ID2D1RectangleGeometry> geo;
    ctx.surface->device().d2d()->CreateRectangleGeometry(toD2D(r.rect), &geo);
    fillAndStroke(*ctx.surface, geo.Get(), style);
}

void draw(const PaintCtx& ctx, uint32_t, const StrokeStyle& style, const RoundRectShape& r) {
    float radius = std::min(r.rect.width(), r.rect.height()) * kRoundRectRadiusRatio;
    ComPtr<ID2D1RoundedRectangleGeometry> geo;
    ctx.surface->device().d2d()->CreateRoundedRectangleGeometry(D2D1::RoundedRect(toD2D(r.rect), radius, radius), &geo);
    fillAndStroke(*ctx.surface, geo.Get(), style);
}

void draw(const PaintCtx& ctx, uint32_t, const StrokeStyle& style, const EllipseShape& e) {
    ComPtr<ID2D1EllipseGeometry> geo;
    ctx.surface->device().d2d()->CreateEllipseGeometry(
        D2D1::Ellipse(toD2D(e.rect.center()), e.rect.width() / 2, e.rect.height() / 2), &geo);
    fillAndStroke(*ctx.surface, geo.Get(), style);
}

void draw(const PaintCtx& ctx, uint32_t, const StrokeStyle& style, const BubbleShape& b) {
    Pt ra, rb, tip;
    bubbleTail(b, ra, rb, tip);
    // 從尾巴根部的一點沿橢圓繞大弧到另一點，再連到尖端。
    auto geo = ctx.surface->device().pathGeometry([&](ID2D1GeometrySink& sink) {
        sink.BeginFigure(toD2D(ra), D2D1_FIGURE_BEGIN_FILLED);
        sink.AddArc(D2D1::ArcSegment(toD2D(rb), D2D1::SizeF(b.rect.width() / 2, b.rect.height() / 2), 0.0f,
                                     D2D1_SWEEP_DIRECTION_CLOCKWISE, D2D1_ARC_SIZE_LARGE));
        sink.AddLine(toD2D(tip));
        sink.EndFigure(D2D1_FIGURE_END_CLOSED);
    });
    fillAndStroke(*ctx.surface, geo.Get(), style);
}

void draw(const PaintCtx& ctx, uint32_t, const StrokeStyle& style, const StarShape& star) {
    fillAndStroke(*ctx.surface, polygonGeometry(ctx.surface->device(), starPoints(star), true).Get(), style);
}

void draw(const PaintCtx& ctx, uint32_t, const StrokeStyle& style, const PolygonShape& poly) {
    fillAndStroke(*ctx.surface, polygonGeometry(ctx.surface->device(), starPoints(poly), true).Get(), style);
}

void draw(const PaintCtx& ctx, uint32_t, const StrokeStyle& style, const MagnifierShape& m) {
    Surface& s = *ctx.surface;
    ID2D1RenderTarget* rt = s.rt();
    ComPtr<ID2D1EllipseGeometry> circle;
    if (FAILED(s.device().d2d()->CreateEllipseGeometry(D2D1::Ellipse(toD2D(m.center), m.radius, m.radius), &circle))) return;
    if (ctx.imageBmp && ctx.imageBmp->bitmap) {
        rt->PushLayer(D2D1::LayerParameters(D2D1::InfiniteRect(), circle.Get()), nullptr);
        float scale = ctx.imageBmp->scale;
        float srcR = m.radius / m.zoom;
        D2D1_RECT_F src = D2D1::RectF((m.center.x - srcR) * scale, (m.center.y - srcR) * scale, (m.center.x + srcR) * scale,
                                      (m.center.y + srcR) * scale);
        D2D1_RECT_F dst = D2D1::RectF(m.center.x - m.radius, m.center.y - m.radius, m.center.x + m.radius, m.center.y + m.radius);
        rt->DrawBitmap(ctx.imageBmp->bitmap.Get(), dst, 1.0f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR, src);
        rt->PopLayer();
    }
    StrokeStyle ring = style;
    if (ring.stroke.none()) ring.stroke = kMagnifierDefaultRing;
    strokeGeometry(s, circle.Get(), ring);
}

void draw(const PaintCtx& ctx, uint32_t id, const StrokeStyle&, const MosaicShape& m) {
    if (!ctx.image || !ctx.mosaic) return;
    ID2D1RenderTarget* rt = ctx.surface->rt();
    const MosaicCache::Tile* tile = ctx.mosaic->get(rt, ctx.image, id, m);
    if (!tile) return;
    D2D1_SIZE_U cells = tile->bitmap->GetPixelSize();
    float bw = static_cast<float>(cells.width * m.blockSize), bh = static_cast<float>(cells.height * m.blockSize);
    D2D1_RECT_F dst = D2D1::RectF(static_cast<float>(tile->x0), static_cast<float>(tile->y0), tile->x0 + bw, tile->y0 + bh);
    // 只畫在圖片範圍內與物件矩形內。
    D2D1_RECT_F clip = D2D1::RectF(std::max(m.rect.left, 0.0f), std::max(m.rect.top, 0.0f),
                                   std::min(m.rect.right, static_cast<float>(ctx.image->width)),
                                   std::min(m.rect.bottom, static_cast<float>(ctx.image->height)));
    rt->PushAxisAlignedClip(clip, D2D1_ANTIALIAS_MODE_ALIASED);
    rt->DrawBitmap(tile->bitmap.Get(), dst, 1.0f, D2D1_BITMAP_INTERPOLATION_MODE_NEAREST_NEIGHBOR);
    rt->PopAxisAlignedClip();
}

void draw(const PaintCtx& ctx, uint32_t, const StrokeStyle& style, const TextShape& t) {
    Surface& s = *ctx.surface;
    ID2D1RenderTarget* rt = s.rt();
    D2D1_RECT_F box = toD2D(t.rect);
    if (!style.fill.none()) rt->FillRectangle(box, s.brush(style.fill));
    if (!style.stroke.none()) {
        ComPtr<ID2D1RectangleGeometry> geo;
        s.device().d2d()->CreateRectangleGeometry(box, &geo);
        strokeGeometry(s, geo.Get(), style);
    }
    if (t.value.empty()) return;
    auto layout = textLayout(s.device(), t, t.rect.width() - 2 * kTextPad);
    if (layout)
        rt->DrawTextLayout(D2D1::Point2F(t.rect.left + kTextPad, t.rect.top + kTextPad), layout.Get(), s.brush(t.style.color),
                           D2D1_DRAW_TEXT_OPTIONS_CLIP);
}

} // namespace

void drawImage(const PaintCtx& ctx) {
    if (!ctx.surface || !ctx.surface->rt() || !ctx.imageBmp || !ctx.imageBmp->bitmap || !ctx.image) return;
    D2D1_RECT_F dest = D2D1::RectF(0, 0, static_cast<float>(ctx.image->width), static_cast<float>(ctx.image->height));
    ctx.surface->rt()->DrawBitmap(ctx.imageBmp->bitmap.Get(), dest, 1.0f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
}

void drawAnnotations(const PaintCtx& ctx, const std::vector<Annotation>& items, uint32_t skipId) {
    for (const auto& a : items)
        if (a.id != skipId) drawAnnotation(ctx, a);
}

void drawAnnotation(const PaintCtx& ctx, const Annotation& a) {
    if (!ctx.surface || !ctx.surface->rt()) return;
    std::visit([&](const auto& shape) { draw(ctx, a.id, a.stroke, shape); }, a.shape);
}

float textHeight(RenderDevice& device, const TextShape& t) {
    auto layout = textLayout(device, t, t.rect.width() - 2 * kTextPad);
    if (!layout) return t.style.size * 1.5f + 2 * kTextPad;
    DWRITE_TEXT_METRICS m{};
    layout->GetMetrics(&m);
    return m.height + 2 * kTextPad;
}

} // namespace mp
