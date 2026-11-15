#include <catch2/catch_test_macros.hpp>

#include <windows.h>

#include <mutex>
#include <thread>
#include <utility>
#include <vector>

#include "core/ImageOps.h"

using namespace mp;

TEST_CASE("parallelRows：每一列剛好跑一次") {
    for (int rows : {1, 63, 64, 1000}) {
        std::vector<int> hit(static_cast<size_t>(rows), 0);
        std::mutex m;
        parallelRows(rows, [&](int y0, int y1) {
            std::lock_guard<std::mutex> lock(m);
            for (int y = y0; y < y1; ++y) ++hit[static_cast<size_t>(y)];
        });
        for (int h : hit) CHECK(h == 1);
    }
}

TEST_CASE("parallelRows：工作執行緒跟著呼叫端的優先順序") {
    for (int priority : {THREAD_PRIORITY_BELOW_NORMAL, THREAD_PRIORITY_NORMAL}) {
        std::vector<int> seen;
        std::mutex m;
        std::thread caller([&] {
            SetThreadPriority(GetCurrentThread(), priority);
            parallelRows(1000, [&](int, int) {
                std::lock_guard<std::mutex> lock(m);
                seen.push_back(GetThreadPriority(GetCurrentThread()));
            });
        });
        caller.join();
        REQUIRE_FALSE(seen.empty());
        for (int p : seen) CHECK(p == priority);
    }
}

static void set(Image& img, int x, int y, uint8_t b, uint8_t g, uint8_t r, uint8_t a = 255) {
    uint8_t* p = img.px(x, y);
    p[0] = b; p[1] = g; p[2] = r; p[3] = a;
}

TEST_CASE("rotateCCW90：3x2 轉成 2x3，右上角跑到左上角") {
    Image img(3, 2);
    img.dpiX = 72; img.dpiY = 144;
    set(img, 2, 0, 1, 2, 3); // 右上
    set(img, 0, 1, 9, 9, 9); // 左下
    Image out = rotateCCW90(img);
    CHECK(out.width == 2);
    CHECK(out.height == 3);
    CHECK(out.dpiX == 144);
    CHECK(out.dpiY == 72);
    CHECK(out.px(0, 0)[0] == 1); // 原右上 (2,0) -> (0, 0)
    CHECK(out.px(1, 2)[0] == 9); // 原左下 (0,1) -> (1, 2)
}

TEST_CASE("rotateCCW90 四次回到原圖") {
    Image img(5, 3);
    for (int y = 0; y < 3; ++y)
        for (int x = 0; x < 5; ++x) set(img, x, y, static_cast<uint8_t>(x), static_cast<uint8_t>(y), 0);
    Image r = rotateCCW90(rotateCCW90(rotateCCW90(rotateCCW90(img))));
    CHECK(r.pixels == img.pixels);
}

TEST_CASE("cropImage：取出指定範圍，dpi 照抄") {
    Image img(5, 4);
    img.dpiX = 72; img.dpiY = 144;
    for (int y = 0; y < 4; ++y)
        for (int x = 0; x < 5; ++x) set(img, x, y, static_cast<uint8_t>(x), static_cast<uint8_t>(y), 7);
    Image out = cropImage(img, 1, 2, 3, 2);
    REQUIRE(out.width == 3);
    REQUIRE(out.height == 2);
    CHECK(out.dpiX == 72);
    CHECK(out.dpiY == 144);
    for (int y = 0; y < 2; ++y)
        for (int x = 0; x < 3; ++x) {
            CHECK(out.px(x, y)[0] == x + 1);
            CHECK(out.px(x, y)[1] == y + 2);
        }
}

TEST_CASE("cropImage：範圍夾到圖內，夾完是空的回空圖") {
    Image img(4, 4);
    set(img, 3, 3, 9, 9, 9);
    Image out = cropImage(img, 2, 2, 10, 10);
    CHECK(out.width == 2);
    CHECK(out.height == 2);
    CHECK(out.px(1, 1)[0] == 9);
    CHECK(cropImage(img, -3, -3, 4, 4).width == 1);
    CHECK(cropImage(img, 5, 0, 2, 2).empty());
    CHECK(cropImage(img, 0, 0, 0, 3).empty());
}

