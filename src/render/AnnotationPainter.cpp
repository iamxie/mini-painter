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

// 每個畫圖函式回 HRESULT：S_OK 是畫好了（或本來就沒東西可畫），失敗是要的資源（幾何、畫刷、文字排版、
// 馬賽克小圖）建不出來，那個物件沒畫完整。存檔的壓平靠這個知道不能把少了標註的圖當成存好了。

// 建標註用的矩形 / 橢圓幾何。create 收到 ID2D1Factory&，回 Create…Geometry 的 HRESULT。
template <class Create>
HRESULT createGeometry(RenderDevice& dev, Create create) {
    if (dev.testFaults.geometry) return E_OUTOFMEMORY;
    if (!dev.d2d()) return E_FAIL;
    return create(*dev.d2d());
}

// ---- 描邊與填色 ----

HRESULT strokeGeometry(Surface& s, ID2D1Geometry* geo, const StrokeStyle& style) {
    if (style.stroke.none()) return S_OK;
    ID2D1RenderTarget* rt = s.rt();
    RenderDevice& dev = s.device();
    const float w = style.width;
    const D2D1_COLOR_F c = toD2D(style.stroke);
    HRESULT hr = S_OK;
    switch (style.line) {
    case LineStyle::Solid:
        if (auto* b = s.brush(c, &hr)) rt->DrawGeometry(geo, b, w, dev.roundStyle());
        break;
    case LineStyle::Dashed:
        if (auto* b = s.brush(c, &hr)) rt->DrawGeometry(geo, b, w, dev.dashStyle());
        break;
    case LineStyle::Fuzzy:
        for (const FuzzyLayer& layer : kFuzzyLayers) {
            D2D1_COLOR_F faint = c;
            faint.a = c.a * layer.alpha;
            auto* b = s.brush(faint, &hr);
            if (!b) return hr;
            rt->DrawGeometry(geo, b, w * layer.widthScale, dev.roundStyle());
        }
        break;
    }
    return hr;
}

HRESULT fillAndStroke(Surface& s, ID2D1Geometry* geo, const StrokeStyle& style) {
    if (!style.fill.none()) {
        HRESULT hr = S_OK;
        auto* b = s.brush(style.fill, &hr);
        if (!b) return hr;
        s.rt()->FillGeometry(geo, b);
    }
    return strokeGeometry(s, geo, style);
}

// pts 是空的回 nullptr，error 填 S_OK（沒東西可畫，不是失敗）。
ComPtr<ID2D1PathGeometry> polygonGeometry(RenderDevice& dev, const std::vector<Pt>& pts, bool closed, HRESULT* error) {
    *error = S_OK;
    if (pts.empty()) return nullptr;
    return dev.pathGeometry([&](ID2D1GeometrySink& sink) {
        sink.BeginFigure(toD2D(pts[0]), closed ? D2D1_FIGURE_BEGIN_FILLED : D2D1_FIGURE_BEGIN_HOLLOW);
        for (size_t i = 1; i < pts.size(); ++i) sink.AddLine(toD2D(pts[i]));
        sink.EndFigure(closed ? D2D1_FIGURE_END_CLOSED : D2D1_FIGURE_END_OPEN);
    }, error);
}

// 建好的幾何拿去填色與描邊；幾何建不出來就回它的原因，沒東西可畫（nullptr 但 S_OK）回 S_OK。
HRESULT fillAndStroke(Surface& s, const ComPtr<ID2D1PathGeometry>& geo, HRESULT built, const StrokeStyle& style) {
    if (FAILED(built)) return built;
    return geo ? fillAndStroke(s, geo.Get(), style) : S_OK;
}

// ---- 文字排版 ----

