#include <catch2/catch_test_macros.hpp>

#include <windows.h>

#include <string>

#include "core/ImageIO.h"
#include "test_util.h"

using namespace mp;
using mp::test::tempPath;


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
    REQUIRE(saveImage(img, path));
    auto r = loadImage(path);
    REQUIRE(r.image);
    CHECK(r.info.format == ImageFormat::Png);
    CHECK(r.info.colorModel == ColorModel::Rgb);
    CHECK(r.info.bitsPerPixel == 32);
    CHECK(maxDiff(*r.image, img) <= 1);
    CHECK(r.image->dpiX > 149);
    CHECK(r.image->dpiX < 151);
    DeleteFileW(path.c_str());
}

TEST_CASE("JPEG 存檔再讀回，大致相同、沒有 alpha") {
    Image img = testImage(false);
    auto path = tempPath(L"a.jpg");
    REQUIRE(saveImage(img, path));
    auto r = loadImage(path);
    REQUIRE(r.image);
    CHECK(r.info.format == ImageFormat::Jpeg);
    CHECK(r.info.bitsPerPixel == 24);
    CHECK(maxDiff(*r.image, img) <= 24);
    DeleteFileW(path.c_str());
}

TEST_CASE("BMP 與 TIFF 存檔再讀回") {
    Image img = testImage(false);
    for (auto* ext : {L"a.bmp", L"a.tif"}) {
        auto path = tempPath(ext);
        REQUIRE(saveImage(img, path));
        auto r = loadImage(path);
        REQUIRE(r.image);
        CHECK(maxDiff(*r.image, img) == 0);
        DeleteFileW(path.c_str());
    }
}

// 紅色三區，各 16 px 寬（對齊 JPEG 的 16×16 MCU，色度取樣不會混到隔壁區）：全透明 / alpha 128 / 不透明。premultiplied。
static Image threeZoneRed() {
    Image img(48, 16);
    for (int y = 0; y < img.height; ++y)
        for (int x = 0; x < img.width; ++x) {
            uint8_t a = x < 16 ? 0 : x < 32 ? 128 : 255;
            uint8_t* p = img.px(x, y);
            p[0] = 0;
            p[1] = 0;
            p[2] = a; // 紅 255 乘上 alpha
            p[3] = a;
        }
    return img;
}

static bool closeTo(const uint8_t* p, int b, int g, int r, int tol) {
    return std::abs(p[0] - b) <= tol && std::abs(p[1] - g) <= tol && std::abs(p[2] - r) <= tol && p[3] == 255;
}

TEST_CASE("透明圖存成 JPEG / BMP：鋪白底，半透明處和白色混合") {
    Image img = threeZoneRed();
    for (auto* name : {L"t.jpg", L"t.bmp"}) {
        bool jpeg = std::wstring(name) == L"t.jpg";
        int tol = jpeg ? 6 : 0;
        auto path = tempPath(name);
        REQUIRE(saveImage(img, path));
        auto r = loadImage(path);
        REQUIRE(r.image);
        CHECK(r.info.bitsPerPixel == 24);
        CHECK(closeTo(r.image->px(8, 8), 255, 255, 255, tol));  // 透明 → 白
        CHECK(closeTo(r.image->px(24, 8), 127, 127, 255, tol)); // 半透明紅 + 白
        CHECK(closeTo(r.image->px(40, 8), 0, 0, 255, tol));     // 不透明紅不變
        DeleteFileW(path.c_str());
    }
}

TEST_CASE("透明圖存成 PNG / TIFF：透明照留") {
    Image img = threeZoneRed();
    for (auto* name : {L"t.png", L"t.tif"}) {
        auto path = tempPath(name);
        REQUIRE(saveImage(img, path));
        auto r = loadImage(path);
        REQUIRE(r.image);
        CHECK(r.info.bitsPerPixel == 32);
        CHECK(maxDiff(*r.image, img) <= 1);
        DeleteFileW(path.c_str());
    }
}

TEST_CASE("extensionKeepsTransparency") {
    for (auto* e : {L".png", L".tif", L".tiff", L".webp"}) CHECK(extensionKeepsTransparency(e));
    for (auto* e : {L".jpg", L".jpeg", L".jpe", L".jfif", L".bmp", L".dib"}) CHECK_FALSE(extensionKeepsTransparency(e));
}

TEST_CASE("WebP 存檔再讀回（含 alpha）") {
    Image img = testImage(true);
    auto path = tempPath(L"a.webp");
    REQUIRE(saveImage(img, path));
    auto r = loadImage(path);
    REQUIRE(r.image);
    CHECK(r.info.format == ImageFormat::WebP);
    CHECK(r.info.bitsPerPixel == 32);
    CHECK(maxDiff(*r.image, img) <= 40); // 有損
    DeleteFileW(path.c_str());
}

TEST_CASE("副檔名錯誤時 loadImage 仍能靠內容判斷 WebP") {
    Image img = testImage(false);
    auto path = tempPath(L"a.webp");
    REQUIRE(saveImage(img, path));
    auto renamed = tempPath(L"a.png");
    MoveFileExW(path.c_str(), renamed.c_str(), MOVEFILE_REPLACE_EXISTING);
    auto r = loadImage(renamed);
    CHECK(r.image);
    CHECK(r.info.format == ImageFormat::WebP);
    DeleteFileW(renamed.c_str());
}

TEST_CASE("不存在的檔案回傳錯誤") {
    auto r = loadImage(tempPath(L"nope.png"));
    CHECK_FALSE(r.image);
    CHECK(r.error.code == IoErrorCode::DecodeFailed);
    CHECK(r.error.hresult != 0);
}

