// DNG plugin（plugins\dng\dng.dll，建置時放在測試程式旁的 plugins\ 底下）。
// DngRender 直接連進來測（換執行緒數、壞檔、配置失敗），也經過 loadImage / readImageSize 走真的 plugin。
// fixture 是 raw.pixls.us 的 CC0 樣本（來源與做法見 src/tests/fixtures/dng-SOURCES.txt）。
#include <catch2/catch_test_macros.hpp>

#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <iterator>
#include <string>
#include <vector>

#include "core/Archive.h"
#include "core/ImageIO.h"
#include "core/Plugins.h"
#include "plugins/dng/DngRender.h"
#include "plugins/sdk/mpainter_plugin.h"
#include "test_util.h"

using namespace mp;
using namespace mp::test;

namespace {

const wchar_t* const kFixtures[] = {L"dng-sigma-fp-cfa.dng", L"dng-canon-proxy.dng", L"dng-iphone-proxy.dng"};

struct Out {
    int32_t w = 0, h = 0;
    int calls = 0;
    std::vector<uint8_t> px;
};

uint8_t* allocOut(void* context, int32_t w, int32_t h) {
    auto* o = static_cast<Out*>(context);
    ++o->calls;
    o->w = w;
    o->h = h;
    o->px.assign(static_cast<size_t>(w) * h * 4, 0);
    return o->px.data();
}

uint8_t* allocNothing(void*, int32_t, int32_t) { return nullptr; }

const uint8_t* bytesOf(const std::string& s) { return reinterpret_cast<const uint8_t*>(s.data()); }

std::string fixture(const wchar_t* name) { return readBytes(fixturePath(name)); }

int32_t decode(const std::string& bytes, unsigned threads, Out& out, std::string& message) {
    return mpdng::dngDecode(bytesOf(bytes), bytes.size(), threads, allocOut, &out, &message);
}

int32_t readSize(const std::string& bytes, int32_t& w, int32_t& h, std::string& message) {
    return mpdng::dngReadSize(bytesOf(bytes), bytes.size(), &w, &h, &message);
}

// 測試程式旁的 plugins\ 只啟用 dng。
void activateDng() {
    auto entries = scanPlugins(exeDir() + L"plugins");
    std::vector<PluginLoadResult> results;
    auto set = loadPlugins(entries, {L"dng"}, results);
    REQUIRE(results.size() == 1);
    REQUIRE(results[0].error == PluginLoadError::None);
    REQUIRE(set->plugins().size() == 1);
    CHECK(set->plugins()[0].extensions == std::vector<std::wstring>{L".dng"});
    setActivePlugins(set);
}

} // namespace

TEST_CASE("DNG looksLikeDng：TIFF 檔頭加上 IFD0 有 DNGVersion 才算；一般 TIFF、PNG、截短的不算") {
    for (size_t i = 0; i < std::size(kFixtures); ++i) {
        const std::string b = fixture(kFixtures[i]);
        INFO("fixture " << i);
        CHECK(mpdng::looksLikeDng(bytesOf(b), b.size()));
    }
    const std::wstring t = tempPath(L"plain-for-dng.tif");
    REQUIRE(saveImage(Image(3, 2), t));
    const std::string tif = readBytes(t);
    CHECK_FALSE(mpdng::looksLikeDng(bytesOf(tif), tif.size()));
    const std::string png = pngBytes(2, 2);
    CHECK_FALSE(mpdng::looksLikeDng(bytesOf(png), png.size()));
    const std::string dng = fixture(L"dng-canon-proxy.dng");
    CHECK_FALSE(mpdng::looksLikeDng(bytesOf(dng), 8)); // IFD 在檔外
    CHECK_FALSE(mpdng::looksLikeDng(nullptr, 0));

    // 大端序、手做的最小 IFD：一個 DNGVersion（0xC612）。
    const std::string mm = std::string("MM\x00\x2A", 4) + std::string("\x00\x00\x00\x08", 4) + std::string("\x00\x01", 2) +
                           std::string("\xC6\x12", 2) + std::string("\x00\x01", 2) + std::string("\x00\x00\x00\x04", 4) +
                           std::string("\x01\x04\x00\x00", 4) + std::string("\x00\x00\x00\x00", 4);
    REQUIRE(mm.size() == 26);
    CHECK(mpdng::looksLikeDng(bytesOf(mm), mm.size()));
    CHECK_FALSE(mpdng::looksLikeDng(bytesOf(mm), 15)); // 項目被截斷
}

