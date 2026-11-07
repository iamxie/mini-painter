#include <catch2/catch_test_macros.hpp>

#include <windows.h>
#include <wincodec.h>
#include <wincodecsdk.h>
#include <wrl/client.h>
#include <propvarutil.h>

#include <string>
#include <vector>

#include "core/ImageIO.h"
#include "core/Lang.h"
#include "core/Metadata.h"

using namespace mp;
using Microsoft::WRL::ComPtr;

namespace {

std::wstring tempPath(const wchar_t* name) {
    wchar_t dir[MAX_PATH];
    GetTempPathW(MAX_PATH, dir);
    return std::wstring(dir) + L"mp_test_" + name;
}

Image solid(int w, int h) {
    Image img(w, h);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            uint8_t* p = img.px(x, y);
            p[0] = 30; p[1] = 120; p[2] = 200; p[3] = 255;
        }
    return img;
}

struct Com {
    Com() { CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED); }
    ~Com() { CoUninitialize(); }
};

// 用 WIC 寫一張帶 EXIF（Orientation=6、Make=TestCam、PixelXDimension）的 JPEG 或 TIFF，另外塞一張 4×2 縮圖。
// extras：再加 IFD0 的 50341 號欄位（排在 Exif 子 IFD 之後）與 XMP 的 dc:title。
bool writeWithExif(const std::wstring& path, const GUID& container, int w, int h, bool withThumb, bool extras = false) {
    ComPtr<IWICImagingFactory> f;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&f)))) return false;
    ComPtr<IWICStream> stream;
    f->CreateStream(&stream);
    if (FAILED(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE))) return false;
    ComPtr<IWICBitmapEncoder> enc;
    f->CreateEncoder(container, nullptr, &enc);
    enc->Initialize(stream.Get(), WICBitmapEncoderNoCache);
    ComPtr<IWICBitmapFrameEncode> frame;
    ComPtr<IPropertyBag2> props;
    enc->CreateNewFrame(&frame, &props);
    frame->Initialize(props.Get());
    ComPtr<IWICMetadataQueryWriter> qw;
    if (FAILED(frame->GetMetadataQueryWriter(&qw))) return false;
    bool jpeg = IsEqualGUID(container, GUID_ContainerFormatJpeg);
    std::wstring ifd = jpeg ? L"/app1/ifd" : L"/ifd";
    PROPVARIANT v;
    PropVariantInit(&v);
    v.vt = VT_UI2; v.uiVal = 6;
    if (FAILED(qw->SetMetadataByName((ifd + L"/{ushort=274}").c_str(), &v))) return false;
    v.vt = VT_LPSTR; v.pszVal = const_cast<char*>("TestCam");
    if (FAILED(qw->SetMetadataByName((ifd + L"/{ushort=271}").c_str(), &v))) return false;
    v.vt = VT_UI4; v.ulVal = static_cast<ULONG>(w);
    if (FAILED(qw->SetMetadataByName((ifd + L"/exif/{ushort=40962}").c_str(), &v))) return false;
    v.ulVal = static_cast<ULONG>(h);
    if (FAILED(qw->SetMetadataByName((ifd + L"/exif/{ushort=40963}").c_str(), &v))) return false;
    if (extras) {
        v.vt = VT_UI2; v.uiVal = 7;
        if (FAILED(qw->SetMetadataByName((ifd + L"/{ushort=50341}").c_str(), &v))) return false;
        v.vt = VT_LPWSTR; v.pwszVal = const_cast<wchar_t*>(L"xmp-title");
        std::wstring xmp = jpeg ? L"/xmp/dc:title" : L"/ifd/xmp/dc:title";
        if (FAILED(qw->SetMetadataByName(xmp.c_str(), &v))) return false;
    }
    Image img = solid(w, h);
    frame->SetSize(w, h);
    GUID fmt = GUID_WICPixelFormat24bppBGR;
    frame->SetPixelFormat(&fmt);
    ComPtr<IWICBitmap> bmp;
    f->CreateBitmapFromMemory(w, h, GUID_WICPixelFormat32bppBGRA, static_cast<UINT>(img.stride()), static_cast<UINT>(img.pixels.size()), img.pixels.data(), &bmp);
    if (withThumb) {
        Image t = solid(4, 2);
        ComPtr<IWICBitmap> tb;
        f->CreateBitmapFromMemory(4, 2, GUID_WICPixelFormat32bppBGRA, static_cast<UINT>(t.stride()), static_cast<UINT>(t.pixels.size()), t.pixels.data(), &tb);
        frame->SetThumbnail(tb.Get());
    }
    if (FAILED(frame->WriteSource(bmp.Get(), nullptr))) return false;
    if (FAILED(frame->Commit())) return false;
    return SUCCEEDED(enc->Commit());
}

