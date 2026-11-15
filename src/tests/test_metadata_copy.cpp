#include <catch2/catch_test_macros.hpp>

#include <windows.h>
#include <wincodec.h>
#include <wincodecsdk.h>
#include <wrl/client.h>
#include <propvarutil.h>

#include <algorithm>
#include <string>
#include <vector>

#include "core/ImageIO.h"
#include "core/Lang.h"
#include "core/Metadata.h"
#include "test_util.h"

using namespace mp;
using Microsoft::WRL::ComPtr;

namespace {

using mp::test::tempPath;

Image solid(int w, int h) { return mp::test::solidImage(w, h, 30, 120, 200); }

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

// section / label 用英文名（翻譯 key）比。
bool hasItem(const ImageInfo& info, const wchar_t* section, const wchar_t* label, const wchar_t* value) {
    for (const auto& s : info.sections) {
        if (std::wstring(metaSectionName(s.id)) != section) continue;
        for (const auto& item : s.items)
            if (item.label == label && item.value.text == value) return true;
    }
    return false;
}

bool hasSection(const ImageInfo& info, MetaSectionId id) {
    for (const auto& s : info.sections)
        if (s.id == id) return true;
    return false;
}

// 只有名字、內容隨便的區塊（給 metadataLossOnSave 的測試）。
MetaSection sectionOf(MetaSectionId id) {
    MetaSection s;
    s.id = id;
    s.items.push_back({L"a", false, {}});
    return s;
}

// JPEG：EXIF（Make）加一個 Interoperability IFD。
bool writeJpegWithInterop(const std::wstring& path) {
    ComPtr<IWICImagingFactory> f;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&f)))) return false;
    ComPtr<IWICStream> stream;
    f->CreateStream(&stream);
    if (FAILED(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE))) return false;
    ComPtr<IWICBitmapEncoder> enc;
    f->CreateEncoder(GUID_ContainerFormatJpeg, nullptr, &enc);
    enc->Initialize(stream.Get(), WICBitmapEncoderNoCache);
    ComPtr<IWICBitmapFrameEncode> frame;
    ComPtr<IPropertyBag2> props;
    enc->CreateNewFrame(&frame, &props);
    frame->Initialize(props.Get());
    ComPtr<IWICMetadataQueryWriter> qw;
    if (FAILED(frame->GetMetadataQueryWriter(&qw))) return false;
    PROPVARIANT v;
    PropVariantInit(&v);
    v.vt = VT_LPSTR; v.pszVal = const_cast<char*>("TestCam");
    if (FAILED(qw->SetMetadataByName(L"/app1/ifd/{ushort=271}", &v))) return false;
    v.vt = VT_LPSTR; v.pszVal = const_cast<char*>("R98");
    if (FAILED(qw->SetMetadataByName(L"/app1/ifd/exif/interop/{ushort=1}", &v))) return false;
    Image img = solid(16, 8);
    frame->SetSize(16, 8);
    GUID fmt = GUID_WICPixelFormat24bppBGR;
    frame->SetPixelFormat(&fmt);
    ComPtr<IWICBitmap> bmp;
    f->CreateBitmapFromMemory(16, 8, GUID_WICPixelFormat32bppBGRA, static_cast<UINT>(img.stride()), static_cast<UINT>(img.pixels.size()), img.pixels.data(), &bmp);
    if (FAILED(frame->WriteSource(bmp.Get(), nullptr))) return false;
    if (FAILED(frame->Commit())) return false;
    return SUCCEEDED(enc->Commit());
}

