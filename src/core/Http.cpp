#include "core/Http.h"

#include <windows.h>
#include <winhttp.h>

namespace mp {

namespace {

// 逾時（毫秒）：解析名稱、連線、送出、等回應。
constexpr int kResolveTimeoutMs = 10000, kConnectTimeoutMs = 10000, kSendTimeoutMs = 15000, kReceiveTimeoutMs = 30000;
// 文字回應的上限。
constexpr size_t kMaxTextBytes = 4u << 20;
// 一次讀多少。
constexpr size_t kReadChunk = 64 * 1024;

struct WinHttpHandle {
    HINTERNET h = nullptr;
    explicit WinHttpHandle(HINTERNET x) : h(x) {}
    ~WinHttpHandle() { if (h) WinHttpCloseHandle(h); }
    WinHttpHandle(const WinHttpHandle&) = delete;
    WinHttpHandle& operator=(const WinHttpHandle&) = delete;
};

UpdateError failure(UpdateErrorCode code, unsigned long systemError = 0) {
    UpdateError e;
    e.code = code;
    e.systemError = systemError;
    return e;
}

} // namespace

// 要求進行的期間把它登記在 HttpCancel 上。cancel 會把登記的要求關掉；這邊結束時看是誰關的，免得關兩次。
class HttpCancelScope {
public:
    HttpCancelScope(HttpCancel* cancel, WinHttpHandle& request) : cancel_(cancel), request_(request) {
        if (!cancel_) return;
        std::lock_guard<std::mutex> lk(cancel_->m_);
        if (cancel_->cancelled_) alreadyCancelled_ = true;
        else cancel_->request_ = request_.h;
    }
    ~HttpCancelScope() {
        if (!cancel_) return;
        std::lock_guard<std::mutex> lk(cancel_->m_);
        if (cancel_->request_ == request_.h) cancel_->request_ = nullptr; // 沒被取消：照常由 WinHttpHandle 關
        else if (!alreadyCancelled_) request_.h = nullptr;               // cancel 已經關掉了：這邊不要再關
    }
    bool cancelled() const { return cancel_ && cancel_->cancelled(); }

private:
    HttpCancel* cancel_;
    WinHttpHandle& request_;
    bool alreadyCancelled_ = false;
};

void HttpCancel::cancel() {
    std::lock_guard<std::mutex> lk(m_);
    cancelled_ = true;
    if (request_) {
        // 關掉要求的 handle 會讓卡在同步呼叫裡的那條執行緒帶著錯誤回來（WinHttpCloseHandle 的文件寫明可以這樣中斷）。
        WinHttpCloseHandle(static_cast<HINTERNET>(request_));
        request_ = nullptr;
    }
}

bool HttpCancel::cancelled() const {
    std::lock_guard<std::mutex> lk(m_);
    return cancelled_;
}

namespace {

// GET url，收到的資料交給 sink（回 false 中止，這時回傳的錯誤由 sink 自己決定，這裡回 Cancelled）。
// total 是 Content-Length（不知道時 0）。
UpdateError httpGet(const std::wstring& url, const std::function<bool(const char*, size_t, uint64_t total)>& sink, HttpCancel* cancel) {
    URL_COMPONENTS uc{sizeof(uc)};
    wchar_t host[256] = L"", path[4096] = L"", extra[4096] = L"";
    uc.lpszHostName = host;
    uc.dwHostNameLength = 256;
    uc.lpszUrlPath = path;
    uc.dwUrlPathLength = 4096;
    uc.lpszExtraInfo = extra;
    uc.dwExtraInfoLength = 4096;
    if (!WinHttpCrackUrl(url.c_str(), 0, 0, &uc)) {
        UpdateError e = failure(UpdateErrorCode::BadUrl, GetLastError());
        e.detail = url;
        return e;
    }
    const bool https = uc.nScheme == INTERNET_SCHEME_HTTPS;

    WinHttpHandle session(WinHttpOpen(L"mpainter-updater", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                                      WINHTTP_NO_PROXY_BYPASS, 0));
    if (!session.h) return failure(UpdateErrorCode::NetworkUnavailable, GetLastError());
    WinHttpSetTimeouts(session.h, kResolveTimeoutMs, kConnectTimeoutMs, kSendTimeoutMs, kReceiveTimeoutMs);
    WinHttpHandle conn(WinHttpConnect(session.h, host, uc.nPort, 0));
    if (!conn.h) return failure(UpdateErrorCode::ConnectFailed, GetLastError());
    const std::wstring object = std::wstring(path) + extra;
    WinHttpHandle req(WinHttpOpenRequest(conn.h, L"GET", object.c_str(), nullptr, WINHTTP_NO_REFERER,
                                         WINHTTP_DEFAULT_ACCEPT_TYPES, https ? WINHTTP_FLAG_SECURE : 0));
    if (!req.h) return failure(UpdateErrorCode::ConnectFailed, GetLastError());

    HttpCancelScope scope(cancel, req);
    if (scope.cancelled()) return failure(UpdateErrorCode::Cancelled);
    // 從這裡起每個失敗都先看是不是被取消的：cancel 關掉 handle 之後，卡住的呼叫回來的是「handle 無效」之類的錯誤。
    auto failed = [&](UpdateErrorCode code) {
        const unsigned long err = GetLastError();
        return scope.cancelled() ? failure(UpdateErrorCode::Cancelled) : failure(code, err);
    };
    if (!WinHttpSendRequest(req.h, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
        !WinHttpReceiveResponse(req.h, nullptr))
        return failed(UpdateErrorCode::ConnectFailed);
    DWORD status = 0, len = sizeof(status);
    if (!WinHttpQueryHeaders(req.h, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &len,
                             WINHTTP_NO_HEADER_INDEX))
        return failed(UpdateErrorCode::ConnectFailed);
    if (status != HTTP_STATUS_OK) {
        UpdateError e = failure(UpdateErrorCode::HttpStatus);
        e.httpStatus = static_cast<int>(status);
        return e;
    }
    uint64_t total = 0;
    wchar_t clen[32] = L"";
    len = sizeof(clen);
    if (WinHttpQueryHeaders(req.h, WINHTTP_QUERY_CONTENT_LENGTH, WINHTTP_HEADER_NAME_BY_INDEX, clen, &len, WINHTTP_NO_HEADER_INDEX))
        total = _wcstoui64(clen, nullptr, 10);

    std::string buf(kReadChunk, '\0');
    for (;;) {
        DWORD got = 0;
        if (!WinHttpReadData(req.h, buf.data(), static_cast<DWORD>(buf.size()), &got)) return failed(UpdateErrorCode::DownloadInterrupted);
        if (got == 0) break;
        if (!sink(buf.data(), got, total)) return failure(UpdateErrorCode::Cancelled);
    }
    return {};
}

} // namespace

UpdateError httpGetText(const std::wstring& url, std::string& body, HttpCancel* cancel) {
    body.clear();
    bool tooLarge = false;
    UpdateError e = httpGet(url, [&](const char* p, size_t n, uint64_t) {
        if (body.size() + n > kMaxTextBytes) {
            tooLarge = true;
            return false;
        }
        body.append(p, n);
        return true;
    }, cancel);
    if (tooLarge) e = failure(UpdateErrorCode::ResponseTooLarge);
    if (!e.ok()) body.clear();
    return e;
}

UpdateError httpDownload(const std::wstring& url, const std::wstring& path,
                         const std::function<bool(uint64_t, uint64_t)>& progress, HttpCancel* cancel) {
    HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) {
        UpdateError e = failure(UpdateErrorCode::CreateFileFailed, GetLastError());
        e.detail = path;
        return e;
    }
    uint64_t done = 0;
    unsigned long writeError = 0;
    bool writeFailed = false;
    UpdateError e = httpGet(url, [&](const char* p, size_t n, uint64_t total) {
        DWORD w = 0;
        if (!WriteFile(f, p, static_cast<DWORD>(n), &w, nullptr) || w != n) {
            writeError = GetLastError();
            writeFailed = true;
            return false;
        }
        done += n;
        return !progress || progress(done, total);
    }, cancel);
    CloseHandle(f);
    if (writeFailed) {
        e = failure(UpdateErrorCode::WriteFileFailed, writeError);
        e.detail = path;
    }
    if (!e.ok()) DeleteFileW(path.c_str());
    return e;
}

} // namespace mp