TEST_CASE("不支援的副檔名存檔失敗") {
    SaveResult res = saveImage(testImage(false), tempPath(L"a.gif"));
    CHECK_FALSE(res);
    CHECK(res.error.code == IoErrorCode::UnsupportedSaveFormat);
    CHECK(res.error.detail == L".gif");
    CHECK_FALSE(mp::test::exists(tempPath(L"a.gif")));
}

TEST_CASE("存檔失敗時不會動到原檔") {
    Image img = testImage(false);
    auto path = tempPath(L"keep.png");
    REQUIRE(saveImage(img, path));
    // 鎖住檔案讓取代失敗
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    REQUIRE(h != INVALID_HANDLE_VALUE);
    SaveResult res = saveImage(Image(2, 2), path);
    CloseHandle(h);
    CHECK_FALSE(res);
    CHECK(res.error.code == IoErrorCode::ReplaceFailed);
    CHECK(res.error.systemError != 0); // 帶著系統的錯誤碼：分得出被占用、沒權限、磁碟滿
    CHECK_FALSE(mp::test::exists(path + L".mp-saving.tmp"));
    auto r = loadImage(path);
    REQUIRE(r.image);
    CHECK(r.image->width == 16);
    DeleteFileW(path.c_str());
}

TEST_CASE("readImageSize：PNG / JPEG / WebP 只讀檔頭，不存在回傳 false") {
    Image img(16, 8);
    for (auto* name : {L"size.png", L"size.jpg", L"size.webp"}) {
        auto path = tempPath(name);
        REQUIRE(saveImage(img, path));
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

// ---- 以下是 reflection 批次 2 修的 bug：每一個都先在 v0.9.5 上重現過 ----

TEST_CASE("B-16：格式看內容不看副檔名，PNG 改名成 .webp / .jpg 都開得了") {
    Image img = testImage(false);
    auto png = tempPath(L"b16.png");
    REQUIRE(saveImage(img, png));
    for (auto* name : {L"b16_as.webp", L"b16_as.jpg", L"b16_as.bmp"}) {
        auto renamed = tempPath(name);
        REQUIRE(CopyFileW(png.c_str(), renamed.c_str(), FALSE));
        auto r = loadImage(renamed);
        REQUIRE(r.image);
        CHECK(r.info.format == ImageFormat::Png); // 實際的格式
        CHECK(maxDiff(*r.image, img) <= 1);
        int w = 0, h = 0;
        CHECK(readImageSize(renamed, w, h));
        CHECK(w == 16);
        CHECK(h == 8);
        DeleteFileW(renamed.c_str());
    }
    DeleteFileW(png.c_str());
}

namespace {
uint32_t crc32Of(const std::string& s) {
    uint32_t c = 0xFFFFFFFFu;
    for (unsigned char ch : s) {
        c ^= ch;
        for (int k = 0; k < 8; ++k) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u)));
    }
    return ~c;
}
std::string be32s(uint32_t v) {
    return {static_cast<char>(v >> 24), static_cast<char>(v >> 16), static_cast<char>(v >> 8), static_cast<char>(v)};
}
std::string pngChunk(const std::string& type, const std::string& data) {
    return be32s(static_cast<uint32_t>(data.size())) + type + data + be32s(crc32Of(type + data));
}
// 檔頭宣稱 w×h 的 PNG，資料只有一小段。
std::string pngClaiming(uint32_t w, uint32_t h) {
    std::string ihdr = be32s(w) + be32s(h) + std::string("\x08\x06\x00\x00\x00", 5);
    std::string idat("\x78\x9c\x63\x00\x00\x00\x01\x00\x01", 9);
    return std::string("\x89PNG\r\n\x1a\n", 8) + pngChunk("IHDR", ihdr) + pngChunk("IDAT", idat) + pngChunk("IEND", "");
}
} // namespace

TEST_CASE("B-10：檔頭宣稱超大尺寸、記憶體配不出來時，loadImage 回 OutOfMemory，不丟例外") {
    auto path = tempPath(L"b10.png");
    mp::test::writeBytes(path, pngClaiming(200000, 200000)); // 160 GB
    int w = 0, h = 0;
    REQUIRE(readImageSize(path, w, h)); // 檔頭讀得出來，所以真的會走到配置那一步
    CHECK(w == 200000);
    LoadResult r;
    REQUIRE_NOTHROW(r = loadImage(path));
    CHECK_FALSE(r.image);
    CHECK(r.error.code == IoErrorCode::OutOfMemory);
    DeleteFileW(path.c_str());
}

TEST_CASE("readImageSize：寬高是 0 或檔案是空的回 false") {
    auto path = tempPath(L"empty.png");
    mp::test::writeBytes(path, "");
    int w = 5, h = 5;
    CHECK_FALSE(readImageSize(path, w, h));
    CHECK(w == 0);
    CHECK(h == 0);
    CHECK_FALSE(loadImage(path).image);
    DeleteFileW(path.c_str());
}

TEST_CASE("saveImage：沒有 metadataSource 時 metadataDropped 是 false") {
    for (auto* name : {L"nd.png", L"nd.jpg", L"nd.webp", L"nd.tif", L"nd.bmp"}) {
        auto path = tempPath(name);
        SaveResult res = saveImage(testImage(false), path);
        CHECK(res);
        CHECK_FALSE(res.metadataDropped);
        DeleteFileW(path.c_str());
    }
}
