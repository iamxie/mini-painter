#include <catch2/catch_test_macros.hpp>

#include <windows.h>
#include <wincodec.h>

#include "render/Renderer.h"
#include "test_util.h"

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
        ComPtr<ID2D1RenderTarget> rt = r.device().createWicTarget(surface.Get());
        REQUIRE(rt != nullptr);
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

TEST_CASE("Renderer：高品質縮圖配置不到記憶體時回 nullptr（不丟例外），呼叫端改用一般的點陣圖") {
    // v0.9.9：畫面上的高品質縮放在 UI 執行緒做，配置失敗的例外沒有人接。
    Fixture f;
    auto a = img(40, 30);
    {
        mp::test::ImageAllocLimit limit(40 * 30 * 4); // 放大的縮圖配置不到
        const ImageBitmap* s = nullptr;
        CHECK_NOTHROW(s = f.r.scaledBitmap(a, 80, 60));
        CHECK(s == nullptr);
        const ImageBitmap* plain = f.r.imageBitmap(a); // 一般的點陣圖不用另外配置
        REQUIRE(plain != nullptr);
        CHECK(plain->bitmap != nullptr);
    }
    const ImageBitmap* s = f.r.scaledBitmap(a, 80, 60); // 配置得到了就照常
    REQUIRE(s != nullptr);
    CHECK(s->bitmap->GetPixelSize().width == 80);
}

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
        CHECK(pa != pb);
        CHECK(pa->bitmap->GetPixelSize().width == 32);
        CHECK(pb->bitmap->GetPixelSize().width == 16);
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

namespace {
// 每個像素顏色都不同的不透明圖：底圖有沒有被動過，逐像素比得出來。
std::shared_ptr<Image> patterned(int w, int h) {
    auto im = std::make_shared<Image>(w, h);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            uint8_t* p = im->px(x, y);
            p[0] = static_cast<uint8_t>(x & 255);
            p[1] = static_cast<uint8_t>((x >> 8) & 255);
            p[2] = static_cast<uint8_t>(40 + y);
            p[3] = 255;
        }
    return im;
}
// 加一個紅框矩形標註，中心 (cx, cy)。
DocState withRect(ImagePtr im, float cx, float cy, float size) {
    DocState st;
    st.image = std::move(im);
    Annotation a = makeShape(Kind::Rect, {cx, cy}, size, StrokeStyle{});
    a.id = 1;
    st.items = {a};
    return st;
}
} // namespace

TEST_CASE("Renderer::flatten：沒有標註時是底圖的複本") {
    Fixture f;
    auto im = patterned(200, 40);
    DocState st;
    st.image = im;
    Image out;
    REQUIRE(flatten(f.r.device(), st, out));
    CHECK(out.width == 200);
    CHECK(out.height == 40);
    CHECK(out.pixels == im->pixels);
}

TEST_CASE("Renderer::flatten：標註畫上去，標註以外的底圖像素不變") {
    Fixture f;
    auto im = patterned(200, 40);
    Image out;
    REQUIRE(flatten(f.r.device(), withRect(im, 20, 20, 24), out));
    REQUIRE(out.width == 200);
    REQUIRE(out.height == 40);
    int changedNear = 0, changedFar = 0;
    for (int y = 0; y < 40; ++y)
        for (int x = 0; x < 200; ++x) {
            if (memcmp(out.px(x, y), im->px(x, y), 4) == 0) continue;
            if (x < 60) ++changedNear;
            else ++changedFar;
        }
    CHECK(changedNear > 0);  // 紅框有畫上去
    CHECK(changedFar == 0);  // 離標註遠的地方和原圖一模一樣
}

TEST_CASE("Renderer::flatten：邊長到 kFlattenMaxSide 為止底圖像素正確") {
    Fixture f;
    const int sizes[][2] = {{kFlattenMaxSide, 2}, {2, kFlattenMaxSide}};
    for (auto& s : sizes) {
        int w = s[0], h = s[1];
        CAPTURE(w, h);
        auto im = patterned(w, h);
        Image out;
        REQUIRE(flatten(f.r.device(), withRect(im, w > h ? 20.0f : 1.0f, 1.0f, 6), out)); // 框一定有一段落在圖內
        long long changedNear = 0, changedAway = 0;
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) {
                if (memcmp(out.px(x, y), im->px(x, y), 4) == 0) continue;
                if (x < 60 && y < 60) ++changedNear;
                else ++changedAway;
            }
        CHECK(changedNear > 0);
        CHECK(changedAway == 0);
    }
}

