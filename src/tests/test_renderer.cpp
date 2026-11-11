#include <catch2/catch_test_macros.hpp>

#include <windows.h>
#include <wincodec.h>

#include "render/Renderer.h"

using namespace mp;

// Renderer 的點陣圖快取：只留上一幀畫過的。
// render target 用記憶體裡的 WIC 點陣圖：沒有桌面的 session（SSH、CI）建不出視窗 target。

namespace {

struct Fixture {
    bool com = false;
    ComPtr<IWICBitmap> surface;
    Renderer r;
    Fixture() {
        com = SUCCEEDED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED));
        REQUIRE(r.init());
        ComPtr<IWICImagingFactory> wic;
        REQUIRE(SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic))));
        REQUIRE(SUCCEEDED(wic->CreateBitmap(64, 64, GUID_WICPixelFormat32bppPBGRA, WICBitmapCacheOnLoad, &surface)));
        ComPtr<ID2D1RenderTarget> rt;
        auto props = D2D1::RenderTargetProperties(
            D2D1_RENDER_TARGET_TYPE_DEFAULT, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96.0f, 96.0f);
        REQUIRE(SUCCEEDED(r.d2d()->CreateWicBitmapRenderTarget(surface.Get(), props, &rt)));
        r.setTarget(rt);
    }
    ~Fixture() {
        r.discardTarget();
        surface.Reset();
        if (com) CoUninitialize();
    }
    // 一幀：和 MainWindow::paint 一樣，畫完呼叫 endFrame。
    template <class F>
    void frame(F draw) {
        r.rt()->BeginDraw();
        draw();
        r.rt()->EndDraw();
        r.endFrame();
    }
};

ImagePtr img(int w, int h) { return std::make_shared<Image>(w, h); }

// 畫完之後 render target 底下那張 WIC 點陣圖 (x, y) 的像素（BGRA）。
uint32_t pixelAt(Fixture& f, int x, int y) {
    WICRect rc{x, y, 1, 1};
    ComPtr<IWICBitmapLock> lock;
    REQUIRE(SUCCEEDED(f.surface->Lock(&rc, WICBitmapLockRead, &lock)));
    UINT size = 0;
    BYTE* p = nullptr;
    REQUIRE(SUCCEEDED(lock->GetDataPointer(&size, &p)));
    return *reinterpret_cast<const uint32_t*>(p);
}

} // namespace

TEST_CASE("Renderer：高品質縮放給了先縮好的圖（尺寸相同）就直接上傳；尺寸不對就自己縮") {
    Fixture f;
    auto a = img(64, 64); // 全 0（透明）
    auto pre = std::make_shared<Image>(16, 16);
    for (auto& b : pre->pixels) b = 255; // 白
    f.frame([&] {
        f.r.rt()->Clear(D2D1::ColorF(0, 0, 0, 0));
        const ImageBitmap* s = f.r.scaledBitmap(a, 16, 16, pre);
        REQUIRE(s != nullptr);
        f.r.rt()->DrawBitmap(s->bitmap.Get(), D2D1::RectF(0, 0, 16, 16));
    });
    CHECK(pixelAt(f, 8, 8) == 0xFFFFFFFFu);
    f.frame([&] {
        f.r.rt()->Clear(D2D1::ColorF(0, 0, 0, 0));
        const ImageBitmap* s = f.r.scaledBitmap(a, 20, 20, pre);
        REQUIRE(s != nullptr);
        CHECK(s->bitmap->GetPixelSize().width == 20);
        f.r.rt()->DrawBitmap(s->bitmap.Get(), D2D1::RectF(0, 0, 20, 20));
    });
    CHECK(pixelAt(f, 8, 8) == 0u);
}

TEST_CASE("Renderer：連續兩幀畫同一張圖，點陣圖沿用") {
    Fixture f;
    auto a = img(32, 32);
    ID2D1Bitmap* first = nullptr;
    f.frame([&] {
        const ImageBitmap* b = f.r.imageBitmap(a);
        REQUIRE(b != nullptr);
        first = b->bitmap.Get();
        CHECK(f.r.imageBitmap(a) == b); // 同一幀再取是同一筆
    });
    CHECK(f.r.cachedBitmapCount() == 1);
    f.frame([&] { CHECK(f.r.imageBitmap(a)->bitmap.Get() == first); });
    CHECK(f.r.cachedBitmapCount() == 1);
}