TEST_CASE("cropImage：整張圖原樣") {
    Image img(3, 2);
    set(img, 2, 1, 4, 5, 6);
    CHECK(cropImage(img, 0, 0, 3, 2).pixels == img.pixels);
}

TEST_CASE("applyExifOrientation") {
    Image img(2, 1);
    set(img, 0, 0, 1, 1, 1);
    set(img, 1, 0, 2, 2, 2);
    SECTION("1 原樣") { CHECK(applyExifOrientation(img, 1).pixels == img.pixels); }
    SECTION("2 水平翻轉") {
        Image o = applyExifOrientation(img, 2);
        CHECK(o.px(0, 0)[0] == 2);
    }
    SECTION("6 順時針 90") {
        Image o = applyExifOrientation(img, 6);
        CHECK(o.width == 1);
        CHECK(o.height == 2);
        CHECK(o.px(0, 0)[0] == 1);
        CHECK(o.px(0, 1)[0] == 2);
    }
    SECTION("8 逆時針 90") {
        Image o = applyExifOrientation(img, 8);
        CHECK(o.px(0, 0)[0] == 2);
        CHECK(o.px(0, 1)[0] == 1);
    }
    SECTION("3 轉 180") {
        Image o = applyExifOrientation(img, 3);
        CHECK(o.px(0, 0)[0] == 2);
    }
}

TEST_CASE("resampleFast 縮小取平均") {
    Image img(4, 4);
    for (int y = 0; y < 4; ++y)
        for (int x = 0; x < 4; ++x) set(img, x, y, (x < 2) ? 0 : 200, 0, 0);
    Image o = resampleFast(img, 2, 2);
    CHECK(o.width == 2);
    CHECK(o.px(0, 0)[0] == 0);
    CHECK(o.px(1, 0)[0] == 200);
    Image one = resampleFast(img, 1, 1);
    CHECK(one.px(0, 0)[0] == 100);
}

TEST_CASE("resampleFast 放大雙線性，純色不變") {
    Image img(2, 2);
    for (int y = 0; y < 2; ++y)
        for (int x = 0; x < 2; ++x) set(img, x, y, 10, 20, 30, 255);
    Image o = resampleFast(img, 5, 7);
    CHECK(o.width == 5);
    CHECK(o.height == 7);
    for (int y = 0; y < 7; ++y)
        for (int x = 0; x < 5; ++x) {
            CHECK(o.px(x, y)[0] == 10);
            CHECK(o.px(x, y)[2] == 30);
        }
}

TEST_CASE("resample（Lanczos3）相同大小回傳複本") {
    Image img(3, 3);
    CHECK(resample(img, 3, 3).pixels == img.pixels);
    CHECK(resampleFast(img, 3, 3).pixels == img.pixels);
}

TEST_CASE("resample（Lanczos3）純色放大縮小都不變") {
    Image img(9, 7);
    for (int y = 0; y < 7; ++y)
        for (int x = 0; x < 9; ++x) set(img, x, y, 10, 20, 30, 255);
    for (auto [w, h] : {std::pair{23, 31}, std::pair{4, 3}, std::pair{1, 1}, std::pair{200, 2}}) {
        Image o = resample(img, w, h);
        REQUIRE(o.width == w);
        REQUIRE(o.height == h);
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) {
                CHECK(static_cast<int>(o.px(x, y)[0]) == 10);
                CHECK(static_cast<int>(o.px(x, y)[1]) == 20);
                CHECK(static_cast<int>(o.px(x, y)[2]) == 30);
                CHECK(static_cast<int>(o.px(x, y)[3]) == 255);
            }
    }
}

TEST_CASE("resample（Lanczos3）縮小：左黑右白的邊緣保持兩側，整張平均接近中間值") {
    Image img(64, 8);
    for (int y = 0; y < 8; ++y)
        for (int x = 0; x < 64; ++x) set(img, x, y, (x < 32) ? 0 : 200, 0, 0);
    Image o = resample(img, 8, 2);
    REQUIRE(o.width == 8);
    CHECK(o.px(0, 0)[0] < 10);
    CHECK(o.px(7, 1)[0] > 190);
    Image one = resample(img, 1, 1);
    CHECK(one.px(0, 0)[0] > 85);
    CHECK(one.px(0, 0)[0] < 115);
}

