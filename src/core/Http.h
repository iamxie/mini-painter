#pragma once
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>

#include "core/UpdateError.h"

namespace mp {

// HTTP(S) GET（WinHTTP，同步，會跟著轉址）。給新版本檢查與下載發佈包用。

// 讓別的執行緒中斷進行中的要求（使用者在進度框按了取消）。
// 同步的 WinHTTP 呼叫卡在等伺服器回應時，沒辦法從裡面檢查旗標；cancel 直接把那個要求關掉，卡住的呼叫就會回來。
// 一個 HttpCancel 可以連續給好幾個要求用（下載 zip、再抓 .sha256）；取消之後再開始的要求一開始就回 Cancelled。
class HttpCancel {
public:
    // 任何執行緒，不等。
    void cancel();
    bool cancelled() const;

private:
    friend class HttpCancelScope;
    mutable std::mutex m_;
    bool cancelled_ = false;
    void* request_ = nullptr; // 進行中的要求（HINTERNET）；沒有是 nullptr
};

// 抓一段文字（查版本的 JSON、.sha256）。非 200 算失敗。回應超過 4 MB 算失敗（擋住意外指到大檔）。
UpdateError httpGetText(const std::wstring& url, std::string& body, HttpCancel* cancel = nullptr);
// 下載到檔案。progress(done, total) 回 false 取消（total 不知道時是 0）。失敗或取消時不留檔。
UpdateError httpDownload(const std::wstring& url, const std::wstring& path,
                         const std::function<bool(uint64_t done, uint64_t total)>& progress, HttpCancel* cancel = nullptr);

} // namespace mp