TEST_CASE("Renderer：翻頁後上一張圖不留在快取") {
    Fixture f;
    auto a = img(32, 32);
    std::weak_ptr<const Image> aWeak = a;
    f.frame([&] { f.r.imageBitmap(a); });
    a.reset(); // 文件換圖：只剩快取握著
    CHECK_FALSE(aWeak.expired());
    auto b = img(32, 32);
    f.frame([&] { f.r.imageBitmap(b); });
    CHECK(aWeak.expired());
    CHECK(f.r.cachedBitmapCount() == 1);
}

TEST_CASE("Renderer：連翻很多張，快取不累積") {
    Fixture f;
    std::vector<std::weak_ptr<const Image>> old;
    for (int i = 0; i < 20; ++i) {
        auto page = img(16, 16);
        old.push_back(page);
        f.frame([&] { f.r.imageBitmap(page); });
        CHECK(f.r.cachedBitmapCount() == 1);
    }
    for (size_t i = 0; i + 1 < old.size(); ++i) CHECK(old[i].expired());
}

TEST_CASE("Renderer：沒有圖可畫的一幀（開圖失敗）清空快取") {
    Fixture f;
    auto a = img(32, 32), b = img(32, 32);
    std::weak_ptr<const Image> aWeak = a, bWeak = b;
    f.frame([&] {
        f.r.imageBitmap(a);
        f.r.imageBitmap(b);
    });
    CHECK(f.r.cachedBitmapCount() == 2);
    a.reset();
    b.reset();
    f.frame([] {});
    CHECK(f.r.cachedBitmapCount() == 0);
    CHECK(aWeak.expired());
    CHECK(bWeak.expired());
}

TEST_CASE("Renderer：兩頁並排，同一幀兩張都在，第一張的指標不因第二張失效") {
    Fixture f;
    auto a = img(32, 32), b = img(16, 48);
    f.frame([&] {
        const ImageBitmap* pa = f.r.imageBitmap(a);
        const ImageBitmap* pb = f.r.imageBitmap(b);
        REQUIRE(pa != nullptr);
        REQUIRE(pb != nullptr);
        CHECK(pa->source == a.get());
        CHECK(pb->source == b.get());
        CHECK(pa->bitmap->GetPixelSize().width == 32);
    });
    CHECK(f.r.cachedBitmapCount() == 2);
}

TEST_CASE("Renderer：高品質縮放換倍率，舊倍率的點陣圖丟掉") {
    Fixture f;
    auto a = img(64, 64);
    f.frame([&] {
        const ImageBitmap* s = f.r.scaledBitmap(a, 16, 16);
        REQUIRE(s != nullptr);
        CHECK(s->bitmap->GetPixelSize().width == 16);
        CHECK(s->scale == 0.25f);
    });
    CHECK(f.r.cachedBitmapCount() == 1);
    for (int size : {20, 24, 32, 40, 48}) {
        f.frame([&] { REQUIRE(f.r.scaledBitmap(a, size, size) != nullptr); });
        CHECK(f.r.cachedBitmapCount() == 1); // 只留這一幀用的倍率
    }
    // 連續縮放中改畫原尺寸：縮好的那張也丟掉
    f.frame([&] { f.r.imageBitmap(a); });
    CHECK(f.r.cachedBitmapCount() == 1);
}

TEST_CASE("Renderer：馬賽克小圖只留這一幀畫過的") {
    Fixture f;
    auto a = img(64, 64);
    Annotation m = makeShape(Kind::Mosaic, {32, 32}, 32, StrokeStyle{});
    m.id = 7;
    m.blockSize = 8;
    f.frame([&] {
        DrawCtx ctx{f.r.rt(), a.get(), f.r.imageBitmap(a), &f.r.mosaicCache()};
        f.r.drawAnnotation(ctx, m);
    });
    CHECK(f.r.mosaicCache().size() == 1);
    f.frame([&] { // 物件還在：沿用
        DrawCtx ctx{f.r.rt(), a.get(), f.r.imageBitmap(a), &f.r.mosaicCache()};
        f.r.drawAnnotation(ctx, m);
    });
    CHECK(f.r.mosaicCache().size() == 1);
    f.frame([&] { f.r.imageBitmap(a); }); // 物件刪掉了
    CHECK(f.r.mosaicCache().empty());
}
