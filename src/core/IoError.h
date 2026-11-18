#pragma once
#include <string>

namespace mp {

// 讀寫圖檔失敗的原因。core 只回代碼，要顯示的句子由 app 翻譯（ioErrorMessage 給英文翻譯 key）。
enum class IoErrorCode {
    None,
    // 讀檔
    FileUnreadable,   // 檔案開不了或讀不完
    WicUnavailable,   // 建不出 WIC factory
    DecodeFailed,     // 不認得或解不開
    NoFrame,          // 讀不到第一格
    EmptyImage,       // 寬或高是 0
    ConvertFailed,    // 像素格式轉不過來
    ReadPixelsFailed,
    InvalidWebP,
    WebPInitFailed,
    WebPDecodeFailed,
    OutOfMemory,      // 圖太大，記憶體配不出來
    PluginFailed,     // 讀圖 plugin 回報失敗。detail 是英文（plugin id、回傳值、plugin 的說明），不翻譯
    // 存檔
    UnsupportedSaveFormat, // detail 是副檔名
    WebPEncodeFailed,
    WriteFailed,           // 寫暫存檔失敗
    CreateFileFailed,
    CreateEncoderFailed,
    CreateFrameFailed,
    FrameSetupFailed,
    WriteImageFailed,
    ReplaceFailed,         // 暫存檔寫好了，取代原檔失敗
};

struct IoError {
    IoErrorCode code = IoErrorCode::None;
    long hresult = 0;            // 出錯的 COM 呼叫回的 HRESULT；沒有就是 0
    unsigned long systemError = 0; // 出錯的 Win32 呼叫的 GetLastError()；沒有就是 0
    std::wstring detail;         // 代碼需要的補充（見各代碼的註解）；多半是空的

    bool ok() const { return code == IoErrorCode::None; }
};

// 這個代碼的說明句（英文翻譯 key）。帶 %s 的是 UnsupportedSaveFormat 與 PluginFailed（填 detail）。None 回空字串。
const wchar_t* ioErrorMessage(IoErrorCode code);

} // namespace mp