TEST_CASE("DNG 解碼：寬高和 readSize 一致（方向轉過）、只配置一次、alpha 255、畫面不是單色") {
    for (size_t i = 0; i < std::size(kFixtures); ++i) {
        const std::string b = fixture(kFixtures[i]);
        int32_t w = 0, h = 0;
        std::string message;
        INFO("fixture " << i);
        REQUIRE(readSize(b, w, h, message) == MP_OK);
        Out out;
        REQUIRE(decode(b, 1, out, message) == MP_OK);
        CHECK(out.calls == 1);
        CHECK(out.w == w);
        CHECK(out.h == h);
        bool opaque = true, varied = false;
        for (size_t k = 0; k < out.px.size(); k += 4) {
            if (out.px[k + 3] != 255) opaque = false;
            if (out.px[k] != out.px[0] || out.px[k + 1] != out.px[1] || out.px[k + 2] != out.px[2]) varied = true;
        }
        CHECK(opaque);
        CHECK(varied);
    }
    // 存成 orientation 6 的直拍：轉過之後是直的；橫拍照舊是橫的。
    int32_t w = 0, h = 0;
    std::string message;
    REQUIRE(readSize(fixture(L"dng-iphone-proxy.dng"), w, h, message) == MP_OK);
    CHECK(w < h);
    REQUIRE(readSize(fixture(L"dng-canon-proxy.dng"), w, h, message) == MP_OK);
    CHECK(w > h);
}

TEST_CASE("DNG 解碼：多執行緒和單執行緒的結果逐位元組相同") {
    for (const wchar_t* f : {L"dng-sigma-fp-cfa.dng", L"dng-canon-proxy.dng"}) {
        const std::string b = fixture(f);
        std::string message;
        Out one;
        REQUIRE(decode(b, 1, one, message) == MP_OK);
        for (unsigned threads : {2u, 3u, 8u}) {
            Out many;
            INFO("threads " << threads);
            REQUIRE(decode(b, threads, many, message) == MP_OK);
            CHECK(many.w == one.w);
            CHECK(many.h == one.h);
            CHECK(many.px == one.px);
        }
    }
}

TEST_CASE("DNG 壞檔與配置失敗：回錯誤碼與英文說明，例外不會跑出來") {
    std::string message;
    Out out;
    const std::string sigma = fixture(L"dng-sigma-fp-cfa.dng");
    const std::string cut = sigma.substr(0, sigma.size() * 6 / 10); // 像素資料讀到一半就沒了
    CHECK(decode(cut, 2, out, message) == MP_ERR_CORRUPT);
    CHECK(message.find("DNG SDK error") != std::string::npos);
    CHECK(out.calls == 0);

    const std::wstring t = tempPath(L"plain-for-dng2.tif");
    REQUIRE(saveImage(Image(3, 2), t));
    int32_t w = 0, h = 0;
    message.clear();
    CHECK(readSize(readBytes(t), w, h, message) == MP_ERR_CORRUPT);
    CHECK(message == "not a valid DNG file");

    message.clear();
    const std::string proxy = fixture(L"dng-canon-proxy.dng");
    CHECK(mpdng::dngDecode(bytesOf(proxy), proxy.size(), 2, allocNothing, nullptr, &message) == MP_ERR_OUT_OF_MEMORY);
    CHECK_FALSE(message.empty());
}

