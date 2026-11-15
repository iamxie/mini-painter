#include <catch2/catch_test_macros.hpp>

#include <cstring>
#include <string>
#include <vector>

#include "core/Metadata.h"

using namespace mp;

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

TEST_CASE("exifTagName：認得的回英文 key，不認得回 nullptr") {
    REQUIRE(exifTagName(0x0112));
    CHECK(std::wstring(exifTagName(0x0112)) == L"Orientation");
    CHECK(std::wstring(exifTagName(0x010F)) == L"Make");
    CHECK(exifTagName(0xFFFF) == nullptr);
}

TEST_CASE("formatFileSize") {
    CHECK(formatFileSize(500) == L"500 bytes");
    CHECK(formatFileSize(500, L"%llu 位元組") == L"500 位元組"); // 不到 1 KB 的格式由呼叫端給（翻譯過的）
    CHECK(formatFileSize(1536) == L"1.5 KB");
    CHECK(formatFileSize(3u * 1024 * 1024) == L"3.0 MB");
    CHECK(formatFileSize(5ull * 1024 * 1024 * 1024) == L"5.00 GB");
}

TEST_CASE("colorModelName / metaSectionName：每個值都有名字") {
    for (auto m : {ColorModel::Rgb, ColorModel::Grayscale, ColorModel::Indexed, ColorModel::Cmyk})
        CHECK(std::wstring(colorModelName(m)).size() > 0);
    CHECK(std::wstring(colorModelName(ColorModel::Grayscale)) == L"Grayscale");
    for (auto id : {MetaSectionId::Tiff, MetaSectionId::TiffSubIfd, MetaSectionId::Exif, MetaSectionId::Gps,
                    MetaSectionId::Interop, MetaSectionId::Xmp, MetaSectionId::Iptc, MetaSectionId::Photoshop,
                    MetaSectionId::Jfif, MetaSectionId::Png, MetaSectionId::Other})
        CHECK(std::wstring(metaSectionName(id)).size() > 0);
    CHECK(std::wstring(metaSectionName(MetaSectionId::Interop)) == L"Interoperability");
    CHECK(std::wstring(metaSectionName(MetaSectionId::TiffSubIfd)) == L"TIFF (sub-IFD)");
}

namespace {
// 最小的 EXIF：IFD0 只有一個欄位。le = little endian。
std::vector<unsigned char> exifWithTag(bool le, unsigned tag, unsigned type, unsigned value) {
    std::vector<unsigned char> t = le ? std::vector<unsigned char>{'I', 'I', 42, 0, 8, 0, 0, 0}
                                      : std::vector<unsigned char>{'M', 'M', 0, 42, 0, 0, 0, 8};
    auto u16 = [&](unsigned v) {
        if (le) { t.push_back(v & 0xFF); t.push_back((v >> 8) & 0xFF); }
        else { t.push_back((v >> 8) & 0xFF); t.push_back(v & 0xFF); }
    };
    auto u32 = [&](unsigned v) {
        if (le) for (int i = 0; i < 4; ++i) t.push_back((v >> (8 * i)) & 0xFF);
        else for (int i = 3; i >= 0; --i) t.push_back((v >> (8 * i)) & 0xFF);
    };
    u16(1);
    u16(tag); u16(type); u32(1);
    if (type == 3) { u16(value); u16(0); } else u32(value);
    u32(0);
    return t;
}
} // namespace

TEST_CASE("exifOrientation：兩種位元組順序、SHORT 與 LONG、沒有或不合法回 1") {
    for (bool le : {true, false}) {
        auto t = exifWithTag(le, 0x0112, 3, 6);
        CHECK(exifOrientation(t.data(), t.size()) == 6);
        t = exifWithTag(le, 0x0112, 4, 8);
        CHECK(exifOrientation(t.data(), t.size()) == 8);
        t = exifWithTag(le, 0x0112, 3, 9); // 超出 1～8
        CHECK(exifOrientation(t.data(), t.size()) == 1);
        t = exifWithTag(le, 0x010F, 3, 6); // 別的欄位
        CHECK(exifOrientation(t.data(), t.size()) == 1);
    }
    CHECK(exifOrientation(nullptr, 0) == 1);
    auto t = exifWithTag(true, 0x0112, 3, 6);
    CHECK(exifOrientation(t.data(), 7) == 1);            // 連檔頭都不夠
    CHECK(exifOrientation(t.data(), t.size() - 6) == 1); // IFD 被截斷
    auto bad = t;
    bad[2] = 43;                                         // 不是 TIFF
    CHECK(exifOrientation(bad.data(), bad.size()) == 1);
    bad = t;
    bad[4] = 0xF8; bad[5] = 0xFF; bad[6] = 0xFF; bad[7] = 0xFF; // IFD0 位移 0xFFFFFFF8：超出範圍
    CHECK(exifOrientation(bad.data(), bad.size()) == 1);
    bad = t;
    bad[8] = 0xFF; bad[9] = 0xFF;                        // 欄位數 65535：比資料長
    CHECK(exifOrientation(bad.data(), bad.size()) == 1);
}
