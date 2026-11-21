#include "core/IoError.h"

#include "core/Lang.h"

namespace mp {

const wchar_t* ioErrorMessage(IoErrorCode code) {
    switch (code) {
    case IoErrorCode::None: return L"";
    case IoErrorCode::FileUnreadable: return trKey(L"Could not read the file");
    case IoErrorCode::WicUnavailable: return trKey(L"Could not create the WIC factory");
    case IoErrorCode::DecodeFailed: return trKey(L"Could not decode this file");
    case IoErrorCode::NoFrame: return trKey(L"Could not read the first frame");
    case IoErrorCode::EmptyImage: return trKey(L"Image size is 0");
    case IoErrorCode::ConvertFailed: return trKey(L"Could not convert the pixel format");
    case IoErrorCode::ReadPixelsFailed: return trKey(L"Reading pixels failed");
    case IoErrorCode::InvalidWebP: return trKey(L"Not a valid WebP file");
    case IoErrorCode::WebPInitFailed: return trKey(L"libwebp initialization failed");
    case IoErrorCode::WebPDecodeFailed: return trKey(L"WebP decoding failed");
    case IoErrorCode::OutOfMemory: return trKey(L"The image is too large: not enough memory");
    case IoErrorCode::PluginFailed: return trKey(L"The plugin could not read this image: %s");
    case IoErrorCode::NeedHeifExtension: return trKey(L"Opening HEIC / HEIF images needs “HEIF Image Extension” from Microsoft Store");
    case IoErrorCode::NeedHevcExtension: return trKey(L"This image is HEVC-encoded and needs “HEVC Video Extensions” from Microsoft Store");
    case IoErrorCode::NeedAv1Extension: return trKey(L"This image is AV1-encoded and needs “AV1 Video Extension” from Microsoft Store");
    case IoErrorCode::UnsupportedSaveFormat: return trKey(L"Saving this format is not supported: %s");
    case IoErrorCode::WebPEncodeFailed: return trKey(L"WebP encoding failed");
    case IoErrorCode::WriteFailed: return trKey(L"Writing the file failed");
    case IoErrorCode::CreateFileFailed: return trKey(L"Could not create the file");
    case IoErrorCode::CreateEncoderFailed: return trKey(L"Could not create the encoder");
    case IoErrorCode::CreateFrameFailed: return trKey(L"Could not create the frame");
    case IoErrorCode::FrameSetupFailed: return trKey(L"Frame setup failed");
    case IoErrorCode::WriteImageFailed: return trKey(L"Writing the image failed");
    case IoErrorCode::ReplaceFailed: return trKey(L"Could not replace the original file");
    }
    return L"";
}

const wchar_t* ioErrorStoreUrl(IoErrorCode code) {
    // 產品識別碼 2026-11-22 從 Store 目錄（displaycatalog.mp.microsoft.com）查證：發行者都是 Microsoft Corporation。
    switch (code) {
    case IoErrorCode::NeedHeifExtension: return L"https://apps.microsoft.com/detail/9pmmsr1cgpwg";
    case IoErrorCode::NeedHevcExtension: return L"https://apps.microsoft.com/detail/9nmzlz57r3t7";
    case IoErrorCode::NeedAv1Extension: return L"https://apps.microsoft.com/detail/9mvzqvxjbq9v";
    default: return nullptr;
    }
}

} // namespace mp
