#include "render/Renderer.h"

#include <wincodec.h>

#include <algorithm>
#include <cmath>

#include "core/ImageOps.h"

#pragma comment(lib, "d2d1")
#pragma comment(lib, "dwrite")

namespace mp {

bool Renderer::init() {
    HRESULT hr = D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory), nullptr,
                                   reinterpret_cast<void**>(d2d_.GetAddressOf()));
    if (FAILED(hr)) return false;
    hr = DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                             reinterpret_cast<IUnknown**>(dw_.GetAddressOf()));
    if (FAILED(hr)) return false;
    d2d_->CreateStrokeStyle(D2D1::StrokeStyleProperties(D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND,
                                                        D2D1_CAP_STYLE_ROUND, D2D1_LINE_JOIN_ROUND, 10.0f,
                                                        D2D1_DASH_STYLE_DASH, 0.0f),
                            nullptr, 0, &dash_);
    d2d_->CreateStrokeStyle(D2D1::StrokeStyleProperties(D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND,
                                                        D2D1_CAP_STYLE_ROUND, D2D1_LINE_JOIN_ROUND, 10.0f,
                                                        D2D1_DASH_STYLE_SOLID, 0.0f),
                            nullptr, 0, &round_);
    return true;
}

bool Renderer::ensureHwndTarget(HWND hwnd) {
    if (rt_) return true;
    RECT rc;
    GetClientRect(hwnd, &rc);
    D2D1_SIZE_U size = D2D1::SizeU(std::max(1l, rc.right - rc.left), std::max(1l, rc.bottom - rc.top));
    auto props = D2D1::RenderTargetProperties(
        D2D1_RENDER_TARGET_TYPE_DEFAULT, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
        96.0f, 96.0f);
    HRESULT hr = d2d_->CreateHwndRenderTarget(props, D2D1::HwndRenderTargetProperties(hwnd, size), &rt_);
    if (FAILED(hr)) return false;
    return true;
}

void Renderer::resize(UINT w, UINT h) {
    if (rt_) rt_->Resize(D2D1::SizeU(std::max(1u, w), std::max(1u, h)));
}

void Renderer::discardTarget() {
    forgetTarget(rt_.Get());
    rt_.Reset();
    bitmaps_.clear();
    mosaic_.clear();
}

ID2D1SolidColorBrush* Renderer::brushFor(ID2D1RenderTarget* rt, D2D1_COLOR_F color) {
    auto& b = brushes_[rt];
    if (!b) rt->CreateSolidColorBrush(color, &b);
    else b->SetColor(color);
    return b.Get();
}

void Renderer::forgetTarget(ID2D1RenderTarget* rt) { brushes_.erase(rt); }

ComPtr<ID2D1Bitmap> Renderer::makeBitmap(ID2D1RenderTarget* rt, const Image& img, float* scale) {
    *scale = 1.0f;
    UINT maxSize = rt->GetMaximumBitmapSize();
    const Image* src = &img;
    Image shrunk;
    if (static_cast<UINT>(img.width) > maxSize || static_cast<UINT>(img.height) > maxSize) {
        float s = static_cast<float>(maxSize) / std::max(img.width, img.height);
        shrunk = resampleFast(img, std::max(1, static_cast<int>(img.width * s)), std::max(1, static_cast<int>(img.height * s)));
        *scale = static_cast<float>(shrunk.width) / img.width;
        src = &shrunk;
    }
    ComPtr<ID2D1Bitmap> bmp;
    rt->CreateBitmap(D2D1::SizeU(src->width, src->height), src->pixels.data(), static_cast<UINT32>(src->stride()),
                     D2D1::BitmapProperties(D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED)),
                     &bmp);
    return bmp;
}

const ImageBitmap* Renderer::imageBitmap(const ImagePtr& img) {
    if (!rt_ || !img) return nullptr;
    // 馬賽克小圖綁主底圖，主底圖換了就清。
    if (mosaicSource_ != img.get()) { mosaic_.clear(); mosaicSource_ = img.get(); }
    for (auto it = bitmaps_.begin(); it != bitmaps_.end(); ++it) {
        if (it->src == img && it->tw == 0) {
            bitmaps_.splice(bitmaps_.end(), bitmaps_, it); // 搬到最後，指標不變
            return &bitmaps_.back().bmp;
        }
    }
    BitmapEntry e;
    e.src = img;
    e.bmp.bitmap = makeBitmap(rt_.Get(), *img, &e.bmp.scale);
    e.bmp.source = img.get();
    if (!e.bmp.bitmap) return nullptr;
    if (bitmaps_.size() >= kBitmapCache) bitmaps_.pop_front();
    bitmaps_.push_back(std::move(e));
    return &bitmaps_.back().bmp;
}

