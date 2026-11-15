#include <catch2/catch_test_macros.hpp>

#include <windows.h>

#include <algorithm>
#include <set>
#include <string>

#include "core/ImageFormat.h"
#include "test_util.h"

using namespace mp;

TEST_CASE("imageFormats：每種格式一列，副檔名小寫含點、不重複") {
    std::set<ImageFormat> seen;
    std::set<std::wstring> exts;
    for (const auto& f : imageFormats()) {
        CHECK(f.format != ImageFormat::Unknown);
        CHECK(seen.insert(f.format).second);
        CHECK(std::wstring(f.name).size() > 0);
        CHECK(std::wstring(f.shortName).size() > 0);
        REQUIRE_FALSE(f.extensions.empty());
        for (const wchar_t* e : f.extensions) {
            std::wstring ext = e;
            CHECK(ext[0] == L'.');
            CHECK(std::all_of(ext.begin(), ext.end(), [](wchar_t c) { return c == L'.' || (c >= L'a' && c <= L'z'); }));
            CHECK(exts.insert(ext).second); // 一個副檔名只屬於一種格式
        }
        CHECK(imageFormatInfo(f.format) == &f);
    }
    CHECK(imageFormatInfo(ImageFormat::Unknown) == nullptr);
    CHECK(std::wstring(imageFormatName(ImageFormat::Unknown)) == L"Image");
    CHECK(std::wstring(imageFormatName(ImageFormat::Png)) == L"PNG image");
}

TEST_CASE("imageFormatOfExtension：別名、不分大小寫、不認得回 Unknown") {
    CHECK(imageFormatOfExtension(L".jpg") == ImageFormat::Jpeg);
    for (auto* e : {L".jpeg", L".JPE", L".jfif"}) CHECK(imageFormatOfExtension(e) == ImageFormat::Jpeg);
    CHECK(imageFormatOfExtension(L".TIFF") == ImageFormat::Tiff);
    CHECK(imageFormatOfExtension(L".dib") == ImageFormat::Bmp);
    CHECK(imageFormatOfExtension(L".Png") == ImageFormat::Png);
    CHECK(imageFormatOfExtension(L".webp") == ImageFormat::WebP);
    for (auto* e : {L".gif", L"", L"png", L".zip"}) CHECK(imageFormatOfExtension(e) == ImageFormat::Unknown);
}

TEST_CASE("supportedExtensions：就是表裡全部的副檔名，每個都認得") {
    size_t total = 0;
    for (const auto& f : imageFormats()) total += f.extensions.size();
    const auto& exts = supportedExtensions();
    CHECK(exts.size() == total);
    for (const auto& e : exts) {
        CHECK(isSupportedExtension(e));
        CHECK(imageFormatOfExtension(e) != ImageFormat::Unknown);
    }
    CHECK(isSupportedExtension(L".JPG"));
    CHECK_FALSE(isSupportedExtension(L".gif"));
    CHECK_FALSE(isSupportedExtension(L".zip"));
}

TEST_CASE("canSaveExtension / extensionKeepsTransparency 照表") {
    for (const auto& f : imageFormats())
        for (const wchar_t* e : f.extensions) {
            CHECK(canSaveExtension(e) == f.canSave);
            CHECK(extensionKeepsTransparency(e) == f.keepsTransparency);
        }
    for (auto* e : {L".png", L".tif", L".tiff", L".webp"}) CHECK(extensionKeepsTransparency(e));
    for (auto* e : {L".jpg", L".jpeg", L".jpe", L".jfif", L".bmp", L".dib"}) CHECK_FALSE(extensionKeepsTransparency(e));
    CHECK(canSaveExtension(L".PNG"));
    CHECK_FALSE(canSaveExtension(L".gif"));
    CHECK_FALSE(extensionKeepsTransparency(L".gif"));
}

TEST_CASE("sniffImageFormat：各格式的檔頭，太短或不認得回 Unknown") {
    auto sniff = [](const std::string& s) { return sniffImageFormat(reinterpret_cast<const unsigned char*>(s.data()), s.size()); };
    CHECK(sniff(std::string("\x89PNG\r\n\x1a\n", 8) + "rest") == ImageFormat::Png);
    CHECK(sniff("\xFF\xD8\xFF\xE0") == ImageFormat::Jpeg);
    CHECK(sniff(std::string("RIFF\x10\x00\x00\x00WEBPVP8 ", 16)) == ImageFormat::WebP);
    CHECK(sniff(std::string("II*\0", 4)) == ImageFormat::Tiff);
    CHECK(sniff(std::string("MM\0*", 4)) == ImageFormat::Tiff);
    CHECK(sniff("BM123456") == ImageFormat::Bmp);
    CHECK(sniff("") == ImageFormat::Unknown);
    CHECK(sniff("B") == ImageFormat::Unknown);
    CHECK(sniff(std::string("RIFF\x10\x00\x00\x00WAVE", 12)) == ImageFormat::Unknown);
    CHECK(sniff(std::string("\x89PNG", 4)) == ImageFormat::Unknown); // 簽章不完整
    CHECK(sniff("PK\x03\x04") == ImageFormat::Unknown);
    CHECK(sniffImageFormat(nullptr, 16) == ImageFormat::Unknown);
}

TEST_CASE("detectImageFormat：讀檔頭，不看副檔名；讀不到回 Unknown") {
    auto png = mp::test::tempPath(L"detect.png"), p = mp::test::tempPath(L"detect.jpg");
    mp::test::writePng(png, 2, 2);
    REQUIRE(MoveFileExW(png.c_str(), p.c_str(), MOVEFILE_REPLACE_EXISTING)); // 副檔名是 jpg，內容是 PNG
    CHECK(detectImageFormat(p) == ImageFormat::Png);
    mp::test::writeBytes(p, "not an image");
    CHECK(detectImageFormat(p) == ImageFormat::Unknown);
    mp::test::writeBytes(p, "");
    CHECK(detectImageFormat(p) == ImageFormat::Unknown);
    DeleteFileW(p.c_str());
    CHECK(detectImageFormat(p) == ImageFormat::Unknown);
}

TEST_CASE("檔案對話框：樣式與存檔篩選從表導出") {
    CHECK(imagePatterns(ImageFormat::Jpeg) == L"*.jpg;*.jpeg;*.jpe;*.jfif");
    CHECK(imagePatterns(ImageFormat::Png) == L"*.png");
    CHECK(imagePatterns(ImageFormat::Unknown).empty());
    std::wstring all = allImagePatterns();
    for (const auto& e : supportedExtensions()) CHECK(all.find(L"*" + e) != std::wstring::npos);
    CHECK(std::count(all.begin(), all.end(), L';') + 1 == static_cast<long long>(supportedExtensions().size()));

    // 寫得出來的格式都列在存檔對話框，不重複，PNG 排第一（預設）。
    const auto& formats = saveDialogFormats();
    REQUIRE_FALSE(formats.empty());
    CHECK(formats.front() == ImageFormat::Png);
    for (const auto& f : imageFormats())
        CHECK((std::count(formats.begin(), formats.end(), f.format) == 1) == f.canSave);
    // 篩選字串：每種格式兩段，都以 \0 結尾。
    std::wstring filter = saveDialogFilter();
    CHECK(std::count(filter.begin(), filter.end(), L'\0') == static_cast<long long>(formats.size()) * 2);
    CHECK(filter.rfind(std::wstring(L"PNG (*.png)") + L'\0' + L"*.png" + L'\0', 0) == 0);
    CHECK(filter.find(L"JPEG (*.jpg)") != std::wstring::npos);
}