TEST_CASE("resample（Lanczos3）放大：邊緣位置正確，遠離邊緣處不受 ringing 影響") {
    Image img(8, 1);
    for (int x = 0; x < 8; ++x) set(img, x, 0, (x < 4) ? 0 : 200, 0, 0);
    Image o = resample(img, 32, 4);
    REQUIRE(o.height == 4);
    CHECK(o.px(0, 0)[0] == 0);   // 最左離邊緣 4 個來源像素，lanczos3 支援 3，不受影響
    CHECK(o.px(31, 3)[0] == 200);
    CHECK(o.px(15, 0)[0] < 100); // 邊緣左側
    CHECK(o.px(16, 0)[0] > 100); // 邊緣右側
    // 大圖走多執行緒路徑（>= 64 列），結果仍是純色
    Image big(300, 300);
    for (int y = 0; y < 300; ++y)
        for (int x = 0; x < 300; ++x) set(big, x, y, 77, 88, 99, 255);
    Image ob = resample(big, 150, 450);
    CHECK(ob.px(0, 0)[0] == 77);
    CHECK(ob.px(149, 449)[1] == 88);
    CHECK(ob.px(75, 225)[2] == 99);
}

TEST_CASE("mosaicBlocks 每格取平均，區域夾到圖內") {
    Image img(8, 8);
    for (int y = 0; y < 8; ++y)
        for (int x = 0; x < 8; ++x) set(img, x, y, (x % 2 == 0) ? 0 : 100, 0, 0);
    Image m = mosaicBlocks(img, 0, 0, 8, 8, 4);
    CHECK(m.width == 2);
    CHECK(m.height == 2);
    CHECK(m.px(0, 0)[0] == 50);
    Image partial = mosaicBlocks(img, -10, -10, 3, 3, 4); // 夾成 [0,3)，一格
    CHECK(partial.width == 1);
    CHECK(partial.height == 1);
    CHECK(mosaicBlocks(img, 5, 5, 5, 9, 4).empty());
    CHECK(mosaicBlocks(img, 0, 0, 8, 8, 0).empty());
}

TEST_CASE("hasTransparency 與 compositeOnWhite（premultiplied 疊在白底上）") {
    Image img(3, 1);
    set(img, 0, 0, 0, 0, 0, 0);       // 全透明
    set(img, 1, 0, 0, 0, 128, 128);   // 半透明紅
    set(img, 2, 0, 255, 0, 0, 255);   // 不透明藍
    CHECK(hasTransparency(img));
    compositeOnWhite(img);
    CHECK_FALSE(hasTransparency(img));
    auto is = [&](int x, int b, int g, int r) {
        const uint8_t* p = img.px(x, 0);
        return p[0] == b && p[1] == g && p[2] == r && p[3] == 255;
    };
    CHECK(is(0, 255, 255, 255));
    CHECK(is(1, 127, 127, 255));
    CHECK(is(2, 255, 0, 0));

    Image opaque(2, 2);
    for (int y = 0; y < 2; ++y)
        for (int x = 0; x < 2; ++x) set(opaque, x, y, 1, 2, 3);
    CHECK_FALSE(hasTransparency(opaque));
}