TEST_CASE("Renderer::flatten：畫不出正確結果時回報失敗") {
    // 舊版在這三種尺寸都回傳一張圖、不報錯（2026-11-16 在 VM 上量的）：
    // 邊長 524288 起：每個呼叫都成功，但底圖有像素是錯的。
    // 8388608（點陣圖邊長上限）：底圖整張是錯的。8388609：render target 建不起來，回傳沒有標註的底圖。
    Fixture f;
    const int sizes[][2] = {{kFlattenMaxSide + 1, 1}, {1, kFlattenMaxSide + 1}, {8388608, 1}, {8388609, 1}};
    for (auto& s : sizes) {
        CAPTURE(s[0], s[1]);
        Image out;
        CHECK_FALSE(flatten(f.r.device(), withRect(patterned(s[0], s[1]), 1.0f, 1.0f, 6), out));
    }
    // 沒有標註就不用畫，多大都只是複製
    DocState plain;
    plain.image = patterned(kFlattenMaxSide + 1, 1);
    Image out;
    CHECK(flatten(f.r.device(), plain, out));
    CHECK(out.pixels == plain.image->pixels);
}

TEST_CASE("故障開關：pathGeometry、畫刷建不出來時回 nullptr 並帶原因；關掉就恢復") {
    Fixture f;
    RenderDevice& d = f.r.device();
    auto triangle = [](ID2D1GeometrySink& sink) {
        sink.BeginFigure(D2D1::Point2F(0, 0), D2D1_FIGURE_BEGIN_FILLED);
        sink.AddLine(D2D1::Point2F(10, 0));
        sink.AddLine(D2D1::Point2F(10, 10));
        sink.EndFigure(D2D1_FIGURE_END_CLOSED);
    };
    HRESULT hr = E_FAIL;
    CHECK(d.pathGeometry(triangle, &hr) != nullptr);
    CHECK(hr == S_OK);
    d.testFaults.geometry = true;
    CHECK(d.pathGeometry(triangle, &hr) == nullptr);
    CHECK(hr == E_OUTOFMEMORY);
    d.testFaults.geometry = false;
    CHECK(d.pathGeometry(triangle, &hr) != nullptr);
    CHECK(hr == S_OK);

    Surface& s = f.r.surface();
    d.testFaults.brush = true;
    CHECK(s.brush(D2D1::ColorF(0, 1, 0), &hr) == nullptr);
    CHECK(hr == E_OUTOFMEMORY);
    CHECK(s.brushCount() == 0); // 失敗的不留在快取裡
    d.testFaults.brush = false;
    CHECK(s.brush(D2D1::ColorF(0, 1, 0), &hr) != nullptr);
    CHECK(hr == S_OK);
    CHECK(s.brushCount() == 1);
}

TEST_CASE("MosaicCache::get：整個在圖外回 nullptr 但不算失敗；點陣圖建不出來回 nullptr 並帶原因") {
    Fixture f;
    auto a = img(64, 64);
    MosaicCache cache;
    MosaicShape inside{{8, 8, 40, 40}, 8}, outside{{100, 100, 140, 140}, 8};
    HRESULT hr = E_FAIL;
    const MosaicCache::Tile* t = cache.get(f.r.rt(), a, 1, inside, &hr);
    CHECK(t != nullptr);
    CHECK(hr == S_OK);
    hr = E_FAIL;
    CHECK(cache.get(f.r.rt(), a, 2, outside, &hr) == nullptr);
    CHECK(hr == S_OK);
    CHECK(cache.get(f.r.rt(), a, 3, inside, &hr, true) == nullptr);
    CHECK(hr == E_OUTOFMEMORY);
    hr = E_FAIL;
    CHECK(cache.get(f.r.rt(), a, 4, outside, &hr, true) == nullptr); // 沒東西可畫就不用建，也就不會失敗
    CHECK(hr == S_OK);
    CHECK(cache.get(f.r.rt(), a, 3, inside, &hr) != nullptr); // 上次失敗的，下次重建
    CHECK(hr == S_OK);
}

