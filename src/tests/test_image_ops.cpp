#include <catch2/catch_test_macros.hpp>

#include <windows.h>

#include <algorithm>
#include <mutex>
#include <new>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

#include "core/ImageOps.h"
#include "core/Parallel.h"
#include "test_util.h"

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

TEST_CASE("parallelRows：工作執行緒丟出的例外等每一段都結束後在呼叫端丟出，其他段照樣做完") {
    // v0.9.9：例外逃出工作執行緒 → terminate，整個程式結束（實測結束碼 0xC0000409）。
    std::mutex m;
    std::vector<std::pair<int, int>> done;
    int thrownSize = 0;
    bool caught = false;
    try {
        parallelRows(1000, [&](int y0, int y1) {
            if (y0 <= 500 && 500 < y1) {
                std::lock_guard<std::mutex> lk(m);
                thrownSize = y1 - y0;
                throw std::bad_alloc();
            }
            Sleep(20); // 讓丟出的那段先結束：呼叫端要等其他段做完才回來
            std::lock_guard<std::mutex> lk(m);
            done.push_back({y0, y1});
        });
    } catch (const std::bad_alloc&) {
        caught = true;
    }
    CHECK(caught);
    int rowsDone = 0;
    for (auto [y0, y1] : done) rowsDone += y1 - y0;
    CHECK(thrownSize > 0);
    CHECK(rowsDone + thrownSize == 1000); // 回來的時候其他段都做完了

    // 其他種類的例外也照樣交回來；只有一段（少於 64 列）時本來就在呼叫端跑
    CHECK_THROWS_AS(parallelRows(1000, [](int, int) { throw std::runtime_error("x"); }), std::runtime_error);
    CHECK_THROWS_AS(parallelRows(10, [](int, int) { throw std::runtime_error("x"); }), std::runtime_error);
    // 沒有例外：每一列剛好做一次
    std::vector<int> hits(1000, 0);
    parallelRows(1000, [&](int y0, int y1) {
        for (int y = y0; y < y1; ++y) ++hits[static_cast<size_t>(y)];
    });
    CHECK(std::count(hits.begin(), hits.end(), 1) == 1000);
}

TEST_CASE("resample：輸出配置不到時丟 std::bad_alloc（不會讓程式結束），配置得到時照常") {
    Image src(100, 80);
    {
        mp::test::ImageAllocLimit limit(100 * 80 * 4); // 來源那麼大的還可以，再大就失敗
        CHECK_THROWS_AS(resample(src, 300, 200), std::bad_alloc);
    }
    Image big = resample(src, 300, 200);
    CHECK(big.width == 300);
    CHECK(big.height == 200);
}

static void set(Image& img, int x, int y, uint8_t b, uint8_t g, uint8_t r, uint8_t a = 255) {
    uint8_t* p = img.px(x, y);
    p[0] = b; p[1] = g; p[2] = r; p[3] = a;
}

