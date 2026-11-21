// HEIC / HEIF：檔頭辨識、缺 Microsoft Store 擴充時的錯誤代碼、經過 WIC 的讀檔與輸出成。
// 解不解得開看機器裝了什麼（HEIF 影像延伸模組、HEVC / AV1 視訊延伸模組），所以讀檔的測試接受幾種結果，
// 走了哪一條用 WARN 印出來；解得開時才檢查像素。HEVC 擴充的授權要互動登入的 session：從 SSH 跑測試時讀像素
// 會回 HRESULT_FROM_WIN32(ERROR_NO_SUCH_LOGON_SESSION)（2026-11-22 VM 實測），那也算一種結果。
#include <catch2/catch_test_macros.hpp>

#include <windows.h>
#include <mferror.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <cstdio>
#include <cstdlib>
#include <string>

#include "core/ImageFormat.h"
#include "core/ImageIO.h"
#include "core/IoError.h"
#include "test_util.h"

using namespace mp;
using namespace mp::test;
using Microsoft::WRL::ComPtr;

namespace {

const wchar_t kOrient6[] = L"heic-exif-orient6.heic"; // HEVC，64×48 存成 irot（顯示 48×64），EXIF 方向 6
const wchar_t kAvif[] = L"heic-avif.heic";            // 內容是 AVIF，64×48

std::string bytes(const char* s, size_t n) { return std::string(s, n); }

HeifCodec codecOf(const std::string& s) { return heifCodecOf(reinterpret_cast<const unsigned char*>(s.data()), s.size()); }
ImageFormat sniff(const std::string& s) { return sniffImageFormat(reinterpret_cast<const unsigned char*>(s.data()), s.size()); }

// ftyp box：主要 brand 與相容 brand，大小照實際長度寫。
std::string ftyp(const char* major, std::initializer_list<const char*> compat) {
    std::string s(4, '\0');
    s += "ftyp";
    s += major;
    s += std::string(4, '\0');
    for (const char* c : compat) s += c;
    const size_t n = s.size();
    s[0] = static_cast<char>((n >> 24) & 0xFF);
    s[1] = static_cast<char>((n >> 16) & 0xFF);
    s[2] = static_cast<char>((n >> 8) & 0xFF);
    s[3] = static_cast<char>(n & 0xFF);
    return s;
}

bool nearColor(const uint8_t* bgra, int r, int g, int b, int tol) {
    return std::abs(bgra[2] - r) <= tol && std::abs(bgra[1] - g) <= tol && std::abs(bgra[0] - b) <= tol;
}

std::string describe(const IoError& e) {
    char buf[64];
    snprintf(buf, sizeof(buf), "IoErrorCode %d, HRESULT 0x%08lX", static_cast<int>(e.code), static_cast<unsigned long>(e.hresult));
    return buf;
}

// 讀檔失敗時，可以接受的「機器少了東西」的結果。
bool missingPiece(const IoError& e, IoErrorCode codecMissing) {
    if (e.code == IoErrorCode::NeedHeifExtension || e.code == codecMissing) return true;
    return e.code == IoErrorCode::ReadPixelsFailed && e.hresult == HRESULT_FROM_WIN32(ERROR_NO_SUCH_LOGON_SESSION);
}

std::wstring readStr(const std::wstring& path, const wchar_t* key) {
    ComPtr<IWICImagingFactory> f;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&f)))) return L"";
    ComPtr<IWICBitmapDecoder> dec;
    if (FAILED(f->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &dec))) return L"";
    ComPtr<IWICBitmapFrameDecode> frame;
    if (FAILED(dec->GetFrame(0, &frame))) return L"";
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

struct Com {
    Com() { CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED); }
    ~Com() { CoUninitialize(); }
};

} // namespace

TEST_CASE("HEIF 的格式表：副檔名、寫不出來所以唯讀、不在存檔對話框") {
    for (auto* e : {L".heic", L".HEIF", L".hif"}) CHECK(imageFormatOfExtension(e) == ImageFormat::Heif);
    CHECK(imageFormatOfExtension(L".avif") == ImageFormat::Unknown); // .avif 不收
    CHECK(isSupportedExtension(L".HEIC"));
    CHECK_FALSE(canSaveExtension(L".heic"));
    CHECK(isViewOnlyFormat(ImageFormat::Heif));
    CHECK_FALSE(isViewOnlyFormat(ImageFormat::Png));
    CHECK_FALSE(isViewOnlyFormat(ImageFormat::Unknown)); // 表外的（GIF 這類）照舊可以編輯
    CHECK(imageFormatInfo(ImageFormat::Heif)->systemCodec);
    CHECK(std::wstring(imageFormatName(ImageFormat::Heif)) == L"HEIF image");
    for (ImageFormat f : saveDialogFormats()) CHECK(f != ImageFormat::Heif);
}