TEST_CASE("Renderer：馬賽克小圖只留這一幀畫過的") {
    Fixture f;
    auto a = img(64, 64);
    Annotation m = makeShape(Kind::Mosaic, {32, 32}, 32, StrokeStyle{});
    m.id = 7;
    m.as<MosaicShape>()->blockSize = 8;
    f.frame([&] {
        drawAnnotation(f.r.paintCtx(a, f.r.imageBitmap(a)), m);
    });
    CHECK(f.r.cachedMosaicCount() == 1);
    f.frame([&] { // 物件還在：沿用
        drawAnnotation(f.r.paintCtx(a, f.r.imageBitmap(a)), m);
    });
    CHECK(f.r.cachedMosaicCount() == 1);
    f.frame([&] { f.r.imageBitmap(a); }); // 物件刪掉了
    CHECK(f.r.cachedMosaicCount() == 0);
}

TEST_CASE("Surface：畫刷一個顏色一支，同時拿兩支不會互相改色") {
    // 以前每個 target 只有一支畫刷，每次取用就改色：先拿一支、再拿第二支，第一支也變成第二支的顏色。
    Fixture f;
    Surface& s = f.r.surface();
    ID2D1SolidColorBrush* red = s.brush(D2D1::ColorF(1, 0, 0));
    ID2D1SolidColorBrush* blue = s.brush(D2D1::ColorF(0, 0, 1));
    REQUIRE(red != nullptr);
    REQUIRE(blue != nullptr);
    CHECK(red != blue);
    CHECK(red->GetColor().r == 1.0f);
    CHECK(red->GetColor().b == 0.0f);
    CHECK(blue->GetColor().b == 1.0f);
    CHECK(s.brush(D2D1::ColorF(1, 0, 0)) == red); // 同一個顏色是同一支
    CHECK(s.brush(Color::rgb(255, 0, 0)) == red);
    CHECK(s.brush(D2D1::ColorF(1, 0, 0, 0.5f)) != red); // alpha 不同就是另一支
    CHECK(s.brushCount() == 3);
}

TEST_CASE("Surface：換 target 時畫刷一起丟；沒有 target 時拿不到畫刷") {
    Fixture f;
    Surface& s = f.r.surface();
    REQUIRE(s.brush(D2D1::ColorF(1, 0, 0)) != nullptr);
    CHECK(s.brushCount() == 1);
    f.r.discardTarget();
    CHECK(s.brushCount() == 0);
    CHECK(s.rt() == nullptr);
    CHECK(s.brush(D2D1::ColorF(1, 0, 0)) == nullptr);
    s.drawUiText(L"x", D2D1::RectF(0, 0, 10, 10), 12, D2D1::ColorF(0, 0, 0)); // 沒有 target：不畫，也不當掉
}

TEST_CASE("Surface：畫刷用過太多種顏色，endFrame 清掉重來") {
    Fixture f;
    Surface& s = f.r.surface();
    for (size_t i = 0; i < Surface::kMaxBrushes; ++i) s.brush(D2D1::ColorF(static_cast<float>(i), 0, 0));
    f.r.endFrame();
    CHECK(s.brushCount() == Surface::kMaxBrushes); // 還沒超過：留著
    s.brush(D2D1::ColorF(0, 1, 0));
    f.r.endFrame();
    CHECK(s.brushCount() == 0);
    CHECK(s.brush(D2D1::ColorF(0, 1, 0)) != nullptr);
}

