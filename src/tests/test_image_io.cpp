#include <catch2/catch_test_macros.hpp>

#include <windows.h>

#include <string>

#include "core/ImageIO.h"
#include "core/Lang.h"
#include "core/Metadata.h"

using namespace mp;

static std::wstring tempPath(const wchar_t* name) {
    wchar_t dir[MAX_PATH];
    GetTempPathW(MAX_PATH, dir);
    return std::wstring(dir) + L"mp_test_" + name;
}

static Image testImage(bool withAlpha) {
    Image img(16, 8);
    img.dpiX = 150;
    img.dpiY = 150;
    for (int y = 0; y < img.height; ++y)
        for (int x = 0; x < img.width; ++x) {
            uint8_t* p = img.px(x, y);
            uint8_t a = (withAlpha && x < 4) ? 128 : 255;
            // premultiplied：存入時先乘 alpha
            uint8_t r = static_cast<uint8_t>(x * 16), g = static_cast<uint8_t>(y * 32), b = 64;
            p[0] = static_cast<uint8_t>(b * a / 255);
            p[1] = static_cast<uint8_t>(g * a / 255);
            p[2] = static_cast<uint8_t>(r * a / 255);
            p[3] = a;
        }
    return img;
}

static int maxDiff(const Image& a, const Image& b) {
    REQUIRE(a.width == b.width);
    REQUIRE(a.height == b.height);
    int m = 0;
    for (size_t i = 0; i < a.pixels.size(); ++i) m = std::max(m, std::abs(a.pixels[i] - b.pixels[i]));
    return m;
}

TEST_CASE("PNG 存檔再讀回，像素與 DPI 一致") {
    Image img = testImage(true);
    auto path = tempPath(L"a.png");
    std::wstring err;
    REQUIRE(saveImage(img, path, &err));
    auto r = loadImage(path);
    REQUIRE(r.image);
    CHECK(r.info.formatName == L"PNG image");
    CHECK(r.info.colorModel == L"RGB");
    CHECK(r.info.bitsPerPixel == 32);
    CHECK(maxDiff(*r.image, img) <= 1);
    CHECK(r.image->dpiX > 149);
    CHECK(r.image->dpiX < 151);
    DeleteFileW(path.c_str());
}

TEST_CASE("JPEG 存檔再讀回，大致相同、沒有 alpha") {
    Image img = testImage(false);
    auto path = tempPath(L"a.jpg");
    REQUIRE(saveImage(img, path, nullptr));
    auto r = loadImage(path);
    REQUIRE(r.image);
    CHECK(r.info.formatName == L"JPEG image");
    CHECK(r.info.bitsPerPixel == 24);
    CHECK(maxDiff(*r.image, img) <= 24);
    DeleteFileW(path.c_str());
}

TEST_CASE("BMP 與 TIFF 存檔再讀回") {
    Image img = testImage(false);
    for (auto* ext : {L"a.bmp", L"a.tif"}) {
        auto path = tempPath(ext);
        REQUIRE(saveImage(img, path, nullptr));
        auto r = loadImage(path);
        REQUIRE(r.image);
        CHECK(maxDiff(*r.image, img) == 0);
        DeleteFileW(path.c_str());
    }
}

TEST_CASE("WebP 存檔再讀回（含 alpha）") {
    Image img = testImage(true);
    auto path = tempPath(L"a.webp");
    REQUIRE(saveImage(img, path, nullptr));
    auto r = loadImage(path);
    REQUIRE(r.image);
    CHECK(r.info.formatName == L"WebP image");
    CHECK(r.info.bitsPerPixel == 32);
    CHECK(maxDiff(*r.image, img) <= 40); // 有損
    DeleteFileW(path.c_str());
}