// 讀某個 query path 的整數值；沒有回傳 -1。
int readTag(const std::wstring& path, const wchar_t* key) {
    ComPtr<IWICImagingFactory> f;
    CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&f));
    ComPtr<IWICBitmapDecoder> dec;
    if (FAILED(f->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &dec))) return -1;
    ComPtr<IWICBitmapFrameDecode> frame;
    dec->GetFrame(0, &frame);
    ComPtr<IWICMetadataQueryReader> q;
    if (FAILED(frame->GetMetadataQueryReader(&q))) return -1;
    PROPVARIANT v;
    PropVariantInit(&v);
    if (FAILED(q->GetMetadataByName(key, &v))) return -1;
    int r = v.vt == VT_UI2 ? v.uiVal : v.vt == VT_UI4 ? static_cast<int>(v.ulVal) : v.vt == VT_I4 ? v.lVal : -2;
    PropVariantClear(&v);
    return r;
}

// 讀某個 query path 的字串值；沒有回傳空字串。
std::wstring readStr(const std::wstring& path, const wchar_t* key) {
    ComPtr<IWICImagingFactory> f;
    CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&f));
    ComPtr<IWICBitmapDecoder> dec;
    if (FAILED(f->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &dec))) return L"";
    ComPtr<IWICBitmapFrameDecode> frame;
    dec->GetFrame(0, &frame);
    ComPtr<IWICMetadataQueryReader> q;
    if (FAILED(frame->GetMetadataQueryReader(&q))) return L"";
    PROPVARIANT v;
    PropVariantInit(&v);
    if (FAILED(q->GetMetadataByName(key, &v))) return L"";
    std::wstring r;
    if (v.vt == VT_LPWSTR) r = v.pwszVal;
    else if (v.vt == VT_LPSTR) { std::string s = v.pszVal; r.assign(s.begin(), s.end()); }
    PropVariantClear(&v);
    return r;
}

// 縮圖寬度；沒有縮圖回傳 0。
int thumbWidth(const std::wstring& path) {
    ComPtr<IWICImagingFactory> f;
    CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&f));
    ComPtr<IWICBitmapDecoder> dec;
    if (FAILED(f->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &dec))) return 0;
    ComPtr<IWICBitmapFrameDecode> frame;
    dec->GetFrame(0, &frame);
    ComPtr<IWICBitmapSource> t;
    if (FAILED(frame->GetThumbnail(&t)) || !t) return 0;
    UINT w = 0, h = 0;
    t->GetSize(&w, &h);
    return static_cast<int>(w);
}

bool hasItem(const ImageInfo& info, const wchar_t* section, const wchar_t* label, const wchar_t* value) {
    for (const auto& s : info.sections) {
        if (s.name != section) continue;
        for (const auto& kv : s.items)
            if (kv.first == label && kv.second == value) return true;
    }
    return false;
}

// 手工造一個最小 EXIF（little endian）：IFD0 有 Orientation=6、Make="Cam"、ExifIFD 指標；
// Exif IFD 有 PixelXDimension=16（SHORT）、PixelYDimension=8（LONG）；IFD0 的 next 指向一個假的 IFD1。
std::vector<uint8_t> tinyExif() {
    std::vector<uint8_t> t = {'I', 'I', 42, 0, 8, 0, 0, 0};
    auto u16 = [&](unsigned v) { t.push_back(v & 0xFF); t.push_back((v >> 8) & 0xFF); };
    auto u32 = [&](uint32_t v) { for (int i = 0; i < 4; ++i) t.push_back((v >> (8 * i)) & 0xFF); };
    // IFD0 @8：3 entries -> 2 + 36 + 4 = 42 bytes，結束在 50
    u16(3);
    u16(0x010F); u16(2); u32(4); u32(50);            // Make，字串放在 50
    u16(0x0112); u16(3); u32(1); u16(6); u16(0);     // Orientation = 6
    u16(0x8769); u16(4); u32(1); u32(54);            // Exif IFD @54
    u32(84);                                         // next IFD（假的 IFD1）@84
    t.insert(t.end(), {'C', 'a', 'm', 0});           // @50
    // Exif IFD @54：2 entries -> 2 + 24 + 4 = 30，結束在 84
    u16(2);
    u16(0xA002); u16(3); u32(1); u16(16); u16(0);
    u16(0xA003); u16(4); u32(1); u32(8);
    u32(0);
    // 假 IFD1 @84：0 entries
    u16(0); u32(0);
    return t;
}

} // namespace