const ImageBitmap* Renderer::scaledBitmap(const ImagePtr& img, int tw, int th) {
    if (!rt_ || !img || tw <= 0 || th <= 0) return nullptr;
    for (auto it = bitmaps_.begin(); it != bitmaps_.end(); ++it) {
        if (it->src == img && it->tw == tw && it->th == th) {
            bitmaps_.splice(bitmaps_.end(), bitmaps_, it);
            return &bitmaps_.back().bmp;
        }
    }
    UINT maxSize = rt_->GetMaximumBitmapSize();
    // 放大到超過 GPU 上限或 4000 萬像素就不做，記憶體和時間都不值得。
    if (static_cast<UINT>(tw) > maxSize || static_cast<UINT>(th) > maxSize || static_cast<long long>(tw) * th > 40'000'000LL) return nullptr;
    Image scaled = resample(*img, tw, th);
    ComPtr<ID2D1Bitmap> bmp;
    rt_->CreateBitmap(D2D1::SizeU(scaled.width, scaled.height), scaled.pixels.data(), static_cast<UINT32>(scaled.stride()),
                      D2D1::BitmapProperties(D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED)),
                      &bmp);
    if (!bmp) return nullptr;
    BitmapEntry e;
    e.src = img;
    e.tw = tw;
    e.th = th;
    e.bmp.bitmap = bmp;
    e.bmp.source = img.get();
    e.bmp.scale = static_cast<float>(tw) / img->width;
    if (bitmaps_.size() >= kBitmapCache) bitmaps_.pop_front();
    bitmaps_.push_back(std::move(e));
    return &bitmaps_.back().bmp;
}