TEST_CASE("sniffImageFormat：ftyp 的主要 brand 是 HEIF 的才算，MP4 影片的不算") {
    for (const char* b : {"heic", "heix", "hevc", "mif1", "msf1", "miaf", "avif", "avis"})
        CHECK(sniff(ftyp(b, {})) == ImageFormat::Heif);
    for (const char* b : {"isom", "mp42", "qt  ", "M4V "}) CHECK(sniff(ftyp(b, {"heic"})) == ImageFormat::Unknown);
    CHECK(sniff(ftyp("heic", {}).substr(0, 11)) == ImageFormat::Unknown); // brand 不完整
    CHECK(sniff(bytes("\0\0\0\x18" "ftap" "heic", 12)) == ImageFormat::Unknown);
}

TEST_CASE("heifCodecOf：看主要與相容 brand；HEVC 優先；只看 ftyp box 裡、也只看拿到的部分") {
    CHECK(codecOf(ftyp("heic", {"mif1", "MiHE", "MiPr", "miaf"})) == HeifCodec::Hevc); // iPhone
    CHECK(codecOf(ftyp("mif1", {"heic", "miaf"})) == HeifCodec::Hevc);
    CHECK(codecOf(ftyp("avif", {"avif", "mif1", "miaf", "MA1B"})) == HeifCodec::Av1);
    CHECK(codecOf(ftyp("mif1", {"miaf", "avif"})) == HeifCodec::Av1);
    CHECK(codecOf(ftyp("mif1", {"avif", "heic"})) == HeifCodec::Hevc);
    CHECK(codecOf(ftyp("mif1", {"miaf"})) == HeifCodec::Unknown);
    // box 只到相容 brand 之前：後面的 "heic" 不算。
    std::string shortBox = ftyp("mif1", {}) + "heic";
    CHECK(codecOf(shortBox) == HeifCodec::Unknown);
    // 只拿到前 16 bytes：看得到主要 brand，看不到相容 brand。
    CHECK(codecOf(ftyp("mif1", {"heic"}).substr(0, 16)) == HeifCodec::Unknown);
    CHECK(codecOf(ftyp("heic", {}).substr(0, 12)) == HeifCodec::Hevc);
    CHECK(codecOf(bytes("\x89PNG\r\n\x1a\n", 8)) == HeifCodec::Unknown);
    CHECK(heifCodecOf(nullptr, 32) == HeifCodec::Unknown);
}

TEST_CASE("fixture：檔頭是 HEIF，編碼分得出 HEVC 與 AV1") {
    CHECK(detectImageFormat(fixturePath(kOrient6)) == ImageFormat::Heif);
    CHECK(detectHeifCodec(fixturePath(kOrient6)) == HeifCodec::Hevc);
    CHECK(detectImageFormat(fixturePath(kAvif)) == ImageFormat::Heif);
    CHECK(detectHeifCodec(fixturePath(kAvif)) == HeifCodec::Av1);
    CHECK(detectHeifCodec(tempPath(L"no-such-file.heic")) == HeifCodec::Unknown);
}

TEST_CASE("classifyHeifFailure：建不出解碼器是缺 HEIF 擴充，缺解碼器看 brand，其他不動") {
    using C = IoErrorCode;
    for (HeifCodec codec : {HeifCodec::Hevc, HeifCodec::Av1, HeifCodec::Unknown}) {
        CHECK(classifyHeifFailure(C::DecodeFailed, WINCODEC_ERR_COMPONENTINITIALIZEFAILURE, codec) == C::NeedHeifExtension);
        CHECK(classifyHeifFailure(C::DecodeFailed, WINCODEC_ERR_COMPONENTNOTFOUND, codec) == C::NeedHeifExtension);
    }
    CHECK(classifyHeifFailure(C::ReadPixelsFailed, MF_E_TOPO_CODEC_NOT_FOUND, HeifCodec::Hevc) == C::NeedHevcExtension);
    CHECK(classifyHeifFailure(C::ReadPixelsFailed, MF_E_TOPO_CODEC_NOT_FOUND, HeifCodec::Unknown) == C::NeedHevcExtension);
    CHECK(classifyHeifFailure(C::ReadPixelsFailed, MF_E_TOPO_CODEC_NOT_FOUND, HeifCodec::Av1) == C::NeedAv1Extension);
    CHECK(classifyHeifFailure(C::NoFrame, MF_E_TOPO_CODEC_NOT_FOUND, HeifCodec::Hevc) == C::NeedHevcExtension);
    // 實測過、不是缺擴充的：檔案損毀、10 位元、其他 WIC 錯誤。
    CHECK(classifyHeifFailure(C::ReadPixelsFailed, static_cast<HRESULT>(0xC00D36BEL), HeifCodec::Hevc) == C::ReadPixelsFailed);
    CHECK(classifyHeifFailure(C::ReadPixelsFailed, E_FAIL, HeifCodec::Hevc) == C::ReadPixelsFailed);
    CHECK(classifyHeifFailure(C::DecodeFailed, WINCODEC_ERR_BADHEADER, HeifCodec::Hevc) == C::DecodeFailed);
    CHECK(classifyHeifFailure(C::OutOfMemory, 0, HeifCodec::Hevc) == C::OutOfMemory);
    CHECK(classifyHeifFailure(C::None, MF_E_TOPO_CODEC_NOT_FOUND, HeifCodec::Hevc) == C::None);
}