TEST_CASE("DNG plugin：TIFF 檔頭先問 plugin，經過 loadImage / readImageSize；改名成 .tif 也一樣；壓縮檔裡沒副檔名的補 .dng") {
    activateDng();
    const auto& p = activePlugins()->plugins()[0];
    CHECK(p.manifest.name == L"DNG");
    CHECK(p.manifest.api == 2);
    const std::string bytes = fixture(L"dng-iphone-proxy.dng");
    for (const wchar_t* name : {L"photo.dng", L"renamed-dng.tif"}) {
        const std::wstring path = tempPath(name);
        writeBytes(path, bytes);
        LoadResult r = loadImage(path);
        REQUIRE(r.error.ok());
        REQUIRE(r.image);
        CHECK(r.info.pluginFormat == L"Adobe DNG image");
        CHECK(r.image->width < r.image->height);
        int w = 0, h = 0;
        CHECK(readImageSize(path, w, h));
        CHECK(w == r.image->width);
        CHECK(h == r.image->height);
    }
    const std::wstring noext = tempPath(L"dng-noext");
    writeBytes(noext, bytes);
    CHECK(sniffImageExtension(noext) == L".dng");

    const std::wstring bad = tempPath(L"cut.dng");
    const std::string sigma = fixture(L"dng-sigma-fp-cfa.dng");
    writeBytes(bad, sigma.substr(0, sigma.size() / 2));
    LoadResult r = loadImage(bad);
    CHECK_FALSE(r.image);
    CHECK(r.error.code == IoErrorCode::PluginFailed);
    CHECK(r.error.detail.find(L"dng, decode returned 1: ") == 0);
    setActivePlugins(nullptr);
}

// 量測，平常不跑：set MP_DNG_DIR=<放 DNG 的資料夾> 後 mp_tests "[.time-dng]"。
// 每個檔：readSize 與解出來的寬高一致；單執行緒與 defaultThreads() 各解一次的時間。
TEST_CASE("DNG 時間量測：一個資料夾裡每個 DNG 單執行緒與多執行緒各解一次", "[.time-dng]") {
    char dir[512] = {};
    size_t got = 0;
    if (getenv_s(&got, dir, sizeof(dir), "MP_DNG_DIR") != 0 || got == 0) SKIP("MP_DNG_DIR is not set");
    const std::wstring wdir(dir, dir + got - 1);
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((wdir + L"\\*").c_str(), &fd);
    REQUIRE(h != INVALID_HANDLE_VALUE);
    std::vector<std::wstring> files;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) files.push_back(fd.cFileName);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    std::printf("TIME threads=%u\n", mpdng::defaultThreads());
    for (const auto& name : files) {
        const std::string bytes = readBytes(wdir + L"\\" + name);
        int32_t w = 0, hh = 0;
        std::string message;
        const int32_t sizeRc = readSize(bytes, w, hh, message);
        double ms[2] = {0, 0};
        Out out[2];
        int32_t rc[2] = {0, 0};
        const unsigned threads[2] = {1, mpdng::defaultThreads()};
        for (int k = 0; k < 2; ++k) {
            LARGE_INTEGER f, a, b;
            QueryPerformanceFrequency(&f);
            QueryPerformanceCounter(&a);
            rc[k] = decode(bytes, threads[k], out[k], message);
            QueryPerformanceCounter(&b);
            ms[k] = 1000.0 * static_cast<double>(b.QuadPart - a.QuadPart) / static_cast<double>(f.QuadPart);
        }
        std::printf("TIME %-42ls size rc=%d %dx%d | 1 thread rc=%d %8.0f ms | %u threads rc=%d %8.0f ms %dx%d same=%d | %s\n",
                    name.c_str(), sizeRc, w, hh, rc[0], ms[0], threads[1], rc[1], ms[1], out[1].w, out[1].h,
                    out[0].px == out[1].px ? 1 : 0, message.c_str());
        if (sizeRc == MP_OK && rc[1] == MP_OK) {
            CHECK(out[1].w == w);
            CHECK(out[1].h == hh);
        }
    }
}