// 建不出來回 nullptr；error 不是 nullptr 就填原因（成功填 S_OK）。
ComPtr<IDWriteTextLayout> textLayout(RenderDevice& dev, const TextShape& t, float width, HRESULT* error = nullptr) {
    HRESULT hr = dev.testFaults.text ? E_OUTOFMEMORY : dev.dwrite() ? S_OK : E_FAIL;
    ComPtr<IDWriteTextFormat> fmt;
    if (SUCCEEDED(hr))
        hr = dev.dwrite()->CreateTextFormat(t.style.font.c_str(), nullptr,
                                            t.style.bold ? DWRITE_FONT_WEIGHT_BOLD : DWRITE_FONT_WEIGHT_NORMAL,
                                            t.style.italic ? DWRITE_FONT_STYLE_ITALIC : DWRITE_FONT_STYLE_NORMAL,
                                            DWRITE_FONT_STRETCH_NORMAL, std::max(1.0f, t.style.size), kTextLocale, &fmt);
    // 空字串也排一個空白：方塊至少有一行高。
    const std::wstring& text = t.value.empty() ? std::wstring(L" ") : t.value;
    ComPtr<IDWriteTextLayout> layout;
    if (SUCCEEDED(hr))
        hr = dev.dwrite()->CreateTextLayout(text.c_str(), static_cast<UINT32>(text.size()), fmt.Get(), std::max(1.0f, width), 1e6f,
                                            &layout);
    if (error) *error = hr;
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

HRESULT draw(const PaintCtx& ctx, uint32_t, const StrokeStyle& style, const FreehandShape& f) {
    if (f.points.empty() || style.stroke.none()) return S_OK;
    Surface& s = *ctx.surface;
    ID2D1RenderTarget* rt = s.rt();
    // 在原始座標畫，用 transform 做非等比縮放，線寬跟著變。
    D2D1_MATRIX_3X2_F old;
    rt->GetTransform(&old);
    float sx = f.rect.width() / f.origW, sy = f.rect.height() / f.origH;
    D2D1_MATRIX_3X2_F local = D2D1::Matrix3x2F::Scale(sx, sy) * D2D1::Matrix3x2F::Translation(f.rect.left, f.rect.top);
    rt->SetTransform(local * old);
    HRESULT hr = S_OK;
    if (f.points.size() == 1) {
        float r = style.width * 0.5f;
        if (auto* b = s.brush(style.stroke, &hr)) rt->FillEllipse(D2D1::Ellipse(toD2D(f.points[0]), r, r), b);
    } else {
        auto geo = polygonGeometry(s.device(), f.points, false, &hr);
        if (geo) hr = strokeGeometry(s, geo.Get(), style);
    }
    rt->SetTransform(old);
    return hr;
}

HRESULT draw(const PaintCtx& ctx, uint32_t, const StrokeStyle& style, const LineShape& l) {
    Surface& s = *ctx.surface;
    ID2D1RenderTarget* rt = s.rt();
    RenderDevice& dev = s.device();
    const Pt c = lineControlPoint(l);
    const bool straight = dist(l.mid, lerp(l.p0, l.p1, 0.5f)) < 0.01f;
    HRESULT hr = S_OK;
    auto geo = dev.pathGeometry([&](ID2D1GeometrySink& sink) {
        sink.BeginFigure(toD2D(l.p0), D2D1_FIGURE_BEGIN_HOLLOW);
        if (straight) sink.AddLine(toD2D(l.p1));
        else sink.AddQuadraticBezier(D2D1::QuadraticBezierSegment(toD2D(c), toD2D(l.p1)));
        sink.EndFigure(D2D1_FIGURE_END_OPEN);
    }, &hr);
    if (!geo) return hr;
    if (FAILED(hr = strokeGeometry(s, geo.Get(), style))) return hr;
    if (style.stroke.none()) return S_OK;
    auto drawHead = [&](Pt tip, Pt dir) -> HRESULT {
        HRESULT headHr = S_OK;
        auto tri = polygonGeometry(dev, arrowHead(tip, dir, style.width), true, &headHr);
        if (!tri) return headHr;
        ID2D1SolidColorBrush* b = s.brush(style.stroke, &headHr);
        if (!b) return headHr;
        rt->FillGeometry(tri.Get(), b);
        // 描一圈圓角的邊，箭頭的三個角才不會太尖。
        rt->DrawGeometry(tri.Get(), b, std::max(1.0f, style.width * 0.5f), dev.roundStyle());
        return S_OK;
    };
    if (style.arrows == ArrowMode::End || style.arrows == ArrowMode::Both)
        if (FAILED(hr = drawHead(l.p1, straight ? l.p1 - l.p0 : l.p1 - c))) return hr;
    if (style.arrows == ArrowMode::Start || style.arrows == ArrowMode::Both)
        if (FAILED(hr = drawHead(l.p0, straight ? l.p0 - l.p1 : l.p0 - c))) return hr;
    return S_OK;
}

HRESULT draw(const PaintCtx& ctx, uint32_t, const StrokeStyle& style, const RectShape& r) {
    ComPtr<ID2D1RectangleGeometry> geo;
    HRESULT hr = createGeometry(ctx.surface->device(), [&](ID2D1Factory& f) { return f.CreateRectangleGeometry(toD2D(r.rect), &geo); });
    return FAILED(hr) ? hr : fillAndStroke(*ctx.surface, geo.Get(), style);
}

HRESULT draw(const PaintCtx& ctx, uint32_t, const StrokeStyle& style, const RoundRectShape& r) {
    float radius = std::min(r.rect.width(), r.rect.height()) * kRoundRectRadiusRatio;
    ComPtr<ID2D1RoundedRectangleGeometry> geo;
    HRESULT hr = createGeometry(ctx.surface->device(), [&](ID2D1Factory& f) {
        return f.CreateRoundedRectangleGeometry(D2D1::RoundedRect(toD2D(r.rect), radius, radius), &geo);
    });
    return FAILED(hr) ? hr : fillAndStroke(*ctx.surface, geo.Get(), style);
}

HRESULT draw(const PaintCtx& ctx, uint32_t, const StrokeStyle& style, const EllipseShape& e) {
    ComPtr<ID2D1EllipseGeometry> geo;
    HRESULT hr = createGeometry(ctx.surface->device(), [&](ID2D1Factory& f) {
        return f.CreateEllipseGeometry(D2D1::Ellipse(toD2D(e.rect.center()), e.rect.width() / 2, e.rect.height() / 2), &geo);
    });
    return FAILED(hr) ? hr : fillAndStroke(*ctx.surface, geo.Get(), style);
}

HRESULT draw(const PaintCtx& ctx, uint32_t, const StrokeStyle& style, const BubbleShape& b) {
    Pt ra, rb, tip;
    bubbleTail(b, ra, rb, tip);
    // 從尾巴根部的一點沿橢圓繞大弧到另一點，再連到尖端。
    HRESULT hr = S_OK;
    auto geo = ctx.surface->device().pathGeometry([&](ID2D1GeometrySink& sink) {
        sink.BeginFigure(toD2D(ra), D2D1_FIGURE_BEGIN_FILLED);
        sink.AddArc(D2D1::ArcSegment(toD2D(rb), D2D1::SizeF(b.rect.width() / 2, b.rect.height() / 2), 0.0f,
                                     D2D1_SWEEP_DIRECTION_CLOCKWISE, D2D1_ARC_SIZE_LARGE));
        sink.AddLine(toD2D(tip));
        sink.EndFigure(D2D1_FIGURE_END_CLOSED);
    }, &hr);
    return fillAndStroke(*ctx.surface, geo, hr, style);
}

HRESULT draw(const PaintCtx& ctx, uint32_t, const StrokeStyle& style, const StarShape& star) {
    HRESULT hr = S_OK;
    auto geo = polygonGeometry(ctx.surface->device(), starPoints(star), true, &hr);
    return fillAndStroke(*ctx.surface, geo, hr, style);
}

HRESULT draw(const PaintCtx& ctx, uint32_t, const StrokeStyle& style, const PolygonShape& poly) {
    HRESULT hr = S_OK;
    auto geo = polygonGeometry(ctx.surface->device(), starPoints(poly), true, &hr);
    return fillAndStroke(*ctx.surface, geo, hr, style);
}

HRESULT draw(const PaintCtx& ctx, uint32_t, const StrokeStyle& style, const MagnifierShape& m) {
    Surface& s = *ctx.surface;
    ID2D1RenderTarget* rt = s.rt();
    ComPtr<ID2D1EllipseGeometry> circle;
    HRESULT hr = createGeometry(s.device(), [&](ID2D1Factory& f) {
        return f.CreateEllipseGeometry(D2D1::Ellipse(toD2D(m.center), m.radius, m.radius), &circle);
    });
    if (FAILED(hr)) return hr;
    // 沒有底圖的點陣圖就不畫圈裡的內容（PaintCtx 的約定）；存檔的壓平一定會給，給不出來它自己會失敗。
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
    return strokeGeometry(s, circle.Get(), ring);
}

HRESULT draw(const PaintCtx& ctx, uint32_t id, const StrokeStyle&, const MosaicShape& m) {
    if (!ctx.image || !ctx.mosaic) return S_OK; // 沒給快取就不畫（PaintCtx 的約定）
    ID2D1RenderTarget* rt = ctx.surface->rt();
    HRESULT hr = S_OK;
    const MosaicCache::Tile* tile = ctx.mosaic->get(rt, ctx.image, id, m, &hr, ctx.surface->device().testFaults.bitmap);
    if (!tile) return hr; // 整個在圖外是 S_OK
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
    return S_OK;
}

HRESULT draw(const PaintCtx& ctx, uint32_t, const StrokeStyle& style, const TextShape& t) {
    Surface& s = *ctx.surface;
    ID2D1RenderTarget* rt = s.rt();
    D2D1_RECT_F box = toD2D(t.rect);
    HRESULT hr = S_OK;
    if (!style.fill.none()) {
        auto* b = s.brush(style.fill, &hr);
        if (!b) return hr;
        rt->FillRectangle(box, b);
    }
    if (!style.stroke.none()) {
        ComPtr<ID2D1RectangleGeometry> geo;
        hr = createGeometry(s.device(), [&](ID2D1Factory& f) { return f.CreateRectangleGeometry(box, &geo); });
        if (FAILED(hr) || FAILED(hr = strokeGeometry(s, geo.Get(), style))) return hr;
    }
    if (t.value.empty()) return S_OK;
    auto layout = textLayout(s.device(), t, t.rect.width() - 2 * kTextPad, &hr);
    if (!layout) return hr;
    auto* b = s.brush(t.style.color, &hr);
    if (!b) return hr;
    rt->DrawTextLayout(D2D1::Point2F(t.rect.left + kTextPad, t.rect.top + kTextPad), layout.Get(), b, D2D1_DRAW_TEXT_OPTIONS_CLIP);
    return S_OK;
}

} // namespace

