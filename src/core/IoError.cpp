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
    case IoErrorCode::UnsupportedSaveFormat: return trKey(L"Saving this format is not supported: %s");
    case IoErrorCode::WebPEncodeFailed: return trKey(L"WebP encoding failed");
    case IoErrorCode::WriteFailed: return trKey(L"Writing the file failed");
    case IoErrorCode::CreateFileFailed: return trKey(L"Could not create the file");
    case IoErrorCode::CreateEncoderFailed: return trKey(L"Could not create the encoder");
    case IoErrorCode::CreateFrameFailed: return trKey(L"Could not create the frame");
    case IoErrorCode::FrameSetupFailed: return trKey(L"Frame setup failed");
    case IoErrorCode::CreateBitmapFailed: return trKey(L"Could not create the source bitmap");
    case IoErrorCode::WriteImageFailed: return trKey(L"Writing the image failed");
    case IoErrorCode::ReplaceFailed: return trKey(L"Could not replace the original file");
    }
    return L"";
}

} // namespace mp