void Renderer::drawImage(DrawCtx& ctx) {
    if (!ctx.imageBmp || !ctx.imageBmp->bitmap || !ctx.image) return;
    D2D1_RECT_F dest = D2D1::RectF(0, 0, static_cast<float>(ctx.image->width), static_cast<float>(ctx.image->height));
    ctx.rt->DrawBitmap(ctx.imageBmp->bitmap.Get(), dest, 1.0f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
}

// ---- 描邊 ----

void Renderer::strokeGeometry(ID2D1RenderTarget* rt, ID2D1Geometry* geo, const StrokeStyle& s, float widthScale) {
    if (s.stroke.none()) return;
    float w = s.width() * widthScale;
    D2D1_COLOR_F c = toD2D(s.stroke);
    switch (s.line) {
    case LineStyle::Solid:
        rt->DrawGeometry(geo, brushFor(rt, c), w, round_.Get());
        break;
    case LineStyle::Dashed:
        rt->DrawGeometry(geo, brushFor(rt, c), w, dash_.Get());
        break;
    case LineStyle::Fuzzy: {
        // 沒有真正的模糊，用三層漸淡的描邊模擬。
        D2D1_COLOR_F faint = c;
        faint.a = c.a * 0.12f;
        rt->DrawGeometry(geo, brushFor(rt, faint), w * 3.0f, round_.Get());
        faint.a = c.a * 0.25f;
        rt->DrawGeometry(geo, brushFor(rt, faint), w * 2.0f, round_.Get());
        faint.a = c.a * 0.8f;
        rt->DrawGeometry(geo, brushFor(rt, faint), w, round_.Get());
        break;
    }
    }
}

void Renderer::drawShapeGeometry(DrawCtx& ctx, ID2D1Geometry* geo, const StrokeStyle& s) {
    if (!s.fill.none()) ctx.rt->FillGeometry(geo, brushFor(ctx.rt, s.fill));
    strokeGeometry(ctx.rt, geo, s);
}

static ComPtr<ID2D1PathGeometry> polygonGeometry(ID2D1Factory* f, const std::vector<Pt>& pts, bool closed) {
    ComPtr<ID2D1PathGeometry> geo;
    if (pts.empty() || FAILED(f->CreatePathGeometry(&geo))) return nullptr;
    ComPtr<ID2D1GeometrySink> sink;
    geo->Open(&sink);
    sink->BeginFigure(toD2D(pts[0]), closed ? D2D1_FIGURE_BEGIN_FILLED : D2D1_FIGURE_BEGIN_HOLLOW);
    for (size_t i = 1; i < pts.size(); ++i) sink->AddLine(toD2D(pts[i]));
    sink->EndFigure(closed ? D2D1_FIGURE_END_CLOSED : D2D1_FIGURE_END_OPEN);
    sink->Close();
    return geo;
}

ComPtr<IDWriteTextLayout> Renderer::textLayout(const Annotation& a, float width) {
    ComPtr<IDWriteTextFormat> fmt;
    HRESULT hr = dw_->CreateTextFormat(a.text.font.c_str(), nullptr,
                                       a.text.bold ? DWRITE_FONT_WEIGHT_BOLD : DWRITE_FONT_WEIGHT_NORMAL,
                                       a.text.italic ? DWRITE_FONT_STYLE_ITALIC : DWRITE_FONT_STYLE_NORMAL,
                                       DWRITE_FONT_STRETCH_NORMAL, std::max(1.0f, a.text.size), L"zh-TW", &fmt);
    if (FAILED(hr)) return nullptr;
    const std::wstring& text = a.textValue.empty() ? std::wstring(L" ") : a.textValue;
    ComPtr<IDWriteTextLayout> layout;
    hr = dw_->CreateTextLayout(text.c_str(), static_cast<UINT32>(text.size()), fmt.Get(), std::max(1.0f, width),
                               1e6f, &layout);
    if (FAILED(hr)) return nullptr;
    DWRITE_TEXT_ALIGNMENT al = DWRITE_TEXT_ALIGNMENT_LEADING;
    switch (a.text.align) {
    case TextAlign::Center: al = DWRITE_TEXT_ALIGNMENT_CENTER; break;
    case TextAlign::Right: al = DWRITE_TEXT_ALIGNMENT_TRAILING; break;
    case TextAlign::Justify: al = DWRITE_TEXT_ALIGNMENT_JUSTIFIED; break;
    default: break;
    }
    layout->SetTextAlignment(al);
    layout->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
    if (a.text.underline) {
        DWRITE_TEXT_RANGE range{0, static_cast<UINT32>(text.size())};
        layout->SetUnderline(TRUE, range);
    }
    return layout;
}

static constexpr float kTextPad = 4.0f;

float Renderer::textHeight(const Annotation& a) {
    auto layout = textLayout(a, a.rect.width() - 2 * kTextPad);
    if (!layout) return a.text.size * 1.5f + 2 * kTextPad;
    DWRITE_TEXT_METRICS m{};
    layout->GetMetrics(&m);
    return m.height + 2 * kTextPad;
}

void Renderer::drawAnnotations(DrawCtx& ctx, const std::vector<Annotation>& items, uint32_t skipId) {
    for (const auto& a : items)
        if (a.id != skipId) drawAnnotation(ctx, a);
}

void Renderer::drawAnnotation(DrawCtx& ctx, const Annotation& a) {
    ID2D1RenderTarget* rt = ctx.rt;
    switch (a.kind) {
    case Kind::Freehand: {
        if (a.strokePoints.empty() || a.stroke.stroke.none()) return;
        // 在原始座標畫，用 transform 做非等比縮放，線寬跟著變。
        auto geo = polygonGeometry(d2d_.Get(), a.strokePoints, false);
        if (!geo) return;
        D2D1_MATRIX_3X2_F old;
        rt->GetTransform(&old);
        float sx = a.rect.width() / a.origW, sy = a.rect.height() / a.origH;
        D2D1_MATRIX_3X2_F local = D2D1::Matrix3x2F::Scale(sx, sy) * D2D1::Matrix3x2F::Translation(a.rect.left, a.rect.top);
        rt->SetTransform(local * old);
        if (a.strokePoints.size() == 1) {
            float r = a.stroke.width() * 0.5f;
            rt->FillEllipse(D2D1::Ellipse(toD2D(a.strokePoints[0]), r, r), brushFor(rt, a.stroke.stroke));
        } else {
            strokeGeometry(rt, geo.Get(), a.stroke);
        }
        rt->SetTransform(old);
        return;
    }
    case Kind::Line: {
        ComPtr<ID2D1PathGeometry> geo;
        d2d_->CreatePathGeometry(&geo);
        ComPtr<ID2D1GeometrySink> sink;
        geo->Open(&sink);
        sink->BeginFigure(toD2D(a.p0), D2D1_FIGURE_BEGIN_HOLLOW);
        Pt c = lineControlPoint(a);
        bool straight = dist(a.mid, lerp(a.p0, a.p1, 0.5f)) < 0.01f;
        if (straight) sink->AddLine(toD2D(a.p1));
        else sink->AddQuadraticBezier(D2D1::QuadraticBezierSegment(toD2D(c), toD2D(a.p1)));
        sink->EndFigure(D2D1_FIGURE_END_OPEN);
        sink->Close();
        strokeGeometry(rt, geo.Get(), a.stroke);
        if (a.stroke.stroke.none()) return;
        auto drawHead = [&](Pt tip, Pt dir) {
            auto tri = polygonGeometry(d2d_.Get(), arrowHead(tip, dir, a.stroke.width()), true);
            if (tri) {
                rt->FillGeometry(tri.Get(), brushFor(rt, a.stroke.stroke));
                rt->DrawGeometry(tri.Get(), brushFor(rt, a.stroke.stroke), std::max(1.0f, a.stroke.width() * 0.5f), round_.Get());
            }
        };
        if (a.stroke.arrows == ArrowMode::End || a.stroke.arrows == ArrowMode::Both)
            drawHead(a.p1, straight ? a.p1 - a.p0 : a.p1 - c);
        if (a.stroke.arrows == ArrowMode::Start || a.stroke.arrows == ArrowMode::Both)
            drawHead(a.p0, straight ? a.p0 - a.p1 : a.p0 - c);
        return;
    }
    case Kind::Rect: {
        ComPtr<ID2D1RectangleGeometry> geo;
        d2d_->CreateRectangleGeometry(toD2D(a.rect), &geo);
        drawShapeGeometry(ctx, geo.Get(), a.stroke);
        return;
    }
    case Kind::RoundRect: {
        float r = std::min(a.rect.width(), a.rect.height()) * 0.2f;
        ComPtr<ID2D1RoundedRectangleGeometry> geo;
        d2d_->CreateRoundedRectangleGeometry(D2D1::RoundedRect(toD2D(a.rect), r, r), &geo);
        drawShapeGeometry(ctx, geo.Get(), a.stroke);
        return;
    }
    case Kind::Ellipse: {
        ComPtr<ID2D1EllipseGeometry> geo;
        d2d_->CreateEllipseGeometry(D2D1::Ellipse(toD2D(a.rect.center()), a.rect.width() / 2, a.rect.height() / 2), &geo);
        drawShapeGeometry(ctx, geo.Get(), a.stroke);
        return;
    }
    case Kind::Bubble: {
        Pt ra, rb, tip;
        bubbleTail(a, ra, rb, tip);
        ComPtr<ID2D1PathGeometry> geo;
        d2d_->CreatePathGeometry(&geo);
        ComPtr<ID2D1GeometrySink> sink;
        geo->Open(&sink);
        sink->BeginFigure(toD2D(ra), D2D1_FIGURE_BEGIN_FILLED);
        sink->AddArc(D2D1::ArcSegment(toD2D(rb), D2D1::SizeF(a.rect.width() / 2, a.rect.height() / 2), 0.0f,
                                      D2D1_SWEEP_DIRECTION_CLOCKWISE, D2D1_ARC_SIZE_LARGE));
        sink->AddLine(toD2D(tip));
        sink->EndFigure(D2D1_FIGURE_END_CLOSED);
        sink->Close();
        drawShapeGeometry(ctx, geo.Get(), a.stroke);
        return;
    }
    case Kind::Star:
    case Kind::Polygon: {
        auto geo = polygonGeometry(d2d_.Get(), starPoints(a), true);
        if (geo) drawShapeGeometry(ctx, geo.Get(), a.stroke);
        return;
    }
    case Kind::Magnifier: {
        ComPtr<ID2D1EllipseGeometry> circle;
        d2d_->CreateEllipseGeometry(D2D1::Ellipse(toD2D(a.center), a.radius, a.radius), &circle);
        if (ctx.imageBmp && ctx.imageBmp->bitmap) {
            rt->PushLayer(D2D1::LayerParameters(D2D1::InfiniteRect(), circle.Get()), nullptr);
            float s = ctx.imageBmp->scale;
            float srcR = a.radius / a.zoom;
            D2D1_RECT_F src = D2D1::RectF((a.center.x - srcR) * s, (a.center.y - srcR) * s, (a.center.x + srcR) * s,
                                          (a.center.y + srcR) * s);
            D2D1_RECT_F dst = D2D1::RectF(a.center.x - a.radius, a.center.y - a.radius, a.center.x + a.radius,
                                          a.center.y + a.radius);
            rt->DrawBitmap(ctx.imageBmp->bitmap.Get(), dst, 1.0f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR, src);
            rt->PopLayer();
        }
        StrokeStyle s = a.stroke;
        if (s.stroke.none()) s.stroke = Color::rgb(80, 80, 80);
        strokeGeometry(rt, circle.Get(), s);
        return;
    }
    case Kind::Mosaic: {
        if (!ctx.image || !ctx.mosaicCache) return;
        auto& entry = (*ctx.mosaicCache)[a.id];
        if (!entry.bitmap || !(entry.rect == a.rect) || entry.block != a.blockSize || entry.source != ctx.image) {
            int x0 = static_cast<int>(std::floor(a.rect.left)), y0 = static_cast<int>(std::floor(a.rect.top));
            int x1 = static_cast<int>(std::ceil(a.rect.right)), y1 = static_cast<int>(std::ceil(a.rect.bottom));
            Image blocks = mosaicBlocks(*ctx.image, x0, y0, x1, y1, a.blockSize);
            entry = {};
            entry.rect = a.rect;
            entry.block = a.blockSize;
            entry.source = ctx.image;
            entry.x0 = std::max(0, x0);
            entry.y0 = std::max(0, y0);
            if (!blocks.empty()) {
                rt->CreateBitmap(D2D1::SizeU(blocks.width, blocks.height), blocks.pixels.data(),
                                 static_cast<UINT32>(blocks.stride()),
                                 D2D1::BitmapProperties(D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED)),
                                 &entry.bitmap);
            }
        }
        if (entry.bitmap) {
            D2D1_SIZE_U bs = entry.bitmap->GetPixelSize();
            float bw = static_cast<float>(bs.width * a.blockSize), bh = static_cast<float>(bs.height * a.blockSize);
            D2D1_RECT_F dst = D2D1::RectF(static_cast<float>(entry.x0), static_cast<float>(entry.y0), entry.x0 + bw, entry.y0 + bh);
            // 只畫在圖片範圍內與物件矩形內。
            D2D1_RECT_F clip = D2D1::RectF(std::max(a.rect.left, 0.0f), std::max(a.rect.top, 0.0f),
                                           std::min(a.rect.right, static_cast<float>(ctx.image->width)),
                                           std::min(a.rect.bottom, static_cast<float>(ctx.image->height)));
            rt->PushAxisAlignedClip(clip, D2D1_ANTIALIAS_MODE_ALIASED);
            rt->DrawBitmap(entry.bitmap.Get(), dst, 1.0f, D2D1_BITMAP_INTERPOLATION_MODE_NEAREST_NEIGHBOR);
            rt->PopAxisAlignedClip();
        }
        return;
    }
    case Kind::Text: {
        D2D1_RECT_F box = toD2D(a.rect);
        if (!a.stroke.fill.none()) rt->FillRectangle(box, brushFor(rt, a.stroke.fill));
        if (!a.stroke.stroke.none()) {
            ComPtr<ID2D1RectangleGeometry> geo;
            d2d_->CreateRectangleGeometry(box, &geo);
            strokeGeometry(rt, geo.Get(), a.stroke);
        }
        if (a.textValue.empty()) return;
        auto layout = textLayout(a, a.rect.width() - 2 * kTextPad);
        if (layout) rt->DrawTextLayout(D2D1::Point2F(a.rect.left + kTextPad, a.rect.top + kTextPad), layout.Get(),
                                       brushFor(rt, a.text.color), D2D1_DRAW_TEXT_OPTIONS_CLIP);
        return;
    }
    }
}