TEST_CASE("applyExifOrientation：八種方向逐像素對照 EXIF 的定義") {
    // 3×2，每個像素的值是 y * 3 + x。
    const int W = 3, H = 2;
    Image img(W, H);
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) set(img, x, y, static_cast<uint8_t>(y * W + x), 0, 0);
    auto src = [&](int x, int y) { return y * W + x; };
    for (int o = 1; o <= 8; ++o) {
        INFO("orientation " << o);
        Image out = applyExifOrientation(img, o);
        const bool swapped = o >= 5;
        REQUIRE(out.width == (swapped ? H : W));
        REQUIRE(out.height == (swapped ? W : H));
        for (int y = 0; y < out.height; ++y)
            for (int x = 0; x < out.width; ++x) {
                int want = 0;
                switch (o) {
                case 1: want = src(x, y); break;
                case 2: want = src(W - 1 - x, y); break;         // 左右鏡射
                case 3: want = src(W - 1 - x, H - 1 - y); break; // 轉 180
                case 4: want = src(x, H - 1 - y); break;         // 上下鏡射
                case 5: want = src(y, x); break;                 // 沿主對角線翻
                case 6: want = src(y, H - 1 - x); break;         // 順時針 90
                case 7: want = src(W - 1 - y, H - 1 - x); break; // 沿副對角線翻
                case 8: want = src(W - 1 - y, x); break;         // 逆時針 90
                }
                INFO("at " << x << "," << y);
                CHECK(out.px(x, y)[0] == want);
            }
    }
    CHECK(applyExifOrientation(img, 0).pixels == img.pixels); // 範圍外的值當成 1
    CHECK(applyExifOrientation(img, 9).pixels == img.pixels);
}

TEST_CASE("toStraight / toPremultiplied：換算、不透明與全透明的像素不動、尺寸與 DPI 跟著走") {
    StraightImage s(4, 1);
    s.dpiX = 150;
    s.dpiY = 72;
    auto put = [&](int x, uint8_t b, uint8_t g, uint8_t r, uint8_t a) {
        uint8_t* p = s.px(x, 0);
        p[0] = b; p[1] = g; p[2] = r; p[3] = a;
    };
    put(0, 10, 20, 30, 255);  // 不透明
    put(1, 200, 100, 50, 128); // 半透明
    put(2, 0, 0, 0, 0);       // 全透明
    put(3, 255, 255, 255, 1);
    const uint8_t* buffer = s.pixels.data();
    Image p = toPremultiplied(std::move(s));
    CHECK(p.pixels.data() == buffer); // 緩衝區是搬過去的，沒有複製
    CHECK(p.width == 4);
    CHECK(p.height == 1);
    CHECK(p.dpiX == 150);
    CHECK(p.dpiY == 72);
    CHECK(p.px(0, 0)[2] == 30);
    CHECK(p.px(1, 0)[0] == 100); // 200 × 128 / 255
    CHECK(p.px(1, 0)[1] == 50);
    CHECK(p.px(1, 0)[2] == 25);
    CHECK(p.px(1, 0)[3] == 128);
    CHECK(p.px(3, 0)[0] == 1);
    for (int x = 0; x < 4; ++x)
        for (int c = 0; c < 3; ++c) CHECK(p.px(x, 0)[c] <= p.px(x, 0)[3]); // premultiplied：顏色不超過 alpha
    CHECK(hasTransparency(p));
    StraightImage back = toStraight(std::move(p));
    CHECK(back.pixels.data() == buffer);
    CHECK(back.dpiX == 150);
    CHECK(back.px(0, 0)[0] == 10);
    CHECK(std::abs(back.px(1, 0)[0] - 200) <= 1); // 捨入誤差一階以內
    CHECK(std::abs(back.px(1, 0)[1] - 100) <= 1);
    CHECK(std::abs(back.px(1, 0)[2] - 50) <= 1);
    CHECK(back.px(2, 0)[3] == 0);
    CHECK(back.px(3, 0)[0] == 255);
    CHECK(hasTransparency(back));
}

TEST_CASE("premultiplyChannel / unpremultiplyChannel") {
    CHECK(premultiplyChannel(200, 255) == 200);
    CHECK(premultiplyChannel(200, 0) == 0);
    CHECK(premultiplyChannel(255, 128) == 128);
    CHECK(unpremultiplyChannel(128, 128) == 255);
    CHECK(unpremultiplyChannel(64, 128) == 128);
    CHECK(unpremultiplyChannel(7, 255) == 7);
    CHECK(unpremultiplyChannel(7, 0) == 7);     // alpha 0：沒有顏色可還原，原樣
    CHECK(unpremultiplyChannel(200, 100) == 255); // 不合法的輸入（顏色大於 alpha）夾在 255
}