TEST_CASE("patchExifTiff：Orientation 歸 1、尺寸欄位依 sizeChanged 改、IFD1 切掉") {
    auto t = tinyExif();
    patchExifTiff(t, true, 300, 200);
    CHECK(t[8 + 2 + 12 + 8] == 1);                    // Orientation 值
    CHECK(t[8 + 2 + 12 + 9] == 0);
    CHECK(t[8 + 2 + 36] == 0);                        // next IFD = 0
    CHECK(t[8 + 2 + 36 + 1] == 0);
    CHECK((t[54 + 2 + 8] | (t[54 + 2 + 9] << 8)) == 300);
    CHECK(t[54 + 2 + 12 + 8] == 200);
    auto u = tinyExif();
    patchExifTiff(u, false, 300, 200);
    CHECK((u[54 + 2 + 8] | (u[54 + 2 + 9] << 8)) == 16); // 尺寸沒變就不動
    CHECK(u[8 + 2 + 12 + 8] == 1);
    // 壞資料不動也不炸
    std::vector<uint8_t> junk = {1, 2, 3};
    patchExifTiff(junk, true, 1, 1);
    CHECK(junk.size() == 3);
    std::vector<uint8_t> bigEndianBad = {'M', 'M', 0, 42, 0, 0, 0, 200};
    patchExifTiff(bigEndianBad, true, 1, 1);
    CHECK(bigEndianBad.size() == 8);
}

TEST_CASE("JPEG 存回 JPEG：Make 照抄、Orientation 改 1、尺寸欄位跟著像素、縮圖重新產生") {
    Com com;
    setActiveLang({});
    auto src = tempPath(L"meta_src.jpg"), dst = tempPath(L"meta_dst.jpg");
    REQUIRE(writeWithExif(src, GUID_ContainerFormatJpeg, 64, 32, true));
    REQUIRE(readTag(src, L"/app1/ifd/{ushort=274}") == 6);
    REQUIRE(thumbWidth(src) == 4);
    auto r = loadImage(src);
    REQUIRE(r.image);
    CHECK(r.image->width == 32); // orientation 6 轉正後寬高對調
    CHECK(r.info.orientation == 6);
    SaveOptions opt;
    opt.metadataSource = src;
    REQUIRE(saveImage(*r.image, dst, nullptr, opt));
    CHECK(readTag(dst, L"/app1/ifd/{ushort=274}") == 1);
    CHECK(readTag(dst, L"/app1/ifd/exif/{ushort=40962}") == 32);
    CHECK(readTag(dst, L"/app1/ifd/exif/{ushort=40963}") == 64);
    auto back = loadImage(dst);
    REQUIRE(back.image);
    CHECK(back.image->width == 32);
    CHECK(back.info.orientation == 1);
    CHECK(hasItem(back.info, L"TIFF", L"Make", L"TestCam"));
    int tw = thumbWidth(dst);
    CHECK(tw > 0);
    CHECK(tw != 4); // 不是舊的 4×2
    DeleteFileW(src.c_str());
    DeleteFileW(dst.c_str());
}

TEST_CASE("原地覆寫：metadata 來源就是要取代的檔，不能被自己鎖住") {
    Com com;
    setActiveLang({});
    auto src = tempPath(L"meta_inplace.jpg");
    REQUIRE(writeWithExif(src, GUID_ContainerFormatJpeg, 64, 32, false));
    auto r = loadImage(src);
    REQUIRE(r.image);
    SaveOptions opt;
    opt.metadataSource = src;
    std::wstring err;
    REQUIRE(saveImage(*r.image, src, &err, opt));
    auto back = loadImage(src);
    REQUIRE(back.image);
    CHECK(back.info.orientation == 1);
    CHECK(hasItem(back.info, L"TIFF", L"Make", L"TestCam"));
    DeleteFileW(src.c_str());
}