// 把檔案裡 EXIF 的 Orientation（SHORT、值 1）直接改成 value。saveImage 存出來的一律是 1，測試要別的值只能這樣改。
void patchOrientationInFile(const std::wstring& path, char value) {
    std::string bytes = mp::test::readBytes(path);
    const std::string le("\x12\x01\x03\x00\x01\x00\x00\x00\x01\x00", 10), be("\x01\x12\x00\x03\x00\x00\x00\x01\x00\x01", 10);
    size_t at = bytes.find(le);
    if (at != std::string::npos) {
        bytes[at + 8] = value;
    } else {
        at = bytes.find(be);
        REQUIRE(at != std::string::npos);
        bytes[at + 9] = value;
    }
    mp::test::writeBytes(path, bytes);
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

TEST_CASE("patchExifTiff：IFD0 的位移接近 4 GB，原樣不動") {
    // 32 位元的 off + 2 會繞回 0 或 1，檢查沒擋住的話接著就讀 4 GB 外的位元組。
    for (uint32_t off : {0xFFFFFFFEu, 0xFFFFFFFFu}) {
        std::vector<uint8_t> t = {'I', 'I', 42, 0, 0, 0, 0, 0, 1, 0, 0x12, 0x01, 3, 0, 1, 0, 0, 0, 6, 0, 0, 0, 0, 0, 0, 0};
        for (int i = 0; i < 4; ++i) t[4 + i] = static_cast<uint8_t>(off >> (8 * i));
        auto before = t;
        patchExifTiff(t, true, 300, 200);
        CHECK(t == before);
    }
}

TEST_CASE("patchExifTiff：Exif IFD 的位移接近 4 GB，只改 IFD0") {
    auto t = tinyExif();
    for (int i = 0; i < 4; ++i) t[8 + 2 + 24 + 8 + i] = static_cast<uint8_t>(0xFFFFFFFEu >> (8 * i)); // Exif IFD 指標
    auto exifPart = std::vector<uint8_t>(t.begin() + 54, t.end());
    patchExifTiff(t, true, 300, 200);
    CHECK(t[8 + 2 + 12 + 8] == 1); // Orientation 照樣歸 1
    CHECK(std::vector<uint8_t>(t.begin() + 54, t.end()) == exifPart); // 原本的 Exif IFD 沒被動到
}

TEST_CASE("JPEG 存回 JPEG：Make 照抄、Orientation 改 1、尺寸欄位跟著像素、縮圖重新產生") {
    Com com;
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
    REQUIRE(saveImage(*r.image, dst, opt));
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
    auto src = tempPath(L"meta_inplace.jpg");
    REQUIRE(writeWithExif(src, GUID_ContainerFormatJpeg, 64, 32, false));
    auto r = loadImage(src);
    REQUIRE(r.image);
    SaveOptions opt;
    opt.metadataSource = src;
    REQUIRE(saveImage(*r.image, src, opt));
    auto back = loadImage(src);
    REQUIRE(back.image);
    CHECK(back.info.orientation == 1);
    CHECK(hasItem(back.info, L"TIFF", L"Make", L"TestCam"));
    DeleteFileW(src.c_str());
}

TEST_CASE("JPEG 另存 TIFF 與 WebP：Make 跟著走；另存 PNG 與 BMP 不會失敗") {
    Com com;
    auto src = tempPath(L"meta_src2.jpg");
    REQUIRE(writeWithExif(src, GUID_ContainerFormatJpeg, 64, 32, false));
    auto r = loadImage(src);
    REQUIRE(r.image);
    SaveOptions opt;
    opt.metadataSource = src;
    for (auto* name : {L"meta_x.tif", L"meta_x.webp", L"meta_x.png", L"meta_x.bmp"}) {
        auto dst = tempPath(name);
        SaveResult saved = saveImage(*r.image, dst, opt);
        REQUIRE(saved);
        CHECK_FALSE(saved.metadataDropped);
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
    // 先用 JPEG 來源做出一個帶 EXIF 的 WebP
    auto jpg = tempPath(L"meta_src3.jpg"), webp = tempPath(L"meta_src3.webp"), webp2 = tempPath(L"meta_dst3.webp"), jpg2 = tempPath(L"meta_dst3.jpg");
    REQUIRE(writeWithExif(jpg, GUID_ContainerFormatJpeg, 64, 32, false));
    auto r = loadImage(jpg);
    REQUIRE(r.image);
    SaveOptions fromJpg;
    fromJpg.metadataSource = jpg;
    REQUIRE(saveImage(*r.image, webp, fromJpg));
    auto w1 = loadImage(webp);
    REQUIRE(w1.image);
    REQUIRE(hasItem(w1.info, L"TIFF", L"Make", L"TestCam"));
    CHECK(w1.info.orientation == 1);
    // WebP -> WebP（尺寸改變：PixelXDimension 要跟著改）
    Image smaller(16, 32);
    for (size_t i = 0; i < smaller.pixels.size(); i += 4) { smaller.pixels[i] = 10; smaller.pixels[i + 1] = 20; smaller.pixels[i + 2] = 30; smaller.pixels[i + 3] = 255; }
    SaveOptions fromWebp;
    fromWebp.metadataSource = webp;
    REQUIRE(saveImage(smaller, webp2, fromWebp));
    auto w2 = loadImage(webp2);
    REQUIRE(w2.image);
    CHECK(hasItem(w2.info, L"TIFF", L"Make", L"TestCam"));
    CHECK(hasItem(w2.info, L"Exif", L"Pixel X dimension", L"16"));
    // WebP -> JPEG
    REQUIRE(saveImage(*w1.image, jpg2, fromWebp));
    auto j2 = loadImage(jpg2);
    REQUIRE(j2.image);
    CHECK(hasItem(j2.info, L"TIFF", L"Make", L"TestCam"));
    for (auto& p : {jpg, webp, webp2, jpg2}) DeleteFileW(p.c_str());
}

TEST_CASE("metadata 走訪：排在子 IFD 之後的欄位不會因 sections 重新配置而消失") {
    // 以前拿著 sections 元素的指標走子區塊，子區塊 push_back 新 section 後指標懸空，
    // 後面的欄位寫進已釋放的記憶體（真機上是隨機當機）。
    Com com;
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
    REQUIRE(saveImage(*rj.image, j2t, fromJpg));
    REQUIRE(saveImage(*rt.image, t2j, fromTif));
    REQUIRE(saveImage(*rt.image, t2t, fromTif));
    CHECK(readStr(j2t, L"/ifd/xmp/dc:title") == L"xmp-title");
    CHECK(readStr(t2j, L"/xmp/dc:title") == L"xmp-title");
    CHECK(readStr(t2t, L"/ifd/xmp/dc:title") == L"xmp-title");
    for (auto& p : {jpg, tif, j2t, t2j, t2t}) DeleteFileW(p.c_str());
}

TEST_CASE("TIFF 另存 JPEG：Make 跟著走，但 StripOffsets、Compression 這類 TIFF 結構欄位不搬進 APP1") {
    Com com;
    auto tif = tempPath(L"strip_src.tif"), jpg = tempPath(L"strip_dst.jpg");
    REQUIRE(writeWithExif(tif, GUID_ContainerFormatTiff, 16, 8, false));
    REQUIRE(readTag(tif, L"/ifd/{ushort=273}") != -1); // 來源確實有 StripOffsets
    auto r = loadImage(tif);
    REQUIRE(r.image);
    SaveOptions opt;
    opt.metadataSource = tif;
    REQUIRE(saveImage(*r.image, jpg, opt));
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
    info.format = ImageFormat::Jpeg;
    info.sections = {sectionOf(MetaSectionId::Tiff), sectionOf(MetaSectionId::Exif), sectionOf(MetaSectionId::Iptc),
                     sectionOf(MetaSectionId::Xmp)};
    info.colorProfile = L"sRGB";
    using Lost = std::vector<std::wstring>;
    CHECK(metadataLossOnSave(info, ImageFormat::Jpeg).empty());
    CHECK(metadataLossOnSave(info, ImageFormat::Tiff) == Lost{L"IPTC"});
    CHECK(metadataLossOnSave(info, ImageFormat::Png) == Lost{L"Exif", L"IPTC", L"XMP"}); // PNG 只留 ICC
    CHECK(metadataLossOnSave(info, ImageFormat::Bmp) == Lost{L"Exif", L"IPTC", L"XMP", L"ICC"});
    CHECK(metadataLossOnSave(info, ImageFormat::WebP) == Lost{L"IPTC"});
    info.format = ImageFormat::Tiff;
    CHECK(metadataLossOnSave(info, ImageFormat::WebP) == Lost{L"Exif", L"IPTC", L"XMP"});
    ImageInfo none;
    none.format = ImageFormat::Jpeg;
    CHECK(metadataLossOnSave(none, ImageFormat::Bmp).empty());
    // 來源不是認得的格式：沒有「同格式」可言，照目標裝得下什麼算。
    info.format = ImageFormat::Unknown;
    CHECK(metadataLossOnSave(info, ImageFormat::Bmp) == Lost{L"Exif", L"IPTC", L"XMP", L"ICC"});
}

TEST_CASE("metadataCarried / iccCarried：同格式全部帶，BMP 什麼都不帶") {
    for (auto f : {ImageFormat::Jpeg, ImageFormat::Tiff, ImageFormat::Png, ImageFormat::WebP, ImageFormat::Bmp}) {
        for (auto id : {MetaSectionId::Exif, MetaSectionId::Xmp, MetaSectionId::Iptc, MetaSectionId::Photoshop,
                        MetaSectionId::Jfif, MetaSectionId::Png, MetaSectionId::Other}) {
            CHECK(metadataCarried(f, f, id));
            if (f != ImageFormat::Bmp) CHECK_FALSE(metadataCarried(f, ImageFormat::Bmp, id));
        }
        CHECK(iccCarried(f, f));
        CHECK(iccCarried(f, ImageFormat::Bmp) == (f == ImageFormat::Bmp));
    }
    // Exif 這一家同進同出。
    for (auto id : {MetaSectionId::Tiff, MetaSectionId::TiffSubIfd, MetaSectionId::Gps, MetaSectionId::Interop})
        for (auto dst : {ImageFormat::Jpeg, ImageFormat::Tiff, ImageFormat::Png, ImageFormat::WebP})
            CHECK(metadataCarried(ImageFormat::Jpeg, dst, id) == metadataCarried(ImageFormat::Jpeg, dst, MetaSectionId::Exif));
    CHECK_FALSE(metadataCarried(ImageFormat::Tiff, ImageFormat::WebP, MetaSectionId::Exif));
    CHECK_FALSE(metadataCarried(ImageFormat::Png, ImageFormat::WebP, MetaSectionId::Xmp));
}

// ---- 以下是 reflection 批次 2 修的 bug：每一個都先在 v0.9.5 上重現過 ----

TEST_CASE("B-14：區塊用 id 認，介面語言不影響 metadataLossOnSave") {
    Com com;
    auto jpg = tempPath(L"b14.jpg");
    REQUIRE(writeJpegWithInterop(jpg));
    auto en = loadImage(jpg);
    REQUIRE(en.image);
    REQUIRE(hasSection(en.info, MetaSectionId::Interop));
    auto toTif = metadataLossOnSave(en.info, ImageFormat::Tiff);
    auto toPng = metadataLossOnSave(en.info, ImageFormat::Png);
    CHECK(std::find(toTif.begin(), toTif.end(), L"Interoperability") == toTif.end()); // Exif 這一家，TIFF 裝得下
    CHECK(std::count(toPng.begin(), toPng.end(), L"Exif") == 1);                      // 合併成一項
    CHECK(std::find(toPng.begin(), toPng.end(), L"Interoperability") == toPng.end());
    // 換成中文介面再讀一次：結果一樣（v0.9.5：TIFF 多一項「互通性」，PNG 的 Exif 拆成兩項）。
    mp::test::LangGuard guard;
    setActiveLang(parseLangText(L"_code=zh-TW\nInteroperability=互通性\nTIFF (sub-IFD)=TIFF（子 IFD）\nOther=其他\n"));
    auto zh = loadImage(jpg);
    REQUIRE(zh.image);
    CHECK(metadataLossOnSave(zh.info, ImageFormat::Tiff) == toTif);
    CHECK(metadataLossOnSave(zh.info, ImageFormat::Png) == toPng);
    DeleteFileW(jpg.c_str());
}

TEST_CASE("B-04：WebP 來源的 XMP 輸出成 JPEG / TIFF 帶得過去；輸出成 PNG 帶不過去，會列在警告裡") {
    Com com;
    auto jpg = tempPath(L"b04.jpg"), webp = tempPath(L"b04.webp");
    REQUIRE(writeWithExif(jpg, GUID_ContainerFormatJpeg, 64, 32, false, true));
    auto r = loadImage(jpg);
    REQUIRE(r.image);
    SaveOptions fromJpg;
    fromJpg.metadataSource = jpg;
    REQUIRE(saveImage(*r.image, webp, fromJpg));
    auto w = loadImage(webp);
    REQUIRE(w.image);
    REQUIRE(hasSection(w.info, MetaSectionId::Xmp)); // WebP 裡有 XMP
    SaveOptions fromWebp;
    fromWebp.metadataSource = webp;
    struct Case { const wchar_t* name; const wchar_t* key; };
    for (Case c : {Case{L"b04_out.jpg", L"/xmp/dc:title"}, Case{L"b04_out.tif", L"/ifd/xmp/dc:title"}}) {
        auto dst = tempPath(c.name);
        SaveResult saved = saveImage(*w.image, dst, fromWebp);
        REQUIRE(saved);
        CHECK_FALSE(saved.metadataDropped);
        CHECK(readStr(dst, c.key) == L"xmp-title"); // 內容真的在
        auto back = loadImage(dst);
        REQUIRE(back.image);
        CHECK(hasSection(back.info, MetaSectionId::Xmp));
        auto lost = metadataLossOnSave(w.info, back.info.format);
        CHECK(std::find(lost.begin(), lost.end(), L"XMP") == lost.end());
        DeleteFileW(dst.c_str());
    }
    // PNG：WIC 的編碼器裝不了 XMP。v0.9.5 沒搬也沒列；現在事前的警告會列出來，存檔不算「該帶的沒帶到」。
    auto png = tempPath(L"b04_out.png");
    SaveResult saved = saveImage(*w.image, png, fromWebp);
    REQUIRE(saved);
    CHECK_FALSE(saved.metadataDropped);
    auto back = loadImage(png);
    REQUIRE(back.image);
    CHECK_FALSE(hasSection(back.info, MetaSectionId::Xmp));
    auto lost = metadataLossOnSave(w.info, ImageFormat::Png);
    CHECK(std::find(lost.begin(), lost.end(), L"XMP") != lost.end());
    DeleteFileW(png.c_str());
    DeleteFileW(jpg.c_str());
    DeleteFileW(webp.c_str());
}

TEST_CASE("B-15：WebP 帶 orientation 5～8 時 readImageSize 的寬高和 loadImage 一致") {
    Com com;
    auto jpg = tempPath(L"b15.jpg"), webp = tempPath(L"b15.webp");
    REQUIRE(writeWithExif(jpg, GUID_ContainerFormatJpeg, 64, 32, false));
    SaveOptions fromJpg;
    fromJpg.metadataSource = jpg;
    for (int orientation : {1, 3, 6, 8}) {
        // 存成 64×32 的 WebP（EXIF 的 Orientation 存檔時歸 1），再把檔案裡的值改掉。
        REQUIRE(saveImage(solid(64, 32), webp, fromJpg));
        patchOrientationInFile(webp, static_cast<char>(orientation));
        auto w = loadImage(webp);
        REQUIRE(w.image);
        CHECK(w.info.orientation == orientation);
        const bool swapped = orientation >= 5;
        CHECK(w.image->width == (swapped ? 32 : 64));
        int sw = 0, sh = 0;
        REQUIRE(readImageSize(webp, sw, sh));
        CHECK(sw == w.image->width);
        CHECK(sh == w.image->height);
    }
    DeleteFileW(jpg.c_str());
    DeleteFileW(webp.c_str());
}

TEST_CASE("readImageSize：JPEG / TIFF 帶 orientation 6 時寬高對調，和 loadImage 一致") {
    Com com;
    for (auto [name, container] : {std::pair{L"rs6.jpg", GUID_ContainerFormatJpeg}, std::pair{L"rs6.tif", GUID_ContainerFormatTiff}}) {
        auto p = tempPath(name);
        REQUIRE(writeWithExif(p, container, 64, 32, false)); // Orientation = 6
        auto r = loadImage(p);
        REQUIRE(r.image);
        CHECK(r.info.orientation == 6);
        int w = 0, h = 0;
        REQUIRE(readImageSize(p, w, h));
        CHECK(w == 32);
        CHECK(h == 64);
        CHECK(w == r.image->width);
        CHECK(h == r.image->height);
        DeleteFileW(p.c_str());
    }
}

TEST_CASE("B-16：內容是 WebP、檔名是 .jpg 的檔，會掉什麼照內容的格式算") {
    Com com;
    auto jpg = tempPath(L"b16m.jpg"), webp = tempPath(L"b16m.webp"), fake = tempPath(L"b16m_fake.jpg");
    REQUIRE(writeWithExif(jpg, GUID_ContainerFormatJpeg, 64, 32, false, true));
    auto r = loadImage(jpg);
    REQUIRE(r.image);
    SaveOptions fromJpg;
    fromJpg.metadataSource = jpg;
    REQUIRE(saveImage(*r.image, webp, fromJpg));
    REQUIRE(MoveFileExW(webp.c_str(), fake.c_str(), MOVEFILE_REPLACE_EXISTING));
    auto f = loadImage(fake);
    REQUIRE(f.image);
    CHECK(f.info.format == ImageFormat::WebP);
    f.info.sections.push_back(sectionOf(MetaSectionId::Iptc)); // WebP 不會有，放一個進去看規則
    // 輸出成 .jpg：不是「同格式」，IPTC 以外都帶得過去；輸出成 .webp 才是同格式。
    CHECK(metadataLossOnSave(f.info, ImageFormat::WebP).empty());
    CHECK(metadataLossOnSave(f.info, ImageFormat::Bmp).size() >= 2);
    // metadata 來源也看內容：從這個「.jpg」存成真的 JPEG，Make 與 XMP 都在。
    auto out = tempPath(L"b16m_out.jpg");
    SaveOptions fromFake;
    fromFake.metadataSource = fake;
    REQUIRE(saveImage(*f.image, out, fromFake));
    auto back = loadImage(out);
    REQUIRE(back.image);
    CHECK(back.info.format == ImageFormat::Jpeg);
    CHECK(hasItem(back.info, L"TIFF", L"Make", L"TestCam"));
    CHECK(readStr(out, L"/xmp/dc:title") == L"xmp-title");
    for (auto& p : {jpg, fake, out}) DeleteFileW(p.c_str());
}

TEST_CASE("metadataCarried 說帶得過去的，存檔後真的在（Make 與 XMP，各種來源與目標）") {
    Com com;
    auto jpg = tempPath(L"rule_src.jpg"), tif = tempPath(L"rule_src.tif"), webp = tempPath(L"rule_src.webp"), png = tempPath(L"rule_src.png");
    REQUIRE(writeWithExif(jpg, GUID_ContainerFormatJpeg, 16, 8, false, true));
    REQUIRE(writeWithExif(tif, GUID_ContainerFormatTiff, 16, 8, false, true));
    {
        auto r = loadImage(jpg);
        REQUIRE(r.image);
        SaveOptions o;
        o.metadataSource = jpg;
        REQUIRE(saveImage(*r.image, webp, o));
        REQUIRE(saveImage(*r.image, png, o)); // PNG 來源：EXIF 與 XMP 都帶不過來，只有像素
    }
    for (const auto& src : {jpg, tif, webp, png}) {
        auto r = loadImage(src);
        REQUIRE(r.image);
        const bool srcHasExif = hasSection(r.info, MetaSectionId::Tiff), srcHasXmp = hasSection(r.info, MetaSectionId::Xmp);
        SaveOptions o;
        o.metadataSource = src;
        for (auto* name : {L"rule_out.jpg", L"rule_out.tif", L"rule_out.png", L"rule_out.webp", L"rule_out.bmp"}) {
            auto dst = tempPath(name);
            SaveResult saved = saveImage(*r.image, dst, o);
            REQUIRE(saved);
            CHECK_FALSE(saved.metadataDropped);
            auto back = loadImage(dst);
            REQUIRE(back.image);
            INFO("source " << static_cast<int>(r.info.format) << " -> target " << static_cast<int>(back.info.format));
            // 規則說帶得過去、來源也有的：一定要在。規則說帶不過去的：一定不在（否則警告會多報）。
            if (srcHasExif) CHECK(hasSection(back.info, MetaSectionId::Tiff) == metadataCarried(r.info.format, back.info.format, MetaSectionId::Tiff));
            if (srcHasXmp) CHECK(hasSection(back.info, MetaSectionId::Xmp) == metadataCarried(r.info.format, back.info.format, MetaSectionId::Xmp));
            DeleteFileW(dst.c_str());
        }
    }
    for (auto& p : {jpg, tif, webp, png}) DeleteFileW(p.c_str());
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
} // namespace

TEST_CASE("PNG 的 eXIf chunk：讀得到 EXIF 的話，會掉什麼的規則要和實際存出來的一致") {
    Com com;
    // 正常的 PNG，在 IHDR 後面插一個 eXIf chunk（內容是 tinyExif：Orientation 6、Make "Cam"）。
    auto png = tempPath(L"exif_src.png");
    REQUIRE(saveImage(solid(16, 8), png));
    std::string bytes = mp::test::readBytes(png);
    auto tiff = tinyExif();
    std::string data(tiff.begin(), tiff.end());
    std::string chunk = be32s(static_cast<uint32_t>(data.size())) + "eXIf" + data + be32s(crc32Of("eXIf" + data));
    const size_t afterIhdr = 8 + 4 + 4 + 13 + 4; // 簽章 + IHDR（長度、名稱、13 bytes、CRC）
    bytes.insert(afterIhdr, chunk);
    mp::test::writeBytes(png, bytes);

    auto r = loadImage(png);
    REQUIRE(r.image);
    const bool hasExif = hasSection(r.info, MetaSectionId::Tiff) || hasSection(r.info, MetaSectionId::Exif);
    WARN("WIC shows EXIF from a PNG eXIf chunk: " << hasExif << ", orientation " << r.info.orientation
                                                   << ", size " << r.image->width << "x" << r.image->height);
    int w = 0, h = 0;
    REQUIRE(readImageSize(png, w, h));
    CHECK(w == r.image->width); // 不管有沒有讀到 orientation，兩邊要一致
    CHECK(h == r.image->height);
    if (hasExif) {
        SaveOptions o;
        o.metadataSource = png;
        for (auto* name : {L"exif_out.jpg", L"exif_out.tif"}) {
            auto dst = tempPath(name);
            REQUIRE(saveImage(*r.image, dst, o));
            auto back = loadImage(dst);
            REQUIRE(back.image);
            CHECK(hasSection(back.info, MetaSectionId::Tiff) == metadataCarried(ImageFormat::Png, back.info.format, MetaSectionId::Tiff));
            DeleteFileW(dst.c_str());
        }
    }
    DeleteFileW(png.c_str());
}