TEST_CASE("副檔名錯誤時 loadImage 仍能靠內容判斷 WebP") {
    Image img = testImage(false);
    auto path = tempPath(L"a.webp");
    REQUIRE(saveImage(img, path, nullptr));
    auto renamed = tempPath(L"a.png");
    MoveFileExW(path.c_str(), renamed.c_str(), MOVEFILE_REPLACE_EXISTING);
    auto r = loadImage(renamed);
    CHECK(r.image);
    CHECK(r.info.formatName == L"WebP image");
    DeleteFileW(renamed.c_str());
}

TEST_CASE("不存在的檔案回傳錯誤") {
    auto r = loadImage(tempPath(L"nope.png"));
    CHECK_FALSE(r.image);
    CHECK_FALSE(r.error.empty());
}

TEST_CASE("不支援的副檔名存檔失敗") {
    std::wstring err;
    CHECK_FALSE(saveImage(testImage(false), tempPath(L"a.gif"), &err));
    CHECK_FALSE(err.empty());
    CHECK(canSaveExtension(L".png"));
    CHECK_FALSE(canSaveExtension(L".gif"));
}

TEST_CASE("存檔失敗時不會動到原檔") {
    Image img = testImage(false);
    auto path = tempPath(L"keep.png");
    REQUIRE(saveImage(img, path, nullptr));
    // 鎖住檔案讓取代失敗
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    REQUIRE(h != INVALID_HANDLE_VALUE);
    std::wstring err;
    CHECK_FALSE(saveImage(Image(2, 2), path, &err));
    CloseHandle(h);
    auto r = loadImage(path);
    REQUIRE(r.image);
    CHECK(r.image->width == 16);
    DeleteFileW(path.c_str());
}

TEST_CASE("iccProfileDescription 解析 v2 desc tag") {
    // 最小 ICC：128 byte 檔頭 + 1 個 tag + desc 內容
    std::vector<unsigned char> icc(128 + 4 + 12, 0);
    auto be32 = [&](size_t off, unsigned v) {
        icc[off] = static_cast<unsigned char>(v >> 24); icc[off + 1] = static_cast<unsigned char>(v >> 16);
        icc[off + 2] = static_cast<unsigned char>(v >> 8); icc[off + 3] = static_cast<unsigned char>(v);
    };
    be32(128, 1);
    const char* text = "sRGB test";
    size_t descOff = icc.size();
    std::vector<unsigned char> desc(12 + 10, 0);
    memcpy(desc.data(), "desc", 4);
    desc[11] = 10; // 長度含結尾 0
    memcpy(desc.data() + 12, text, 10);
    memcpy(icc.data() + 132, "desc", 4);
    be32(136, static_cast<unsigned>(descOff));
    be32(140, static_cast<unsigned>(desc.size()));
    icc.insert(icc.end(), desc.begin(), desc.end());
    CHECK(iccProfileDescription(icc.data(), icc.size()) == L"sRGB test");
    CHECK(iccProfileDescription(icc.data(), 10).empty());
}

TEST_CASE("readImageSize：PNG / JPEG / WebP 只讀檔頭，不存在回傳 false") {
    Image img(16, 8);
    for (auto* name : {L"size.png", L"size.jpg", L"size.webp"}) {
        auto path = tempPath(name);
        REQUIRE(saveImage(img, path, nullptr));
        int w = 0, h = 0;
        CHECK(readImageSize(path, w, h));
        CHECK(w == 16);
        CHECK(h == 8);
        DeleteFileW(path.c_str());
    }
    int w = 1, h = 1;
    CHECK_FALSE(readImageSize(tempPath(L"nope.png"), w, h));
    CHECK(w == 0);
}

TEST_CASE("exifTagName 與 formatFileSize") {
    setActiveLang({}); // 沒有語系就是英文 key
    CHECK(exifTagName(0x0112) == L"Orientation");
    CHECK(exifTagName(0xFFFF).empty());
    CHECK(formatFileSize(500) == L"500 bytes");
    CHECK(formatFileSize(1536) == L"1.5 KB");
    CHECK(formatFileSize(3u * 1024 * 1024) == L"3.0 MB");
}
