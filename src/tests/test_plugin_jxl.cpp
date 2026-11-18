// JPEG XL plugin（plugins\jxl\jxl.dll，建置時放在測試程式旁的 plugins\ 底下）：測試裡用 libjxl 的編碼器現造 .jxl，
// 經過 loadImage / readImageSize 走真的 plugin。
#include <catch2/catch_test_macros.hpp>

#include <jxl/encode.h>

#include <cstdlib>
#include <string>
#include <vector>

#include "core/ImageIO.h"
#include "core/Plugins.h"
#include "test_util.h"

using namespace mp;
using namespace mp::test;

namespace {

struct JxlOptions {
    bool lossless = true;
    bool alpha = false;
    bool gray = false;
    bool container = false;
    int orientation = 1;
};

// frames：每一格的像素（交錯排列，色版數 = 灰階 1 / RGB 3，加 alpha 再多 1）。多於一格就是動畫。
std::string encodeJxl(int w, int h, const std::vector<std::vector<uint8_t>>& frames, const JxlOptions& o) {
    JxlEncoder* enc = JxlEncoderCreate(nullptr);
    REQUIRE(enc);
    if (o.container) REQUIRE(JxlEncoderUseContainer(enc, JXL_TRUE) == JXL_ENC_SUCCESS);
    JxlBasicInfo bi;
    JxlEncoderInitBasicInfo(&bi);
    bi.xsize = static_cast<uint32_t>(w);
    bi.ysize = static_cast<uint32_t>(h);
    bi.bits_per_sample = 8;
    bi.num_color_channels = o.gray ? 1 : 3;
    bi.num_extra_channels = o.alpha ? 1 : 0;
    bi.alpha_bits = o.alpha ? 8 : 0;
    bi.uses_original_profile = o.lossless ? JXL_TRUE : JXL_FALSE;
    bi.orientation = static_cast<JxlOrientation>(o.orientation);
    if (frames.size() > 1) {
        bi.have_animation = JXL_TRUE;
        bi.animation.tps_numerator = 10;
        bi.animation.tps_denominator = 1;
    }
    REQUIRE(JxlEncoderSetBasicInfo(enc, &bi) == JXL_ENC_SUCCESS);
    JxlColorEncoding ce;
    JxlColorEncodingSetToSRGB(&ce, o.gray ? JXL_TRUE : JXL_FALSE);
    REQUIRE(JxlEncoderSetColorEncoding(enc, &ce) == JXL_ENC_SUCCESS);
    JxlEncoderFrameSettings* fs = JxlEncoderFrameSettingsCreate(enc, nullptr);
    if (o.lossless) REQUIRE(JxlEncoderSetFrameLossless(fs, JXL_TRUE) == JXL_ENC_SUCCESS);
    else REQUIRE(JxlEncoderSetFrameDistance(fs, 1.0f) == JXL_ENC_SUCCESS);
    const uint32_t channels = (o.gray ? 1u : 3u) + (o.alpha ? 1u : 0u);
    const JxlPixelFormat pf = {channels, JXL_TYPE_UINT8, JXL_NATIVE_ENDIAN, 0};
    for (const auto& f : frames) {
        if (frames.size() > 1) {
            JxlFrameHeader fh;
            JxlEncoderInitFrameHeader(&fh);
            fh.duration = 1;
            REQUIRE(JxlEncoderSetFrameHeader(fs, &fh) == JXL_ENC_SUCCESS);
        }
        REQUIRE(f.size() == static_cast<size_t>(w) * h * channels);
        REQUIRE(JxlEncoderAddImageFrame(fs, &pf, f.data(), f.size()) == JXL_ENC_SUCCESS);
    }
    JxlEncoderCloseInput(enc);
    std::string out(1 << 16, '\0');
    auto* next = reinterpret_cast<uint8_t*>(out.data());
    size_t avail = out.size();
    JxlEncoderStatus st;
    while ((st = JxlEncoderProcessOutput(enc, &next, &avail)) == JXL_ENC_NEED_MORE_OUTPUT) {
        const size_t used = static_cast<size_t>(next - reinterpret_cast<uint8_t*>(out.data()));
        out.resize(out.size() * 2);
        next = reinterpret_cast<uint8_t*>(out.data()) + used;
        avail = out.size() - used;
    }
    REQUIRE(st == JXL_ENC_SUCCESS);
    out.resize(static_cast<size_t>(next - reinterpret_cast<uint8_t*>(out.data())));
    JxlEncoderDestroy(enc);
    return out;
}

// 測試程式旁的 plugins\ 只啟用 jxl。
void activateJxl() {
    auto entries = scanPlugins(exeDir() + L"plugins");
    std::vector<PluginLoadResult> results;
    auto set = loadPlugins(entries, {L"jxl"}, results);
    REQUIRE(results.size() == 1);
    REQUIRE(results[0].error == PluginLoadError::None);
    REQUIRE(set->plugins().size() == 1);
    CHECK(set->plugins()[0].extensions == std::vector<std::wstring>{L".jxl"});
    setActivePlugins(set);
}

LoadResult loadBytes(const wchar_t* name, const std::string& bytes) {
    const std::wstring p = tempPath(name);
    writeBytes(p, bytes);
    return loadImage(p);
}

bool near(int a, int b, int tol) { return std::abs(a - b) <= tol; }

} // namespace

