#include "core/ImageFormat.h"

#include <windows.h>

#include <algorithm>
#include <cstring>

#include "core/Lang.h"
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

ImageFormat sniffImageFormat(const unsigned char* b, size_t size) {
    if (!b) return ImageFormat::Unknown;
    if (size >= 8 && std::memcmp(b, "\x89PNG\r\n\x1a\n", 8) == 0) return ImageFormat::Png;
    if (size >= 3 && b[0] == 0xFF && b[1] == 0xD8 && b[2] == 0xFF) return ImageFormat::Jpeg;
    if (size >= 12 && std::memcmp(b, "RIFF", 4) == 0 && std::memcmp(b + 8, "WEBP", 4) == 0) return ImageFormat::WebP;
    if (size >= 4 && (std::memcmp(b, "II*\0", 4) == 0 || std::memcmp(b, "MM\0*", 4) == 0)) return ImageFormat::Tiff;
    if (size >= 2 && b[0] == 'B' && b[1] == 'M') return ImageFormat::Bmp;
    return ImageFormat::Unknown;
}

ImageFormat detectImageFormat(const std::wstring& path) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return ImageFormat::Unknown;
    unsigned char head[16] = {};
    DWORD got = 0;
    bool ok = ReadFile(h, head, sizeof(head), &got, nullptr);
    CloseHandle(h);
    return ok ? sniffImageFormat(head, got) : ImageFormat::Unknown;
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

bool isSupportedExtension(const std::wstring& ext) {
    return imageFormatOfExtension(ext) != ImageFormat::Unknown || pluginHandlesExtension(ext);
}

std::vector<std::wstring> supportedExtensions() {
    std::vector<std::wstring> v;
    for (const auto& f : imageFormats())
        for (const wchar_t* e : f.extensions) v.push_back(e);
    for (auto& e : pluginExtensions()) v.push_back(std::move(e));
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