Image Renderer::flatten(const DocState& state) {
    const Image& src = *state.image;
    Image out = src;
    if (state.items.empty()) return out;

    ComPtr<IWICImagingFactory> wic;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic)))) return out;
    ComPtr<IWICBitmap> wicBmp;
    if (FAILED(wic->CreateBitmap(src.width, src.height, GUID_WICPixelFormat32bppPBGRA, WICBitmapCacheOnDemand, &wicBmp)))
        return out;
    ComPtr<ID2D1RenderTarget> rt;
    auto props = D2D1::RenderTargetProperties(
        D2D1_RENDER_TARGET_TYPE_DEFAULT, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
        96.0f, 96.0f);
    if (FAILED(d2d_->CreateWicBitmapRenderTarget(wicBmp.Get(), props, &rt))) return out;

    ImageBitmap ib;
    ib.bitmap = makeBitmap(rt.Get(), src, &ib.scale);
    ib.source = &src;
    std::map<uint32_t, MosaicEntry> cache;
    DrawCtx ctx{rt.Get(), &src, &ib, &cache};

    rt->BeginDraw();
    rt->Clear(D2D1::ColorF(0, 0, 0, 0));
    drawImage(ctx);
    drawAnnotations(ctx, state.items);
    HRESULT hr = rt->EndDraw();
    forgetTarget(rt.Get());
    if (FAILED(hr)) return out;

    ComPtr<IWICBitmapLock> lock;
    WICRect full{0, 0, src.width, src.height};
    if (FAILED(wicBmp->Lock(&full, WICBitmapLockRead, &lock))) return out;
    UINT stride = 0, size = 0;
    BYTE* data = nullptr;
    lock->GetStride(&stride);
    lock->GetDataPointer(&size, &data);
    for (int y = 0; y < src.height; ++y) memcpy(out.row(y), data + static_cast<size_t>(y) * stride, out.stride());
    return out;
}

