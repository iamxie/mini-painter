#include "core/ImageFormat.h"

#include <windows.h>

#include <algorithm>
#include <cstring>

#include "core/Lang.h"
#include "core/PathUtil.h"
#include "core/Plugins.h"
#include "core/TextUtil.h"

namespace mp {

const std::vector<ImageFormatInfo>& imageFormats() {
    // 順序就是 supportedExtensions() 的順序。
    static const std::vector<ImageFormatInfo> table = {
        {ImageFormat::Bmp, trKey(L"BMP image"), L"BMP", {L".bmp", L".dib"}, true, false},
        {ImageFormat::Tiff, trKey(L"TIFF image"), L"TIFF", {L".tif", L".tiff"}, true, true},
        {ImageFormat::Jpeg, trKey(L"JPEG image"), L"JPEG", {L".jpg", L".jpeg", L".jpe", L".jfif"}, true, false},
        {ImageFormat::Png, trKey(L"PNG image"), L"PNG", {L".png"}, true, true},
        {ImageFormat::WebP, trKey(L"WebP image"), L"WebP", {L".webp"}, true, true},
        // HEIC / HEIF：Windows 的 WIC 裝了 Microsoft Store 的擴充才解得開（HEIF 影像延伸模組，加上 HEVC 或 AV1
        // 視訊延伸模組），我們不附解碼器（HEVC 專利，見 history.md 2026-11-18）。.hif 是 Canon、Fujifilm 相機的。
        {ImageFormat::Heif, trKey(L"HEIF image"), L"HEIF", {L".heic", L".heif", L".hif"}, false, false, true},
    };
    return table;
}

const ImageFormatInfo* imageFormatInfo(ImageFormat format) {
    for (const auto& f : imageFormats())
        if (f.format == format) return &f;
    return nullptr;
}

const wchar_t* imageFormatName(ImageFormat format) {
    const ImageFormatInfo* f = imageFormatInfo(format);
    return f ? f->name : trKey(L"Image");
}

ImageFormat imageFormatOfExtension(const std::wstring& ext) {
    std::wstring lower = lowerCopy(ext);
    for (const auto& f : imageFormats())
        for (const wchar_t* e : f.extensions)
            if (lower == e) return f.format;
    return ImageFormat::Unknown;
}

namespace {

// HEIF 的 brand（ISO/IEC 23008-12 與 AVIF 規格）。
constexpr const char* kHevcBrands[] = {"heic", "heix", "heim", "heis", "hevc", "hevx", "hevm", "hevs"};
constexpr const char* kAv1Brands[] = {"avif", "avis"};
// 只說是 HEIF 容器、不說編碼的；AVC（avci、avcs）的 Windows 內建解碼器就有，不必分。
constexpr const char* kOtherHeifBrands[] = {"mif1", "msf1", "mif2", "miaf", "avci", "avcs"};

template <size_t N>
bool isBrandIn(const unsigned char* brand, const char* const (&list)[N]) {
    for (const char* b : list)
        if (std::memcmp(brand, b, 4) == 0) return true;
    return false;
}

bool isHeifBrand(const unsigned char* brand) {
    return isBrandIn(brand, kHevcBrands) || isBrandIn(brand, kAv1Brands) || isBrandIn(brand, kOtherHeifBrands);
}

// ISO BMFF 的 ftyp box：4 bytes 大小（big-endian）、"ftyp"、主要 brand、4 bytes 版本、相容 brand 一個 4 bytes。
bool isFtyp(const unsigned char* b, size_t size) { return size >= 12 && std::memcmp(b + 4, "ftyp", 4) == 0; }

// 讀檔案開頭最多 n bytes。
std::vector<unsigned char> readHead(const std::wstring& path, DWORD n) {
    std::vector<unsigned char> head;
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return head;
    head.resize(n);
    DWORD got = 0;
    if (!ReadFile(h, head.data(), n, &got, nullptr)) got = 0;
    CloseHandle(h);
    head.resize(got);
    return head;
}

} // namespace

ImageFormat sniffImageFormat(const unsigned char* b, size_t size) {
    if (!b) return ImageFormat::Unknown;
    if (size >= 8 && std::memcmp(b, "\x89PNG\r\n\x1a\n", 8) == 0) return ImageFormat::Png;
    if (size >= 3 && b[0] == 0xFF && b[1] == 0xD8 && b[2] == 0xFF) return ImageFormat::Jpeg;
    if (size >= 12 && std::memcmp(b, "RIFF", 4) == 0 && std::memcmp(b + 8, "WEBP", 4) == 0) return ImageFormat::WebP;
    if (size >= 4 && (std::memcmp(b, "II*\0", 4) == 0 || std::memcmp(b, "MM\0*", 4) == 0)) return ImageFormat::Tiff;
    if (size >= 2 && b[0] == 'B' && b[1] == 'M') return ImageFormat::Bmp;
    // 只看主要 brand（16 bytes 裡看得到的）。MP4 影片的 brand（isom、mp42…）不算。
    if (isFtyp(b, size) && isHeifBrand(b + 8)) return ImageFormat::Heif;
    return ImageFormat::Unknown;
}

ImageFormat detectImageFormat(const std::wstring& path) {
    const std::vector<unsigned char> head = readHead(path, 16);
    return sniffImageFormat(head.data(), head.size());
}

bool isViewOnlyFormat(ImageFormat format) {
    const ImageFormatInfo* f = imageFormatInfo(format);
    return f && !f->canSave;
}

HeifCodec heifCodecOf(const unsigned char* b, size_t size) {
    if (!b || sniffImageFormat(b, size) != ImageFormat::Heif) return HeifCodec::Unknown;
    const size_t boxSize = (size_t{b[0]} << 24) | (size_t{b[1]} << 16) | (size_t{b[2]} << 8) | b[3];
    const size_t end = std::min(boxSize, size);
    // 主要 brand 在 8，相容 brand 從 16 開始。HEVC 的 brand 優先：iPhone 的檔主要 brand 是 heic、相容 brand 有 mif1、miaf。
    bool av1 = false;
    for (size_t at = 8; at + 4 <= end; at += (at == 8 ? 8 : 4)) {
        if (isBrandIn(b + at, kHevcBrands)) return HeifCodec::Hevc;
        if (isBrandIn(b + at, kAv1Brands)) av1 = true;
    }
    return av1 ? HeifCodec::Av1 : HeifCodec::Unknown;
}

HeifCodec detectHeifCodec(const std::wstring& path) {
    const std::vector<unsigned char> head = readHead(path, 256);
    return heifCodecOf(head.data(), head.size());
}

std::wstring imagePatterns(ImageFormat format) {
    std::wstring out;
    if (const ImageFormatInfo* f = imageFormatInfo(format))
        for (const wchar_t* e : f->extensions) out += (out.empty() ? L"*" : L";*") + std::wstring(e);
    return out;
}

std::wstring allImagePatterns() {
    std::wstring out;
    for (const auto& e : supportedExtensions()) out += (out.empty() ? L"*" : L";*") + e;
    return out;
}

const std::vector<ImageFormat>& saveDialogFormats() {
    static const std::vector<ImageFormat> order = [] {
        // 使用者習慣的順序放前面；表裡其他寫得出來的格式接在後面，新增格式不會漏。
        std::vector<ImageFormat> v;
        auto add = [&](ImageFormat f) {
            const ImageFormatInfo* info = imageFormatInfo(f);
            if (info && info->canSave && std::find(v.begin(), v.end(), f) == v.end()) v.push_back(f);
        };
        for (ImageFormat f : {ImageFormat::Png, ImageFormat::Jpeg, ImageFormat::Bmp, ImageFormat::Tiff, ImageFormat::WebP}) add(f);
        for (const auto& info : imageFormats()) add(info.format);
        return v;
    }();
    return order;
}

std::wstring saveDialogFilter() {
    std::wstring out;
    for (ImageFormat f : saveDialogFormats()) {
        const ImageFormatInfo* info = imageFormatInfo(f);
        out += std::wstring(info->shortName) + L" (*" + info->extensions.front() + L")";
        out += L'\0';
        out += imagePatterns(f);
        out += L'\0';
    }
    return out;
}

std::wstring saveDialogDefaultName(const std::wstring& path, ImageFormat fallback) {
    const ImageFormatInfo* f = imageFormatInfo(fallback);
    const std::wstring ext = extensionOf(path);
    if (path.empty() || !f || canSaveExtension(ext)) return path;
    return path.substr(0, path.size() - ext.size()) + f->extensions.front();
}

bool isSupportedExtension(const std::wstring& ext) {
    return imageFormatOfExtension(ext) != ImageFormat::Unknown || pluginHandlesExtension(ext);
}

std::vector<std::wstring> supportedExtensions() {
    std::vector<std::wstring> v;
    for (const auto& f : imageFormats())
        for (const wchar_t* e : f.extensions) v.push_back(e);
    // plugin 可以接手 systemCodec 格式的副檔名（例如 .heic），那種已經在表裡，不重複列。
    for (auto& e : pluginExtensions())
        if (std::find(v.begin(), v.end(), e) == v.end()) v.push_back(std::move(e));
    return v;
}

bool canSaveExtension(const std::wstring& ext) {
    const ImageFormatInfo* f = imageFormatInfo(imageFormatOfExtension(ext));
    return f && f->canSave;
}

bool extensionKeepsTransparency(const std::wstring& ext) {
    const ImageFormatInfo* f = imageFormatInfo(imageFormatOfExtension(ext));
    return f && f->keepsTransparency;
}

} // namespace mp