void drawImage(const PaintCtx& ctx) {
    if (!ctx.surface || !ctx.surface->rt() || !ctx.imageBmp || !ctx.imageBmp->bitmap || !ctx.image) return;
    D2D1_RECT_F dest = D2D1::RectF(0, 0, static_cast<float>(ctx.image->width), static_cast<float>(ctx.image->height));
    ctx.surface->rt()->DrawBitmap(ctx.imageBmp->bitmap.Get(), dest, 1.0f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
}

HRESULT drawAnnotations(const PaintCtx& ctx, const std::vector<Annotation>& items, uint32_t skipId, uint32_t* failedId) {
    HRESULT first = S_OK;
    for (const auto& a : items) {
        if (a.id == skipId) continue;
        HRESULT hr = drawAnnotation(ctx, a);
        if (FAILED(hr) && SUCCEEDED(first)) {
            first = hr;
            if (failedId) *failedId = a.id;
        }
    }
    return first;
}

HRESULT drawAnnotation(const PaintCtx& ctx, const Annotation& a) {
    if (!ctx.surface || !ctx.surface->rt()) return E_FAIL;
    return std::visit([&](const auto& shape) { return draw(ctx, a.id, a.stroke, shape); }, a.shape);
}

float textHeight(RenderDevice& device, const TextShape& t) {
    auto layout = textLayout(device, t, t.rect.width() - 2 * kTextPad);
    if (!layout) return t.style.size * 1.5f + 2 * kTextPad;
    DWRITE_TEXT_METRICS m{};
    layout->GetMetrics(&m);
    return m.height + 2 * kTextPad;
}

} // namespace mp
