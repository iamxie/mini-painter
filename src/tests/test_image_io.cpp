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
        REQUIRE(saveImage(img, path, nullptr));
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
        REQUIRE(saveImage(img, path, nullptr));
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

namespace {
// 一個 tag 的 ICC：128 byte 檔頭 + tag 數 + tag 表（sig、offset、size），body 接在 144。
// tagOff / tagLen 照給的寫進 tag 表，不管 body 實際多長。
std::vector<unsigned char> iccOneTag(unsigned tagOff, unsigned tagLen, const std::vector<unsigned char>& body) {
    std::vector<unsigned char> icc(144, 0);
    auto be32 = [&](size_t off, unsigned v) {
        icc[off] = static_cast<unsigned char>(v >> 24); icc[off + 1] = static_cast<unsigned char>(v >> 16);
        icc[off + 2] = static_cast<unsigned char>(v >> 8); icc[off + 3] = static_cast<unsigned char>(v);
    };
    be32(128, 1);
    memcpy(icc.data() + 132, "desc", 4);
    be32(136, tagOff);
    be32(140, tagLen);
    icc.insert(icc.end(), body.begin(), body.end());
    return icc;
}
void putBe32(std::vector<unsigned char>& v, size_t off, unsigned x) {
    v[off] = static_cast<unsigned char>(x >> 24); v[off + 1] = static_cast<unsigned char>(x >> 16);
    v[off + 2] = static_cast<unsigned char>(x >> 8); v[off + 3] = static_cast<unsigned char>(x);
}
} // namespace

TEST_CASE("iccProfileDescription：v2 desc 的字串長度比 tag 長，回空字串") {
    // 長度欄位 0xFFFFFFF8：32 位元的 12 + n 會繞回 4。字串後面故意留一段不是 0 的位元組（在同一塊記憶體裡），
    // 檢查沒擋住的話會一路讀過 tag 的範圍。
    std::vector<unsigned char> body(20, 'A');
    memcpy(body.data(), "desc", 4);
    putBe32(body, 4, 0);
    putBe32(body, 8, 0xFFFFFFF8u);
    auto icc = iccOneTag(144, 20, body);
    size_t size = icc.size();
    icc.insert(icc.end(), 32, 'B'); // 超出 size 的部分
    icc.push_back(0);
    CHECK(iccProfileDescription(icc.data(), size).empty());
}

TEST_CASE("iccProfileDescription：tag 的位移加長度超過 4 GB，回空字串") {
    auto icc = iccOneTag(0xFFFFFFF8u, 12, std::vector<unsigned char>(32, 0));
    CHECK(iccProfileDescription(icc.data(), icc.size()).empty());
    auto icc2 = iccOneTag(0xFFFFFFF4u, 0x20, std::vector<unsigned char>(32, 0));
    CHECK(iccProfileDescription(icc2.data(), icc2.size()).empty());
}

TEST_CASE("iccProfileDescription：v4 mluc 的字串位移加長度超過 4 GB，回空字串") {
    std::vector<unsigned char> body(40, 0);
    memcpy(body.data(), "mluc", 4);
    putBe32(body, 8, 1);            // 一筆紀錄
    putBe32(body, 12, 12);          // 每筆 12 bytes
    memcpy(body.data() + 16, "enUS", 4);
    putBe32(body, 20, 0x20);        // 字串長度
    putBe32(body, 24, 0xFFFFFFF0u); // 字串位移
    auto icc = iccOneTag(144, 40, body);
    CHECK(iccProfileDescription(icc.data(), icc.size()).empty());
}

TEST_CASE("iccProfileDescription 解析 v4 mluc tag") {
    std::vector<unsigned char> body(28 + 8, 0);
    memcpy(body.data(), "mluc", 4);
    putBe32(body, 8, 1);
    putBe32(body, 12, 12);
    memcpy(body.data() + 16, "enUS", 4);
    putBe32(body, 20, 8);  // 4 個 UTF-16BE 字元
    putBe32(body, 24, 28);
    const char text[] = {0, 'P', 0, '3', 0, 'x', 0, 'y'};
    memcpy(body.data() + 28, text, 8);
    auto icc = iccOneTag(144, static_cast<unsigned>(body.size()), body);
    CHECK(iccProfileDescription(icc.data(), icc.size()) == L"P3xy");
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
