#pragma once
// 繪製測試共用：畫在記憶體裡的 WIC render target（SSH、CI 沒有桌面，建不出視窗 target），畫完讀回像素。
#include <catch2/catch_test_macros.hpp>

#include <windows.h>
#include <wincodec.h>

#include <cstdlib>
#include <memory>
#include <vector>

#include "render/Renderer.h"

namespace mp::test {

// 圖上 (x, y) 的顏色。Image 是 premultiplied，不透明的像素兩種表示法一樣。
inline Color pixel(const Image& im, int x, int y) {
    const uint8_t* p = im.px(x, y);
    return {p[2], p[1], p[0], p[3]};
}
inline void setPixel(Image& im, int x, int y, Color c) {
    uint8_t* p = im.px(x, y);
    p[0] = c.b, p[1] = c.g, p[2] = c.r, p[3] = c.a;
}
// 每個色版差距都在 tol 以內。
inline bool isNear(Color a, Color b, int tol = 24) {
    return std::abs(a.r - b.r) <= tol && std::abs(a.g - b.g) <= tol && std::abs(a.b - b.b) <= tol && std::abs(a.a - b.a) <= tol;
}
// area（夾在圖內）裡接近 c 的像素數。
inline int countNear(const Image& im, Color c, Rc area, int tol = 24) {
    int n = 0;
    for (int y = std::max(0, static_cast<int>(area.top)); y < std::min(im.height, static_cast<int>(area.bottom)); ++y)
        for (int x = std::max(0, static_cast<int>(area.left)); x < std::min(im.width, static_cast<int>(area.right)); ++x)
            if (isNear(pixel(im, x, y), c, tol)) ++n;
    return n;
}
// 反鋸齒的字沒有幾個純色像素：算「比較接近字的顏色」的像素。
constexpr int kInkTol = 110;

inline std::shared_ptr<Image> whiteImage(int w, int h) {
    auto im = std::make_shared<Image>(w, h);
    for (auto& b : im->pixels) b = 255;
    return im;
}
// 不透明的漸層：紅 = x、綠 = y（寬高不超過 256）。相鄰區域的平均顏色都不同。
inline std::shared_ptr<Image> gradientImage(int w, int h) {
    auto im = std::make_shared<Image>(w, h);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) setPixel(*im, x, y, {static_cast<uint8_t>(x), static_cast<uint8_t>(y), 128, 255});
    return im;
}

struct RenderFixture {
    bool com = false;
    ComPtr<IWICImagingFactory> wic;
    Renderer r;

    RenderFixture() {
        com = SUCCEEDED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED));
        REQUIRE(r.init());
        REQUIRE(SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic))));
    }
    ~RenderFixture() {
        r.discardTarget();
        wic.Reset();
        if (com) CoUninitialize();
    }

    // 存檔用的壓平。失敗就讓測試停在這裡。
    Image flat(const ImagePtr& base, std::vector<Annotation> items) {
        DocState st;
        st.image = base;
        st.items = std::move(items);
        Image out;
        REQUIRE(flatten(r.device(), st, out));
        return out;
    }

    float textHeight(const Annotation& a) { return mp::textHeight(r.device(), *a.as<TextShape>()); }
    float measureUiText(const std::wstring& text, float size, bool bold = false, float maxWidth = 1e6f, float* height = nullptr) {
        return r.measureUiText(text, size, bold, maxWidth, height);
    }

    // 在一張和 base 一樣大的記憶體 target 上畫一幀，回傳畫完的像素。draw 收到 render target。
    template <class F>
    Image frameOn(const ImagePtr& base, F draw) {
        ComPtr<IWICBitmap> surface;
        REQUIRE(SUCCEEDED(wic->CreateBitmap(base->width, base->height, GUID_WICPixelFormat32bppPBGRA, WICBitmapCacheOnLoad, &surface)));
        ComPtr<ID2D1RenderTarget> rt = r.device().createWicTarget(surface.Get());
        REQUIRE(rt != nullptr);
        r.setTarget(rt);
        rt->BeginDraw();
        rt->Clear(D2D1::ColorF(0, 0, 0, 0));
        draw(rt.Get());
        REQUIRE(SUCCEEDED(rt->EndDraw()));
        r.endFrame();
        r.discardTarget();
        rt.Reset();

        Image out(base->width, base->height);
        WICRect full{0, 0, base->width, base->height};
        ComPtr<IWICBitmapLock> lock;
        REQUIRE(SUCCEEDED(surface->Lock(&full, WICBitmapLockRead, &lock)));
        UINT stride = 0, size = 0;
        BYTE* data = nullptr;
        REQUIRE(SUCCEEDED(lock->GetStride(&stride)));
        REQUIRE(SUCCEEDED(lock->GetDataPointer(&size, &data)));
        for (int y = 0; y < out.height; ++y) memcpy(out.row(y), data + static_cast<size_t>(y) * stride, out.stride());
        return out;
    }

    // 和畫面上一樣：先畫底圖，再畫標註（skipId 的不畫）。
    Image drawItems(const ImagePtr& base, const std::vector<Annotation>& items, uint32_t skipId) {
        return frameOn(base, [&](ID2D1RenderTarget* rt) {
            PaintCtx ctx = r.paintCtx(base, r.imageBitmap(base));
            drawImage(ctx);
            drawAnnotations(ctx, items, skipId);
        });
    }

    // 底圖上畫一段 UI 文字。
    Image drawUi(const ImagePtr& base, const std::wstring& text, Rc box, float size, Color color, DWRITE_TEXT_ALIGNMENT align,
                 bool wrap = false) {
        return frameOn(base, [&](ID2D1RenderTarget* rt) {
            drawImage(r.paintCtx(base, r.imageBitmap(base)));
            r.drawUiText(text, toD2D(box), size, toD2D(color), align, false, DWRITE_PARAGRAPH_ALIGNMENT_CENTER, true, wrap);
        });
    }
};

} // namespace mp::test
