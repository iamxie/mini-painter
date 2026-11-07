#include <catch2/catch_test_macros.hpp>

#include <cstdlib>

#include "core/ColorAdjust.h"

using namespace mp;

namespace {

// 直接寫 straight BGRA，再呼叫 premultiply。
Image solid(int w, int h, uint8_t r, uint8_t g, uint8_t b, uint8_t a = 255) {
    Image img(w, h);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            uint8_t* p = img.px(x, y);
            p[0] = b;
            p[1] = g;
            p[2] = r;
            p[3] = a;
        }
    premultiply(img);
    return img;
}

Image gradient(int w, int h) {
    Image img(w, h);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            uint8_t* p = img.px(x, y);
            p[0] = static_cast<uint8_t>((x * 255) / (w - 1));
            p[1] = static_cast<uint8_t>((y * 255) / (h - 1));
            p[2] = static_cast<uint8_t>(((x + y) * 127) / (w + h - 2));
            p[3] = static_cast<uint8_t>((x % 3 == 0) ? 128 : ((y % 4 == 0) ? 0 : 255));
        }
    premultiply(img);
    return img;
}

bool near(int a, int b, int tol) { return std::abs(a - b) <= tol; }

} // namespace

TEST_CASE("isIdentity only for defaults") {
    ColorAdjustParams p;
    CHECK(p.isIdentity());
    p.sepia = 0.1f;
    CHECK_FALSE(p.isIdentity());
    ColorAdjustParams q;
    q.wbGainB = 1.1f;
    CHECK_FALSE(q.isIdentity());
}

TEST_CASE("identity params keep pixels byte-identical") {
    Image src = gradient(17, 13);
    src.dpiX = 300;
    src.dpiY = 150;
    Image out = applyColorAdjust(src, ColorAdjustParams{});
    CHECK(out.width == src.width);
    CHECK(out.height == src.height);
    CHECK(out.dpiX == 300);
    CHECK(out.dpiY == 150);
    CHECK(out.pixels == src.pixels);
}

TEST_CASE("alpha is copied unchanged") {
    Image src = gradient(17, 13);
    ColorAdjustParams p;
    p.exposure = 0.7f;
    p.saturation = -0.3f;
    p.clarity = 0.5f;
    Image out = applyColorAdjust(src, p);
    REQUIRE(out.pixels.size() == src.pixels.size());
    for (size_t i = 3; i < src.pixels.size(); i += 4) REQUIRE(out.pixels[i] == src.pixels[i]);
}

TEST_CASE("exposure +1 doubles mid gray") {
    Image src = solid(4, 4, 64, 64, 64);
    ColorAdjustParams p;
    p.exposure = 1.0f;
    Image out = applyColorAdjust(src, p);
    const uint8_t* px = out.px(1, 1);
    CHECK(near(px[0], 128, 2));
    CHECK(near(px[1], 128, 2));
    CHECK(near(px[2], 128, 2));
}

TEST_CASE("exposure works on semi-transparent pixel in straight space") {
    Image src = solid(2, 2, 64, 64, 64, 128);
    ColorAdjustParams p;
    p.exposure = 1.0f;
    Image out = applyColorAdjust(src, p);
    unpremultiply(out);
    CHECK(near(out.px(0, 0)[2], 128, 3));
    CHECK(out.px(0, 0)[3] == 128);
}

TEST_CASE("saturation -1 makes gray") {
    Image src = gradient(16, 16);
    ColorAdjustParams p;
    p.saturation = -1.0f;
    Image out = applyColorAdjust(src, p);
    unpremultiply(out);
    for (int y = 0; y < 16; ++y)
        for (int x = 0; x < 16; ++x) {
            const uint8_t* q = out.px(x, y);
            if (q[3] != 255) continue; // 半透明像素還原後有捨入誤差
            CHECK(q[0] == q[1]);
            CHECK(q[1] == q[2]);
        }
    Image s2 = solid(2, 2, 200, 50, 10);
    Image o2 = applyColorAdjust(s2, p);
    CHECK(o2.px(0, 0)[0] == o2.px(0, 0)[1]);
    CHECK(o2.px(0, 0)[1] == o2.px(0, 0)[2]);
}

TEST_CASE("black point and white point remap") {
    Image src = solid(2, 2, 128, 128, 128);
    ColorAdjustParams p;
    p.blackPoint = 0.5f;
    Image out = applyColorAdjust(src, p);
    CHECK(out.px(0, 0)[2] <= 1); // 128/255 略大於 0.5
    ColorAdjustParams q;
    q.whitePoint = 0.5f;
    Image out2 = applyColorAdjust(src, q);
    CHECK(out2.px(0, 0)[2] == 255);
}

TEST_CASE("midtone below 0.5 brightens, above darkens") {
    Image src = solid(2, 2, 128, 128, 128);
    ColorAdjustParams p;
    p.midtone = 0.3f;
    CHECK(applyColorAdjust(src, p).px(0, 0)[1] > 140);
    p.midtone = 0.7f;
    CHECK(applyColorAdjust(src, p).px(0, 0)[1] < 116);
}