TEST_CASE("RenderDevice：UI 文字格式每種組合一個，建好不再改；建不出來回 nullptr、不留在快取") {
    Fixture f;
    RenderDevice& d = f.r.device();
    IDWriteTextFormat* a = d.uiFormat(13, false);
    REQUIRE(a != nullptr);
    CHECK(d.uiFormat(13, false) == a);
    IDWriteTextFormat* wrapped = d.uiFormat(13, false, DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_NEAR, true);
    REQUIRE(wrapped != nullptr);
    CHECK(wrapped != a);
    CHECK(wrapped->GetWordWrapping() == DWRITE_WORD_WRAPPING_WRAP);
    CHECK(a->GetWordWrapping() == DWRITE_WORD_WRAPPING_NO_WRAP); // 拿了換行的那個，原本的不受影響
    CHECK(a->GetParagraphAlignment() == DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    CHECK(d.uiFormat(13, true) != a);
    CHECK(d.uiFormat(13, false, DWRITE_TEXT_ALIGNMENT_CENTER) != a);
    CHECK(d.uiFormat(13, false, DWRITE_TEXT_ALIGNMENT_CENTER)->GetTextAlignment() == DWRITE_TEXT_ALIGNMENT_CENTER);
    // 字級不合法：DirectWrite 建不出格式。以前會把空指標存進快取，之後直接拿去用。
    CHECK(d.uiFormat(0, false) == nullptr);
    CHECK(d.uiFormat(-5, false) == nullptr);
    CHECK(d.uiFormat(0, false) == nullptr); // 再問一次也一樣，不會拿到快取裡的空指標以外的東西
    CHECK(d.measureUiText(L"abc", 0) == 0.0f);
    f.frame([&] { f.r.drawUiText(L"abc", D2D1::RectF(0, 0, 60, 60), 0, D2D1::ColorF(0, 0, 0)); }); // 不畫，也不當掉
}

TEST_CASE("RenderDevice：pathGeometry 建出路徑；還沒 init 時回 nullptr") {
    Fixture f;
    auto geo = f.r.device().pathGeometry([](ID2D1GeometrySink& sink) {
        sink.BeginFigure(D2D1::Point2F(0, 0), D2D1_FIGURE_BEGIN_FILLED);
        sink.AddLine(D2D1::Point2F(10, 0));
        sink.AddLine(D2D1::Point2F(10, 10));
        sink.EndFigure(D2D1_FIGURE_END_CLOSED);
    });
    REQUIRE(geo != nullptr);
    D2D1_RECT_F b{};
    REQUIRE(SUCCEEDED(geo->GetBounds(nullptr, &b)));
    CHECK(b.right == 10.0f);
    CHECK(b.bottom == 10.0f);

    RenderDevice blank; // 沒有 init：工廠是空的
    bool called = false;
    HRESULT hr = S_OK;
    CHECK(blank.pathGeometry([&](ID2D1GeometrySink&) { called = true; }, &hr) == nullptr);
    CHECK_FALSE(called);
    CHECK(FAILED(hr));
    CHECK(blank.createWicTarget(f.surface.Get()) == nullptr);
    CHECK(blank.uiFormat(13, false) == nullptr);
}

TEST_CASE("MosaicCache：握著來源圖到不再畫它的那一幀；底圖換了就重做") {
    Fixture f;
    auto a = std::make_shared<Image>(64, 64);
    for (auto& b : a->pixels) b = 255; // 白
    std::weak_ptr<const Image> aWeak = a;
    Annotation m = makeShape(Kind::Mosaic, {32, 32}, 32, StrokeStyle{});
    m.id = 7;
    m.as<MosaicShape>()->blockSize = 8;
    MosaicCache cache;
    const MosaicCache::Tile* t = cache.get(f.r.rt(), a, m.id, *m.as<MosaicShape>());
    REQUIRE(t != nullptr);
    ID2D1Bitmap* first = t->bitmap.Get();
    CHECK(cache.get(f.r.rt(), a, m.id, *m.as<MosaicShape>())->bitmap.Get() == first); // 沒變：沿用
    cache.endFrame();
    a.reset();
    CHECK_FALSE(aWeak.expired()); // 快取握著，位址不會被別張圖重用
    auto b = std::make_shared<Image>(64, 64);
    const MosaicCache::Tile* t2 = cache.get(f.r.rt(), b, m.id, *m.as<MosaicShape>());
    REQUIRE(t2 != nullptr);
    CHECK(aWeak.expired()); // 換了底圖：舊的那筆被換掉
    Annotation moved = m;
    rectOf(moved)->left -= 8;
    CHECK(cache.get(f.r.rt(), b, moved.id, *moved.as<MosaicShape>()) != nullptr);
    CHECK(cache.size() == 1);
    cache.endFrame();
    cache.endFrame(); // 這一幀沒畫
    CHECK(cache.size() == 0);
    // 整個在圖外、沒有 target、沒有圖：沒有小圖可畫
    Annotation outside = m;
    (*rectOf(outside)) = {200, 200, 260, 260};
    CHECK(cache.get(f.r.rt(), b, outside.id, *outside.as<MosaicShape>()) == nullptr);
    CHECK(cache.get(nullptr, b, m.id, *m.as<MosaicShape>()) == nullptr);
    CHECK(cache.get(f.r.rt(), nullptr, m.id, *m.as<MosaicShape>()) == nullptr);
}
