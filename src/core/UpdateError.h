#pragma once
#include <string>

#include "core/Archive.h"

namespace mp {

// 新版本檢查與自動更新的錯誤：連線、下載、核對、解開、換檔都回這一種。
// core 只給代碼與數字，畫面上的句子由 app/UiText 的 updateErrorText 組。
enum class UpdateErrorCode {
    None,
    Cancelled,           // 使用者取消（不是錯誤，不必顯示）
    // 連線
    BadUrl,              // 網址解析不了；detail 是那個網址
    NetworkUnavailable,  // WinHTTP 開不起來
    ConnectFailed,       // 連不上、送不出去、等不到回應
    HttpStatus,          // 伺服器回的不是 200；httpStatus 是狀態碼
    DownloadInterrupted, // 讀到一半斷了
    ResponseTooLarge,    // 回應比預期大得多（查版本的 API 不該回這麼多）
    // 檔案
    CreateFileFailed,    // 建不出下載的檔
    WriteFileFailed,     // 寫不進去
    // 內容
    BadResponse,         // 回應不是預期的 JSON，或標籤不是版本號
    ChecksumMismatch,    // 下載的檔和 .sha256 對不上
    UnpackFailed,        // 發佈包解不開；archive 是原因
    NoExeInPackage,      // 發佈包裡沒有 mpainter.exe
    // 換檔
    FolderInTheWay,      // 要放檔案的位置是一個資料夾；detail 是那個路徑
    RenameFailed,        // 舊檔改不了名；detail 是那個路徑
    CopyFailed,          // 新檔放不進去；detail 是那個路徑
    FolderNotWritable,   // 資料夾不能寫；detail 是那個資料夾
};

struct UpdateError {
    UpdateErrorCode code = UpdateErrorCode::None;
    int httpStatus = 0;            // HttpStatus
    unsigned long systemError = 0; // Windows / WinHTTP 的錯誤碼；沒有是 0
    std::wstring detail;           // 路徑或網址（不是訊息）
    ArchiveError archive;          // UnpackFailed
    bool ok() const { return code == UpdateErrorCode::None; }
    bool cancelled() const { return code == UpdateErrorCode::Cancelled; }
};

// code 的英文說明，也是翻譯 key。帶 %s / %d 的要填 detail / httpStatus（見 updateErrorText）。None、Cancelled 回空字串。
const wchar_t* updateErrorMessage(UpdateErrorCode code);

} // namespace mp