TEST_CASE("JPEG 另存 TIFF 與 WebP：Make 跟著走；另存 PNG 與 BMP 不會失敗") {
    Com com;
    setActiveLang({});
    auto src = tempPath(L"meta_src2.jpg");
    REQUIRE(writeWithExif(src, GUID_ContainerFormatJpeg, 64, 32, false));
    auto r = loadImage(src);
    REQUIRE(r.image);
    SaveOptions opt;
    opt.metadataSource = src;
    for (auto* name : {L"meta_x.tif", L"meta_x.webp", L"meta_x.png", L"meta_x.bmp"}) {
        auto dst = tempPath(name);
        std::wstring err;
        REQUIRE(saveImage(*r.image, dst, &err, opt));
        auto back = loadImage(dst);
        REQUIRE(back.image);
        CHECK(back.image->width == 32);
        std::wstring n = name;
        if (n.find(L".tif") != std::wstring::npos || n.find(L".webp") != std::wstring::npos) {
            CHECK(hasItem(back.info, L"TIFF", L"Make", L"TestCam"));
            CHECK(back.info.orientation == 1);
        }
        DeleteFileW(dst.c_str());
    }
    DeleteFileW(src.c_str());
}

TEST_CASE("WebP 存回 WebP：EXIF chunk 保留、Orientation 歸 1；WebP 另存 JPEG 也帶 Make") {
    Com com;
    setActiveLang({});
    // 先用 JPEG 來源做出一個帶 EXIF 的 WebP
    auto jpg = tempPath(L"meta_src3.jpg"), webp = tempPath(L"meta_src3.webp"), webp2 = tempPath(L"meta_dst3.webp"), jpg2 = tempPath(L"meta_dst3.jpg");
    REQUIRE(writeWithExif(jpg, GUID_ContainerFormatJpeg, 64, 32, false));
    auto r = loadImage(jpg);
    REQUIRE(r.image);
    SaveOptions fromJpg;
    fromJpg.metadataSource = jpg;
    REQUIRE(saveImage(*r.image, webp, nullptr, fromJpg));
    auto w1 = loadImage(webp);
    REQUIRE(w1.image);
    REQUIRE(hasItem(w1.info, L"TIFF", L"Make", L"TestCam"));
    CHECK(w1.info.orientation == 1);
    // WebP -> WebP（尺寸改變：PixelXDimension 要跟著改）
    Image smaller(16, 32);
    for (size_t i = 0; i < smaller.pixels.size(); i += 4) { smaller.pixels[i] = 10; smaller.pixels[i + 1] = 20; smaller.pixels[i + 2] = 30; smaller.pixels[i + 3] = 255; }
    SaveOptions fromWebp;
    fromWebp.metadataSource = webp;
    REQUIRE(saveImage(smaller, webp2, nullptr, fromWebp));
    auto w2 = loadImage(webp2);
    REQUIRE(w2.image);
    CHECK(hasItem(w2.info, L"TIFF", L"Make", L"TestCam"));
    CHECK(hasItem(w2.info, L"Exif", L"Pixel X dimension", L"16"));
    // WebP -> JPEG
    REQUIRE(saveImage(*w1.image, jpg2, nullptr, fromWebp));
    auto j2 = loadImage(jpg2);
    REQUIRE(j2.image);
    CHECK(hasItem(j2.info, L"TIFF", L"Make", L"TestCam"));
    for (auto& p : {jpg, webp, webp2, jpg2}) DeleteFileW(p.c_str());
}

TEST_CASE("metadata 走訪：排在子 IFD 之後的欄位不會因 sections 重新配置而消失") {
    // 以前拿著 sections 元素的指標走子區塊，子區塊 push_back 新 section 後指標懸空，
    // 後面的欄位寫進已釋放的記憶體（真機上是隨機當機）。
    Com com;
    setActiveLang({});
    for (auto [name, container] : {std::pair{L"walk.jpg", GUID_ContainerFormatJpeg}, std::pair{L"walk.tif", GUID_ContainerFormatTiff}}) {
        auto p = tempPath(name);
        REQUIRE(writeWithExif(p, container, 16, 8, false, true));
        auto r = loadImage(p);
        REQUIRE(r.image);
        CHECK(hasItem(r.info, L"TIFF", L"Make", L"TestCam"));
        CHECK(hasItem(r.info, L"TIFF", L"Tag 0xC4A5", L"7"));
        CHECK(hasItem(r.info, L"Exif", L"Pixel X dimension", L"16"));
        DeleteFileW(p.c_str());
    }
}