TEST_CASE("JPEG XL plugin：資訊與載入（隨附的那一個）") {
    activateJxl();
    const auto& p = activePlugins()->plugins()[0];
    CHECK(p.manifest.name == L"JPEG XL");
    REQUIRE(p.manifest.formats.size() == 1);
    CHECK(p.manifest.formats[0].name == L"JPEG XL image");
}

TEST_CASE("JPEG XL plugin：無損 RGBA 逐像素一樣（換成 premultiplied），格式名、寬高") {
    activateJxl();
    const int w = 4, h = 3;
    std::vector<uint8_t> px;
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            px.push_back(static_cast<uint8_t>(x * 60));      // R
            px.push_back(static_cast<uint8_t>(y * 100));     // G
            px.push_back(static_cast<uint8_t>(255 - x * 30)); // B
            px.push_back(static_cast<uint8_t>(x == 0 ? 255 : 64 * x)); // A
        }
    JxlOptions o;
    o.alpha = true;
    LoadResult r = loadBytes(L"lossless.jxl", encodeJxl(w, h, {px}, o));
    REQUIRE(r.error.ok());
    REQUIRE(r.image);
    CHECK(r.info.pluginFormat == L"JPEG XL image");
    REQUIRE(r.image->width == w);
    REQUIRE(r.image->height == h);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const uint8_t* s = &px[(static_cast<size_t>(y) * w + x) * 4];
            const uint8_t* d = r.image->px(x, y);
            INFO("x=" << x << " y=" << y);
            CHECK(d[0] == premultiplyChannel(s[2], s[3]));
            CHECK(d[1] == premultiplyChannel(s[1], s[3]));
            CHECK(d[2] == premultiplyChannel(s[0], s[3]));
            CHECK(d[3] == s[3]);
        }
    int rw = 0, rh = 0;
    CHECK(readImageSize(tempPath(L"lossless.jxl"), rw, rh));
    CHECK(rw == w);
    CHECK(rh == h);
}