TEST_CASE("rotateCCW90：3x2 轉成 2x3，右上角跑到左上角") {
    Image img(3, 2);
    set(img, 2, 0, 1, 2, 3); // 右上
    set(img, 0, 1, 9, 9, 9); // 左下
    Image out = rotateCCW90(img);
    CHECK(out.width == 2);
    CHECK(out.height == 3);
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

TEST_CASE("cropImage：取出指定範圍") {
    Image img(5, 4);
    for (int y = 0; y < 4; ++y)
        for (int x = 0; x < 5; ++x) set(img, x, y, static_cast<uint8_t>(x), static_cast<uint8_t>(y), 7);
    Image out = cropImage(img, 1, 2, 3, 2);
    REQUIRE(out.width == 3);
    REQUIRE(out.height == 2);
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

TEST_CASE("compositeOnWhite：不合規的像素（色版大於 alpha）停在 255，不繞回暗色") {
    // v0.9.9：(145, 145, 145, 111) 算成 145 + 144 = 289，存進 8 位元變成 33。
    Image img(2, 1);
    set(img, 0, 0, 145, 145, 145, 111);
    set(img, 1, 0, 200, 10, 111, 111); // 只有一個色版不合規：其他照公式
    compositeOnWhite(img);
    const uint8_t* p = img.px(0, 0);
    CHECK(int(p[0]) == 255);
    CHECK(int(p[1]) == 255);
    CHECK(int(p[2]) == 255);
    CHECK(int(p[3]) == 255);
    p = img.px(1, 0);
    CHECK(int(p[0]) == 255);
    CHECK(int(p[1]) == 10 + 144);
    CHECK(int(p[2]) == 111 + 144);
    CHECK(int(p[3]) == 255);
}

TEST_CASE("resample：半透明的銳利邊緣放大、縮小後，每個像素的色版都不大於 alpha") {
    // v0.9.9：Lanczos 的負權重讓色版算過頭（例如 (145, 145, 145, 111)），鋪白底時就變成暗點。
    // 3×1：半透明白、不透明黑、不透明黑（review 給的例子），加上一張各種 alpha 交錯的圖。
    Image edge(3, 1);
    set(edge, 0, 0, 128, 128, 128, 128);
    set(edge, 1, 0, 0, 0, 0, 255);
    set(edge, 2, 0, 0, 0, 0, 255);
    Image mixed(17, 13);
    for (int y = 0; y < mixed.height; ++y)
        for (int x = 0; x < mixed.width; ++x) {
            const uint8_t a = static_cast<uint8_t>(((x * 7 + y * 13) % 5) * 60 + 15); // 15、75、135、195、255
            const uint8_t c = (x + y) % 2 ? a : 0;                                       // 白與黑交錯：邊緣最銳利
            set(mixed, x, y, c, c, c, a);
        }
    const std::pair<int, int> sizes[] = {{6, 1}, {7, 3}, {40, 31}, {9, 5}, {3, 2}};
    for (const Image* src : {&edge, &mixed})
        for (auto [w, h] : sizes) {
            CAPTURE(src->width, w, h);
            Image out = resample(*src, w, h);
            int bad = 0;
            for (int y = 0; y < out.height; ++y)
                for (int x = 0; x < out.width; ++x) {
                    const uint8_t* p = out.px(x, y);
                    if (p[0] > p[3] || p[1] > p[3] || p[2] > p[3]) ++bad;
                }
            CHECK(bad == 0);
        }
    // 不透明的圖不受影響：alpha 全是 255
    Image opaque(9, 7);
    for (int y = 0; y < opaque.height; ++y)
        for (int x = 0; x < opaque.width; ++x) set(opaque, x, y, static_cast<uint8_t>(x * 20), static_cast<uint8_t>(y * 30), (x + y) % 2 ? 255 : 0, 255);
    Image big = resample(opaque, 23, 17);
    int notOpaque = 0;
    for (int y = 0; y < big.height; ++y)
        for (int x = 0; x < big.width; ++x)
            if (big.px(x, y)[3] != 255) ++notOpaque;
    CHECK(notOpaque == 0);
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

TEST_CASE("toStraight / toPremultiplied：換算、不透明與全透明的像素不動、尺寸跟著走") {
    StraightImage s(4, 1);
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

TEST_CASE("applyExifOrientation（接手來源）：結果和複製的版本一樣；翻轉與轉 180 度沿用原本的緩衝區") {
    for (auto size : {std::pair<int, int>{5, 3}, {4, 4}, {1, 6}, {6, 1}, {1, 1}}) {
        Image img(size.first, size.second);
        for (int y = 0; y < img.height; ++y)
            for (int x = 0; x < img.width; ++x) set(img, x, y, static_cast<uint8_t>(x + 1), static_cast<uint8_t>(y + 1), 9);
        for (int o = 0; o <= 9; ++o) {
            CAPTURE(size.first, size.second, o);
            Image expected = applyExifOrientation(img, o);
            Image owned = img;
            const uint8_t* buffer = owned.pixels.data();
            Image out = applyExifOrientation(std::move(owned), o);
            CHECK(out.width == expected.width);
            CHECK(out.height == expected.height);
            CHECK(out.pixels == expected.pixels);
            const bool swaps = o >= 5 && o <= 8;
            CHECK((out.pixels.data() == buffer) == !swaps); // 不對調寬高的方向都不另外配置
        }
    }
}

TEST_CASE("Dpi：預設 96，swapped 把水平與垂直對調") {
    CHECK(Dpi{} == Dpi{96.0, 96.0});
    CHECK(swapped(Dpi{300, 150}) == Dpi{150, 300});
    CHECK(Dpi{72, 72} != Dpi{72, 96});
}
