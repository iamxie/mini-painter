#include <catch2/catch_test_macros.hpp>

#include <utility>

#include "core/ImageOps.h"

using namespace mp;

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
