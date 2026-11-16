#include <catch2/catch_test_macros.hpp>

#include <windows.h>
#include <psapi.h>

#include <cstdio>

#include "core/ImageIO.h"
#include "core/ImageOps.h"
#include "test_util.h"

using namespace mp;
using namespace mp::test;

// 量測用，平常不跑（標籤開頭是點）。一次只跑一個，數字才是那個動作的尖峰：
//   mp_tests "[.mem-save-png]"
// 印出行程的 commit 尖峰（PeakPagefileUsage）與一張圖的大小。尖峰減掉動作開始前的用量，除以圖的大小，
// 就是「這個動作同時握著幾張圖」。

namespace {

constexpr int kW = 8000, kH = 6000; // 4800 萬像素，一張 183 MB

double mb(size_t bytes) { return bytes / (1024.0 * 1024.0); }

size_t commitNow() {
    PROCESS_MEMORY_COUNTERS pmc{sizeof(pmc)};
    GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc));
    return pmc.PagefileUsage;
}
size_t commitPeak() {
    PROCESS_MEMORY_COUNTERS pmc{sizeof(pmc)};
    GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc));
    return pmc.PeakPagefileUsage;
}

// 有內容的圖（純色的圖編碼器幾乎不用記憶體）。transparent：左半邊半透明。
Image bigImage(bool transparent) {
    Image im(kW, kH);
    for (int y = 0; y < kH; ++y) {
        uint8_t* p = im.row(y);
        for (int x = 0; x < kW; ++x, p += 4) {
            uint8_t a = transparent && x < kW / 2 ? 128 : 255;
            p[0] = static_cast<uint8_t>((x * 7 + y) * a / 255);
            p[1] = static_cast<uint8_t>((x + y * 3) * a / 255);
            p[2] = static_cast<uint8_t>((x ^ y) * a / 255);
            p[3] = a;
        }
    }
    return im;
}

void report(const char* what, size_t before, size_t imageBytes) {
    size_t peak = commitPeak();
    double extra = peak > before ? mb(peak - before) : 0.0;
    std::printf("MEM %-28s image %.0f MB, before %.0f MB, peak %.0f MB, extra %.0f MB = %.2f images\n", what, mb(imageBytes),
                mb(before), mb(peak), extra, extra / mb(imageBytes));
    std::fflush(stdout);
}

void measureSave(const char* what, const wchar_t* file, bool transparent) {
    Image im = bigImage(transparent);
    std::wstring path = tempPath(file);
    size_t before = commitNow();
    REQUIRE(saveImage(im, path).ok());
    report(what, before, im.pixels.size());
}

} // namespace

TEST_CASE("記憶體：不透明的圖存 PNG", "[.mem-save-png]") { measureSave("save opaque -> png", L"mem.png", false); }
TEST_CASE("記憶體：不透明的圖存 JPEG", "[.mem-save-jpg]") { measureSave("save opaque -> jpg", L"mem.jpg", false); }
TEST_CASE("記憶體：不透明的圖存 WebP", "[.mem-save-webp]") { measureSave("save opaque -> webp", L"mem.webp", false); }
TEST_CASE("記憶體：有透明的圖存 PNG", "[.mem-save-alpha-png]") { measureSave("save alpha -> png", L"mem_a.png", true); }
TEST_CASE("記憶體：有透明的圖存 JPEG", "[.mem-save-alpha-jpg]") { measureSave("save alpha -> jpg", L"mem_a.jpg", true); }

namespace {
void measureOrientation(const char* what, int orientation) {
    Image im = bigImage(false);
    size_t bytes = im.pixels.size();
    size_t before = commitNow();
    Image out = applyExifOrientation(std::move(im), orientation);
    REQUIRE(out.pixels.size() == bytes);
    report(what, before, bytes);
}
} // namespace

// 解碼後依 EXIF orientation 轉正。extra 是除了來源那一張以外多握的量。
TEST_CASE("記憶體：orientation 2（左右翻）", "[.mem-orient-2]") { measureOrientation("orientation 2 (flip)", 2); }
TEST_CASE("記憶體：orientation 3（轉 180）", "[.mem-orient-3]") { measureOrientation("orientation 3 (180)", 3); }
TEST_CASE("記憶體：orientation 6（轉 90）", "[.mem-orient-6]") { measureOrientation("orientation 6 (90)", 6); }
TEST_CASE("記憶體：orientation 5（轉置）", "[.mem-orient-5]") { measureOrientation("orientation 5 (transpose)", 5); }

#include "render_fixture.h"

namespace {
// 壓平（標註畫到底圖上）。extra 是底圖以外多握的量，含壓平的結果那一張。
void measureFlatten(const char* what, Kind kind) {
    RenderFixture f;
    auto base = std::make_shared<Image>(bigImage(false));
    Annotation a = makeShape(kind, {400, 400}, 600, StrokeStyle{});
    a.id = 1;
    size_t before = commitNow();
    Image out = f.flat(base, {a});
    REQUIRE(out.pixels.size() == base->pixels.size());
    report(what, before, base->pixels.size());
}
} // namespace

TEST_CASE("記憶體：壓平一個矩形", "[.mem-flatten]") { measureFlatten("flatten rect", Kind::Rect); }
TEST_CASE("記憶體：壓平一個放大鏡", "[.mem-flatten-magnifier]") { measureFlatten("flatten magnifier", Kind::Magnifier); }