TEST_CASE("JPEG XL plugin：有損 RGB 顏色接近；灰階；container 格式") {
    activateJxl();
    const int w = 16, h = 16;
    std::vector<uint8_t> rgb;
    for (int i = 0; i < w * h; ++i) rgb.insert(rgb.end(), {200, 100, 50});
    JxlOptions lossy;
    lossy.lossless = false;
    LoadResult r = loadBytes(L"lossy.jxl", encodeJxl(w, h, {rgb}, lossy));
    REQUIRE(r.image);
    const uint8_t* c = r.image->px(8, 8);
    CHECK(near(c[2], 200, 4));
    CHECK(near(c[1], 100, 4));
    CHECK(near(c[0], 50, 4));
    CHECK(c[3] == 255);

    std::vector<uint8_t> gray(static_cast<size_t>(w) * h, 77);
    JxlOptions g;
    g.gray = true;
    r = loadBytes(L"gray.jxl", encodeJxl(w, h, {gray}, g));
    REQUIRE(r.image);
    c = r.image->px(3, 3);
    CHECK(c[0] == 77);
    CHECK(c[1] == 77);
    CHECK(c[2] == 77);
    CHECK(c[3] == 255);

    JxlOptions box;
    box.container = true;
    const std::string bytes = encodeJxl(w, h, {rgb}, box);
    REQUIRE(bytes.compare(4, 4, "JXL ") == 0); // 真的是 container
    r = loadBytes(L"container.jxl", bytes);
    REQUIRE(r.image);
    CHECK(r.image->px(0, 0)[2] == 200);
}

TEST_CASE("JPEG XL plugin：方向 6（順時針轉 90 度）寬高對調，readImageSize 一樣") {
    activateJxl();
    const int w = 4, h = 2;
    std::vector<uint8_t> rgb(static_cast<size_t>(w) * h * 3, 0);
    rgb[0] = 255; // 原本左上角是紅色
    JxlOptions o;
    o.orientation = 6;
    LoadResult r = loadBytes(L"rotated.jxl", encodeJxl(w, h, {rgb}, o));
    REQUIRE(r.image);
    CHECK(r.image->width == h);
    CHECK(r.image->height == w);
    CHECK(r.image->px(h - 1, 0)[2] == 255); // 轉過之後在右上角
    CHECK(r.image->px(0, 0)[2] == 0);
    int rw = 0, rh = 0;
    CHECK(readImageSize(tempPath(L"rotated.jxl"), rw, rh));
    CHECK(rw == h);
    CHECK(rh == w);
}

TEST_CASE("JPEG XL plugin：動畫只取第一格") {
    activateJxl();
    const int w = 2, h = 2;
    std::vector<uint8_t> red, blue;
    for (int i = 0; i < w * h; ++i) {
        red.insert(red.end(), {255, 0, 0});
        blue.insert(blue.end(), {0, 0, 255});
    }
    LoadResult r = loadBytes(L"anim.jxl", encodeJxl(w, h, {red, blue}, JxlOptions{}));
    REQUIRE(r.image);
    CHECK(r.image->px(1, 1)[2] == 255);
    CHECK(r.image->px(1, 1)[0] == 0);
}

TEST_CASE("JPEG XL plugin：截斷、簽章對但內容是亂碼 → 讀不了（不當掉）；不是 JPEG XL 的不接手") {
    activateJxl();
    std::vector<uint8_t> rgb(16 * 16 * 3, 128);
    const std::string good = encodeJxl(16, 16, {rgb}, JxlOptions{});
    LoadResult r = loadBytes(L"cut.jxl", good.substr(0, good.size() / 2));
    CHECK_FALSE(r.image);
    CHECK(r.error.code == IoErrorCode::PluginFailed);
    CHECK(r.error.detail.find(L"jxl, decode returned") == 0);

    std::string junk = "\xFF\x0A";
    for (int i = 0; i < 200; ++i) junk += static_cast<char>((i * 37) & 0xFF);
    r = loadBytes(L"junk.jxl", junk);
    CHECK_FALSE(r.image);
    CHECK(r.error.code == IoErrorCode::PluginFailed);
    int w = 0, h = 0;
    CHECK_FALSE(readImageSize(tempPath(L"junk.jxl"), w, h));

    writeBytes(tempPath(L"really-png.jxl"), pngBytes(3, 3)); // 副檔名是 .jxl，內容是 PNG：內建的先接手
    r = loadImage(tempPath(L"really-png.jxl"));
    REQUIRE(r.image);
    CHECK(r.info.pluginFormat.empty());
    CHECK(r.info.format == ImageFormat::Png);
}