// ---- UI 文字 ----

IDWriteTextFormat* Renderer::uiFormat(float size, bool bold) {
    auto key = std::make_pair(static_cast<int>(size * 4), bold);
    auto it = uiFormats_.find(key);
    if (it != uiFormats_.end()) return it->second.Get();
    ComPtr<IDWriteTextFormat> fmt;
    dw_->CreateTextFormat(L"Microsoft JhengHei UI", nullptr, bold ? DWRITE_FONT_WEIGHT_SEMI_BOLD : DWRITE_FONT_WEIGHT_NORMAL,
                          DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, size, L"zh-TW", &fmt);
    uiFormats_[key] = fmt;
    return fmt.Get();
}

void Renderer::drawUiText(ID2D1RenderTarget* rt, const std::wstring& text, const D2D1_RECT_F& rect, float size,
                          D2D1_COLOR_F color, DWRITE_TEXT_ALIGNMENT align, bool bold, DWRITE_PARAGRAPH_ALIGNMENT para,
                          bool clip, bool wrap) {
    IDWriteTextFormat* fmt = uiFormat(size, bold);
    if (!fmt) return;
    fmt->SetTextAlignment(align);
    fmt->SetParagraphAlignment(para);
    fmt->SetWordWrapping(wrap ? DWRITE_WORD_WRAPPING_WRAP : DWRITE_WORD_WRAPPING_NO_WRAP);
    rt->DrawText(text.c_str(), static_cast<UINT32>(text.size()), fmt, rect, brushFor(rt, color),
                 clip ? D2D1_DRAW_TEXT_OPTIONS_CLIP : D2D1_DRAW_TEXT_OPTIONS_NONE);
}

float Renderer::measureUiText(const std::wstring& text, float size, bool bold, float maxWidth, float* height) {
    IDWriteTextFormat* fmt = uiFormat(size, bold);
    ComPtr<IDWriteTextLayout> layout;
    if (!fmt || FAILED(dw_->CreateTextLayout(text.c_str(), static_cast<UINT32>(text.size()), fmt, maxWidth, 1e6f, &layout)))
        return 0;
    layout->SetWordWrapping(maxWidth < 1e5f ? DWRITE_WORD_WRAPPING_WRAP : DWRITE_WORD_WRAPPING_NO_WRAP);
    DWRITE_TEXT_METRICS m{};
    layout->GetMetrics(&m);
    if (height) *height = m.height;
    return m.widthIncludingTrailingWhitespace;
}

} // namespace mp
