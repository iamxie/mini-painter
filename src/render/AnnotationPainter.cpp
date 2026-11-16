#include "render/AnnotationPainter.h"

#include <algorithm>
#include <cmath>

namespace mp {

namespace {

// 文字方塊的字和邊框之間的留白（圖片像素）。
constexpr float kTextPad = 4.0f;
// 圓角矩形的圓角半徑佔短邊的比例。
constexpr float kRoundRectRadiusRatio = 0.2f;
// 放大鏡沒有設線條顏色時的框線顏色。
constexpr Color kMagnifierDefaultRing = {80, 80, 80, 255};

// ---- 描邊與填色 ----

void strokeGeometry(Surface& s, ID2D1Geometry* geo, const StrokeStyle& style) {
    if (!geo || style.stroke.none()) return;
    ID2D1RenderTarget* rt = s.rt();
    RenderDevice& dev = s.device();
    const float w = style.width();
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

ComPtr<IDWriteTextLayout> textLayout(RenderDevice& dev, const Annotation& a, float width) {
    IDWriteFactory* dw = dev.dwrite();
    if (!dw) return nullptr;
    ComPtr<IDWriteTextFormat> fmt;
    HRESULT hr = dw->CreateTextFormat(a.text.font.c_str(), nullptr,
                                      a.text.bold ? DWRITE_FONT_WEIGHT_BOLD : DWRITE_FONT_WEIGHT_NORMAL,
                                      a.text.italic ? DWRITE_FONT_STYLE_ITALIC : DWRITE_FONT_STYLE_NORMAL,
                                      DWRITE_FONT_STRETCH_NORMAL, std::max(1.0f, a.text.size), L"zh-TW", &fmt);
    if (FAILED(hr)) return nullptr;
    // 空字串也排一個空白：方塊至少有一行高。
    const std::wstring& text = a.textValue.empty() ? std::wstring(L" ") : a.textValue;
    ComPtr<IDWriteTextLayout> layout;
    hr = dw->CreateTextLayout(text.c_str(), static_cast<UINT32>(text.size()), fmt.Get(), std::max(1.0f, width), 1e6f, &layout);
    if (FAILED(hr)) return nullptr;
    DWRITE_TEXT_ALIGNMENT al = DWRITE_TEXT_ALIGNMENT_LEADING;
    switch (a.text.align) {
    case TextAlign::Left: break;
    case TextAlign::Center: al = DWRITE_TEXT_ALIGNMENT_CENTER; break;
    case TextAlign::Right: al = DWRITE_TEXT_ALIGNMENT_TRAILING; break;
    case TextAlign::Justify: al = DWRITE_TEXT_ALIGNMENT_JUSTIFIED; break;
    }
    layout->SetTextAlignment(al);
    layout->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
    if (a.text.underline) {
        DWRITE_TEXT_RANGE range{0, static_cast<UINT32>(text.size())};
        layout->SetUnderline(TRUE, range);
    }
    return layout;
}

// ---- 每種物件 ----

void drawFreehand(const PaintCtx& ctx, const Annotation& a) {
    if (a.strokePoints.empty() || a.stroke.stroke.none()) return;
    Surface& s = *ctx.surface;
    ID2D1RenderTarget* rt = s.rt();
    // 在原始座標畫，用 transform 做非等比縮放，線寬跟著變。
    D2D1_MATRIX_3X2_F old;
    rt->GetTransform(&old);
    float sx = a.rect.width() / a.origW, sy = a.rect.height() / a.origH;
    D2D1_MATRIX_3X2_F local = D2D1::Matrix3x2F::Scale(sx, sy) * D2D1::Matrix3x2F::Translation(a.rect.left, a.rect.top);
    rt->SetTransform(local * old);
    if (a.strokePoints.size() == 1) {
        float r = a.stroke.width() * 0.5f;
        rt->FillEllipse(D2D1::Ellipse(toD2D(a.strokePoints[0]), r, r), s.brush(a.stroke.stroke));
    } else {
        strokeGeometry(s, polygonGeometry(s.device(), a.strokePoints, false).Get(), a.stroke);
    }
    rt->SetTransform(old);
}

void drawLine(const PaintCtx& ctx, const Annotation& a) {
    Surface& s = *ctx.surface;
    ID2D1RenderTarget* rt = s.rt();
    RenderDevice& dev = s.device();
    const Pt c = lineControlPoint(a);
    const bool straight = dist(a.mid, lerp(a.p0, a.p1, 0.5f)) < 0.01f;
    auto geo = dev.pathGeometry([&](ID2D1GeometrySink& sink) {
        sink.BeginFigure(toD2D(a.p0), D2D1_FIGURE_BEGIN_HOLLOW);
        if (straight) sink.AddLine(toD2D(a.p1));
        else sink.AddQuadraticBezier(D2D1::QuadraticBezierSegment(toD2D(c), toD2D(a.p1)));
        sink.EndFigure(D2D1_FIGURE_END_OPEN);
    });
    strokeGeometry(s, geo.Get(), a.stroke);
    if (a.stroke.stroke.none()) return;
    auto drawHead = [&](Pt tip, Pt dir) {
        auto tri = polygonGeometry(dev, arrowHead(tip, dir, a.stroke.width()), true);
        if (!tri) return;
        ID2D1SolidColorBrush* b = s.brush(a.stroke.stroke);
        rt->FillGeometry(tri.Get(), b);
        // 描一圈圓角的邊，箭頭的三個角才不會太尖。
        rt->DrawGeometry(tri.Get(), b, std::max(1.0f, a.stroke.width() * 0.5f), dev.roundStyle());
    };
    if (a.stroke.arrows == ArrowMode::End || a.stroke.arrows == ArrowMode::Both) drawHead(a.p1, straight ? a.p1 - a.p0 : a.p1 - c);
    if (a.stroke.arrows == ArrowMode::Start || a.stroke.arrows == ArrowMode::Both) drawHead(a.p0, straight ? a.p0 - a.p1 : a.p0 - c);
}

void drawRect(const PaintCtx& ctx, const Annotation& a) {
    ComPtr<ID2D1RectangleGeometry> geo;
    ctx.surface->device().d2d()->CreateRectangleGeometry(toD2D(a.rect), &geo);
    fillAndStroke(*ctx.surface, geo.Get(), a.stroke);
}

void drawRoundRect(const PaintCtx& ctx, const Annotation& a) {
    float r = std::min(a.rect.width(), a.rect.height()) * kRoundRectRadiusRatio;
    ComPtr<ID2D1RoundedRectangleGeometry> geo;
    ctx.surface->device().d2d()->CreateRoundedRectangleGeometry(D2D1::RoundedRect(toD2D(a.rect), r, r), &geo);
    fillAndStroke(*ctx.surface, geo.Get(), a.stroke);
}

void drawEllipse(const PaintCtx& ctx, const Annotation& a) {
    ComPtr<ID2D1EllipseGeometry> geo;
    ctx.surface->device().d2d()->CreateEllipseGeometry(
        D2D1::Ellipse(toD2D(a.rect.center()), a.rect.width() / 2, a.rect.height() / 2), &geo);
    fillAndStroke(*ctx.surface, geo.Get(), a.stroke);
}

void drawBubble(const PaintCtx& ctx, const Annotation& a) {
    Pt ra, rb, tip;
    bubbleTail(a, ra, rb, tip);
    // 從尾巴根部的一點沿橢圓繞大弧到另一點，再連到尖端。
    auto geo = ctx.surface->device().pathGeometry([&](ID2D1GeometrySink& sink) {
        sink.BeginFigure(toD2D(ra), D2D1_FIGURE_BEGIN_FILLED);
        sink.AddArc(D2D1::ArcSegment(toD2D(rb), D2D1::SizeF(a.rect.width() / 2, a.rect.height() / 2), 0.0f,
                                     D2D1_SWEEP_DIRECTION_CLOCKWISE, D2D1_ARC_SIZE_LARGE));
        sink.AddLine(toD2D(tip));
        sink.EndFigure(D2D1_FIGURE_END_CLOSED);
    });
    fillAndStroke(*ctx.surface, geo.Get(), a.stroke);
}

void drawStar(const PaintCtx& ctx, const Annotation& a) {
    fillAndStroke(*ctx.surface, polygonGeometry(ctx.surface->device(), starPoints(a), true).Get(), a.stroke);
}

void drawMagnifier(const PaintCtx& ctx, const Annotation& a) {
    Surface& s = *ctx.surface;
    ID2D1RenderTarget* rt = s.rt();
    ComPtr<ID2D1EllipseGeometry> circle;
    if (FAILED(s.device().d2d()->CreateEllipseGeometry(D2D1::Ellipse(toD2D(a.center), a.radius, a.radius), &circle))) return;
    if (ctx.imageBmp && ctx.imageBmp->bitmap) {
        rt->PushLayer(D2D1::LayerParameters(D2D1::InfiniteRect(), circle.Get()), nullptr);
        float scale = ctx.imageBmp->scale;
        float srcR = a.radius / a.zoom;
        D2D1_RECT_F src = D2D1::RectF((a.center.x - srcR) * scale, (a.center.y - srcR) * scale, (a.center.x + srcR) * scale,
                                      (a.center.y + srcR) * scale);
        D2D1_RECT_F dst = D2D1::RectF(a.center.x - a.radius, a.center.y - a.radius, a.center.x + a.radius, a.center.y + a.radius);
        rt->DrawBitmap(ctx.imageBmp->bitmap.Get(), dst, 1.0f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR, src);
        rt->PopLayer();
    }
    StrokeStyle ring = a.stroke;
    if (ring.stroke.none()) ring.stroke = kMagnifierDefaultRing;
    strokeGeometry(s, circle.Get(), ring);
}

void drawMosaic(const PaintCtx& ctx, const Annotation& a) {
    if (!ctx.image || !ctx.mosaic) return;
    ID2D1RenderTarget* rt = ctx.surface->rt();
    const MosaicCache::Tile* tile = ctx.mosaic->get(rt, ctx.image, a);
    if (!tile) return;
    D2D1_SIZE_U cells = tile->bitmap->GetPixelSize();
    float bw = static_cast<float>(cells.width * a.blockSize), bh = static_cast<float>(cells.height * a.blockSize);
    D2D1_RECT_F dst = D2D1::RectF(static_cast<float>(tile->x0), static_cast<float>(tile->y0), tile->x0 + bw, tile->y0 + bh);
    // 只畫在圖片範圍內與物件矩形內。
    D2D1_RECT_F clip = D2D1::RectF(std::max(a.rect.left, 0.0f), std::max(a.rect.top, 0.0f),
                                   std::min(a.rect.right, static_cast<float>(ctx.image->width)),
                                   std::min(a.rect.bottom, static_cast<float>(ctx.image->height)));
    rt->PushAxisAlignedClip(clip, D2D1_ANTIALIAS_MODE_ALIASED);
    rt->DrawBitmap(tile->bitmap.Get(), dst, 1.0f, D2D1_BITMAP_INTERPOLATION_MODE_NEAREST_NEIGHBOR);
    rt->PopAxisAlignedClip();
}

void drawText(const PaintCtx& ctx, const Annotation& a) {
    Surface& s = *ctx.surface;
    ID2D1RenderTarget* rt = s.rt();
    D2D1_RECT_F box = toD2D(a.rect);
    if (!a.stroke.fill.none()) rt->FillRectangle(box, s.brush(a.stroke.fill));
    if (!a.stroke.stroke.none()) {
        ComPtr<ID2D1RectangleGeometry> geo;
        s.device().d2d()->CreateRectangleGeometry(box, &geo);
        strokeGeometry(s, geo.Get(), a.stroke);
    }
    if (a.textValue.empty()) return;
    auto layout = textLayout(s.device(), a, a.rect.width() - 2 * kTextPad);
    if (layout)
        rt->DrawTextLayout(D2D1::Point2F(a.rect.left + kTextPad, a.rect.top + kTextPad), layout.Get(), s.brush(a.text.color),
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
    switch (a.kind) {
    case Kind::Freehand: drawFreehand(ctx, a); return;
    case Kind::Line: drawLine(ctx, a); return;
    case Kind::Rect: drawRect(ctx, a); return;
    case Kind::RoundRect: drawRoundRect(ctx, a); return;
    case Kind::Ellipse: drawEllipse(ctx, a); return;
    case Kind::Bubble: drawBubble(ctx, a); return;
    case Kind::Star:
    case Kind::Polygon: drawStar(ctx, a); return;
    case Kind::Magnifier: drawMagnifier(ctx, a); return;
    case Kind::Mosaic: drawMosaic(ctx, a); return;
    case Kind::Text: drawText(ctx, a); return;
    }
}

float textHeight(RenderDevice& device, const Annotation& a) {
    auto layout = textLayout(device, a, a.rect.width() - 2 * kTextPad);
    if (!layout) return a.text.size * 1.5f + 2 * kTextPad;
    DWRITE_TEXT_METRICS m{};
    layout->GetMetrics(&m);
    return m.height + 2 * kTextPad;
}

} // namespace mp