TEST_CASE("ioErrorStoreUrl：缺擴充的三個代碼各指一個 Store 網頁，其他沒有") {
    CHECK(std::wstring(ioErrorStoreUrl(IoErrorCode::NeedHeifExtension)) == L"https://apps.microsoft.com/detail/9pmmsr1cgpwg");
    CHECK(std::wstring(ioErrorStoreUrl(IoErrorCode::NeedHevcExtension)) == L"https://apps.microsoft.com/detail/9nmzlz57r3t7");
    CHECK(std::wstring(ioErrorStoreUrl(IoErrorCode::NeedAv1Extension)) == L"https://apps.microsoft.com/detail/9mvzqvxjbq9v");
    for (IoErrorCode c : {IoErrorCode::None, IoErrorCode::DecodeFailed, IoErrorCode::ReadPixelsFailed, IoErrorCode::PluginFailed})
        CHECK(ioErrorStoreUrl(c) == nullptr);
}

TEST_CASE("loadImage(HEIC)：解得開時寬高是轉過的、方向只轉一次；解不開時說缺哪個擴充") {
    LoadResult r = loadImage(fixturePath(kOrient6));
    int w = 0, h = 0;
    const bool sized = readImageSize(fixturePath(kOrient6), w, h);
    if (!r.image) {
        WARN("HEIC 解不開：" << describe(r.error));
        CHECK(missingPiece(r.error, IoErrorCode::NeedHevcExtension));
        // 有 HEIF 擴充（只是缺 HEVC）時寬高照樣讀得到：兩頁並排與預先解碼不受影響。
        CHECK(sized == (r.error.code != IoErrorCode::NeedHeifExtension));
        if (sized) {
            CHECK(w == 48);
            CHECK(h == 64);
        }
        return;
    }
    WARN("HEIC 解得開");
    CHECK(r.info.format == ImageFormat::Heif);
    CHECK(r.info.orientation == 1); // WIC 自己套 irot，EXIF 的方向欄位拿掉了
    REQUIRE(r.image->width == 48);
    REQUIRE(r.image->height == 64);
    CHECK(nearColor(r.image->px(44, 3), 255, 0, 0, 40)); // 左上角的紅塊轉到右上角
    CHECK(nearColor(r.image->px(3, 3), 128, 128, 128, 30));
    CHECK(nearColor(r.image->px(3, 60), 128, 128, 128, 30));
    CHECK(sized);
    CHECK(w == 48);
    CHECK(h == 64);
}

TEST_CASE("loadImage(.heic 裡是 AVIF)：解得開時照原樣；缺 AV1 擴充時說 AV1") {
    LoadResult r = loadImage(fixturePath(kAvif));
    if (!r.image) {
        WARN("AVIF 內容的 .heic 解不開：" << describe(r.error));
        CHECK(missingPiece(r.error, IoErrorCode::NeedAv1Extension));
        return;
    }
    WARN("AVIF 內容的 .heic 解得開");
    CHECK(r.info.format == ImageFormat::Heif);
    REQUIRE(r.image->width == 64);
    REQUIRE(r.image->height == 48);
    CHECK(nearColor(r.image->px(3, 3), 255, 0, 0, 40));
    CHECK(nearColor(r.image->px(60, 44), 128, 128, 128, 30));
}

TEST_CASE("輸出成 JPEG：EXIF 從 HEIC 搬過去，方向欄位不帶（WIC 已經拿掉）") {
    int w = 0, h = 0;
    if (!readImageSize(fixturePath(kOrient6), w, h)) SKIP("這台沒有 HEIF 影像延伸模組，讀不到 HEIC 的 metadata");
    Com com;
    const std::wstring out = tempPath(L"from-heic.jpg");
    SaveOptions opt;
    opt.metadataSource = fixturePath(kOrient6);
    SaveResult s = saveImage(solidImage(48, 64, 30, 120, 200), out, opt); // 像素不必從 HEIC 解，不需要 HEVC
    REQUIRE(s.ok());
    CHECK_FALSE(s.metadataDropped);
    CHECK(readStr(out, L"/app1/ifd/{ushort=271}") == L"mpainter test");
    CHECK(readStr(out, L"/app1/ifd/{ushort=272}") == L"heic fixture");
    LoadResult back = loadImage(out);
    REQUIRE(back.image);
    CHECK(back.info.orientation == 1);
    CHECK(back.image->width == 48);
}