TEST_CASE("XMP 跨容器：JPEG→TIFF、TIFF→JPEG、TIFF→TIFF 都帶著走") {
    Com com;
    setActiveLang({});
    auto jpg = tempPath(L"xmp_src.jpg"), tif = tempPath(L"xmp_src.tif");
    REQUIRE(writeWithExif(jpg, GUID_ContainerFormatJpeg, 16, 8, false, true));
    REQUIRE(writeWithExif(tif, GUID_ContainerFormatTiff, 16, 8, false, true));
    REQUIRE(readStr(jpg, L"/xmp/dc:title") == L"xmp-title");
    REQUIRE(readStr(tif, L"/ifd/xmp/dc:title") == L"xmp-title");
    auto rj = loadImage(jpg), rt = loadImage(tif);
    REQUIRE(rj.image);
    REQUIRE(rt.image);
    SaveOptions fromJpg, fromTif;
    fromJpg.metadataSource = jpg;
    fromTif.metadataSource = tif;
    auto j2t = tempPath(L"xmp_j2t.tif"), t2j = tempPath(L"xmp_t2j.jpg"), t2t = tempPath(L"xmp_t2t.tif");
    REQUIRE(saveImage(*rj.image, j2t, nullptr, fromJpg));
    REQUIRE(saveImage(*rt.image, t2j, nullptr, fromTif));
    REQUIRE(saveImage(*rt.image, t2t, nullptr, fromTif));
    CHECK(readStr(j2t, L"/ifd/xmp/dc:title") == L"xmp-title");
    CHECK(readStr(t2j, L"/xmp/dc:title") == L"xmp-title");
    CHECK(readStr(t2t, L"/ifd/xmp/dc:title") == L"xmp-title");
    for (auto& p : {jpg, tif, j2t, t2j, t2t}) DeleteFileW(p.c_str());
}

TEST_CASE("TIFF 另存 JPEG：Make 跟著走，但 StripOffsets、Compression 這類 TIFF 結構欄位不搬進 APP1") {
    Com com;
    setActiveLang({});
    auto tif = tempPath(L"strip_src.tif"), jpg = tempPath(L"strip_dst.jpg");
    REQUIRE(writeWithExif(tif, GUID_ContainerFormatTiff, 16, 8, false));
    REQUIRE(readTag(tif, L"/ifd/{ushort=273}") != -1); // 來源確實有 StripOffsets
    auto r = loadImage(tif);
    REQUIRE(r.image);
    SaveOptions opt;
    opt.metadataSource = tif;
    REQUIRE(saveImage(*r.image, jpg, nullptr, opt));
    CHECK(readTag(jpg, L"/app1/ifd/{ushort=273}") == -1);
    CHECK(readTag(jpg, L"/app1/ifd/{ushort=259}") == -1);
    CHECK(readTag(jpg, L"/app1/ifd/{ushort=256}") == -1);
    auto back = loadImage(jpg);
    REQUIRE(back.image);
    CHECK(hasItem(back.info, L"TIFF", L"Make", L"TestCam"));
    CHECK(back.info.orientation == 1);
    DeleteFileW(tif.c_str());
    DeleteFileW(jpg.c_str());
}

TEST_CASE("metadataLossOnSave：同格式不警告，跨格式列出裝不下的區塊") {
    ImageInfo info;
    info.sections.push_back({L"TIFF", {{L"Make", L"X"}}});
    info.sections.push_back({L"Exif", {{L"ISO", L"100"}}});
    info.sections.push_back({L"IPTC", {{L"a", L"b"}}});
    info.sections.push_back({L"XMP", {{L"a", L"b"}}});
    info.colorProfile = L"sRGB";
    CHECK(metadataLossOnSave(info, L".jpg", L".jpeg").empty());
    CHECK(metadataLossOnSave(info, L".jpg", L".jpg").empty());
    auto toTif = metadataLossOnSave(info, L".jpg", L".tif");
    CHECK(toTif == std::vector<std::wstring>{L"IPTC"});
    auto toPng = metadataLossOnSave(info, L".jpg", L".png");
    CHECK(toPng == std::vector<std::wstring>{L"Exif", L"IPTC"});
    auto toBmp = metadataLossOnSave(info, L".jpg", L".bmp");
    CHECK(toBmp == std::vector<std::wstring>{L"Exif", L"IPTC", L"XMP", L"ICC"});
    auto toWebp = metadataLossOnSave(info, L".jpg", L".webp");
    CHECK(toWebp == std::vector<std::wstring>{L"IPTC"});
    auto tifToWebp = metadataLossOnSave(info, L".tif", L".webp");
    CHECK(tifToWebp == std::vector<std::wstring>{L"Exif", L"IPTC", L"XMP"});
    ImageInfo none;
    CHECK(metadataLossOnSave(none, L".jpg", L".bmp").empty());
}

