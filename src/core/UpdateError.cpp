#include "core/UpdateError.h"

#include "core/Lang.h"

namespace mp {

const wchar_t* updateErrorMessage(UpdateErrorCode code) {
    switch (code) {
    case UpdateErrorCode::None:
    case UpdateErrorCode::Cancelled: return L"";
    case UpdateErrorCode::BadUrl: return trKey(L"The address is not valid: %s");
    case UpdateErrorCode::NetworkUnavailable: return trKey(L"The network is not available");
    case UpdateErrorCode::ConnectFailed: return trKey(L"Could not connect to the server");
    case UpdateErrorCode::HttpStatus: return trKey(L"The server answered HTTP %d");
    case UpdateErrorCode::DownloadInterrupted: return trKey(L"The download was interrupted");
    case UpdateErrorCode::ResponseTooLarge: return trKey(L"The response is too large");
    case UpdateErrorCode::CreateFileFailed: return trKey(L"Could not create the download file");
    case UpdateErrorCode::WriteFileFailed: return trKey(L"Could not write the download file");
    case UpdateErrorCode::BadResponse: return trKey(L"Unexpected response from the update server.");
    case UpdateErrorCode::ChecksumMismatch: return trKey(L"The downloaded file is damaged (checksum mismatch). Nothing was changed.");
    case UpdateErrorCode::UnpackFailed: return trKey(L"Could not unpack the update: %s");
    case UpdateErrorCode::NoExeInPackage: return trKey(L"The update package does not contain mpainter.exe. Nothing was changed.");
    case UpdateErrorCode::FolderInTheWay: return trKey(L"A folder is in the way: %s");
    case UpdateErrorCode::RenameFailed: return trKey(L"Could not rename %s");
    case UpdateErrorCode::CopyFailed: return trKey(L"Could not write %s");
    case UpdateErrorCode::FolderNotWritable: return trKey(L"mpainter cannot write to its folder:\n%s");
    }
    return L"";
}

} // namespace mp