TEST_CASE("contrast, shadows, highlights direction") {
    Image dark = solid(2, 2, 40, 40, 40);
    Image bright = solid(2, 2, 220, 220, 220);
    ColorAdjustParams p;
    p.contrast = 0.5f;
    CHECK(applyColorAdjust(dark, p).px(0, 0)[1] < 40);
    CHECK(applyColorAdjust(bright, p).px(0, 0)[1] > 220);
    ColorAdjustParams s;
    s.shadows = 1.0f;
    CHECK(applyColorAdjust(dark, s).px(0, 0)[1] > 60);
    ColorAdjustParams h;
    h.highlights = -1.0f;
    CHECK(applyColorAdjust(bright, h).px(0, 0)[1] < 200);
}

TEST_CASE("temperature and tint") {
    Image src = solid(2, 2, 128, 128, 128);
    ColorAdjustParams p;
    p.temperature = 1.0f;
    Image o = applyColorAdjust(src, p);
    CHECK(o.px(0, 0)[2] > 128 + 30);
    CHECK(o.px(0, 0)[0] < 128 - 30);
    ColorAdjustParams t;
    t.tint = 1.0f;
    CHECK(applyColorAdjust(src, t).px(0, 0)[1] < 128 - 30);
    t.tint = -1.0f;
    CHECK(applyColorAdjust(src, t).px(0, 0)[1] > 128 + 30);
}

TEST_CASE("sepia on gray gives R > G > B") {
    Image src = solid(2, 2, 128, 128, 128);
    ColorAdjustParams p;
    p.sepia = 1.0f;
    Image out = applyColorAdjust(src, p);
    const uint8_t* q = out.px(0, 0);
    CHECK(q[2] > q[1]);
    CHECK(q[1] > q[0]);
}

TEST_CASE("histogram of solid image") {
    Image src = solid(100, 100, 200, 200, 200);
    auto h = luminanceHistogram(src);
    CHECK(h[200] == 10000u);
    uint32_t total = 0;
    for (auto v : h) total += v;
    CHECK(total == 10000u);
}

TEST_CASE("autoLevels finds range 50..200") {
    Image src(151, 10);
    for (int y = 0; y < 10; ++y)
        for (int x = 0; x < 151; ++x) {
            uint8_t* p = src.px(x, y);
            p[0] = p[1] = p[2] = static_cast<uint8_t>(50 + x);
            p[3] = 255;
        }
    ColorAdjustParams base;
    base.sepia = 0.25f;
    ColorAdjustParams r = autoLevels(src, base);
    CHECK(std::abs(r.blackPoint - 50.0f / 255.0f) <= 2.0f / 255.0f);
    CHECK(std::abs(r.whitePoint - 200.0f / 255.0f) <= 2.0f / 255.0f);
    CHECK(r.sepia == 0.25f);
}

TEST_CASE("autoLevels keeps whitePoint above blackPoint") {
    Image src = solid(10, 10, 120, 120, 120);
    ColorAdjustParams r = autoLevels(src, ColorAdjustParams{});
    CHECK(r.whitePoint > r.blackPoint + 0.0099f);
    Image white = solid(10, 10, 255, 255, 255);
    ColorAdjustParams w = autoLevels(white, ColorAdjustParams{});
    CHECK(w.whitePoint <= 1.0f);
    CHECK(w.whitePoint > w.blackPoint + 0.0099f);
}

TEST_CASE("setWhiteBalanceFrom neutralizes warm tint") {
    Image src = solid(20, 20, 200, 150, 100);
    ColorAdjustParams p;
    setWhiteBalanceFrom(p, src, 50, -3); // 超出範圍，夾回圖內
    CHECK(p.wbGainR < 1.0f);
    CHECK(p.wbGainB > 1.0f);
    Image out = applyColorAdjust(src, p);
    const uint8_t* q = out.px(10, 10);
    CHECK(near(q[0], q[1], 2));
    CHECK(near(q[1], q[2], 2));
}

TEST_CASE("setWhiteBalanceFrom guards dark channel and clamps gains") {
    Image src = solid(5, 5, 250, 120, 0);
    ColorAdjustParams p;
    setWhiteBalanceFrom(p, src, 2, 2);
    CHECK(p.wbGainB == 1.0f);
    CHECK(p.wbGainR >= 0.2f);
    CHECK(p.wbGainR <= 5.0f);
}

TEST_CASE("clarity changes edges, leaves flat image") {
    Image flat = solid(40, 40, 120, 80, 60);
    ColorAdjustParams p;
    p.clarity = 1.0f;
    Image of = applyColorAdjust(flat, p);
    for (size_t i = 0; i < flat.pixels.size(); ++i) REQUIRE(near(of.pixels[i], flat.pixels[i], 1));

    Image edge(40, 40);
    for (int y = 0; y < 40; ++y)
        for (int x = 0; x < 40; ++x) {
            uint8_t v = x < 20 ? 80 : 170;
            uint8_t* q = edge.px(x, y);
            q[0] = q[1] = q[2] = v;
            q[3] = 255;
        }
    Image oe = applyColorAdjust(edge, p);
    CHECK(oe.px(19, 20)[1] < 80); // 暗側變更暗
    CHECK(oe.px(20, 20)[1] > 170); // 亮側變更亮
}
