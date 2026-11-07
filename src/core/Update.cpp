#include "core/Update.h"

#include <windows.h>
#include <bcrypt.h>
#include <winhttp.h>

#include <cctype>
#include <cwctype>

#include <nlohmann/json.hpp>

namespace mp {

const wchar_t* const kDefaultUpdateUrl = L"https://api.github.com/repos/iamxie/mini-painter/releases/latest";

namespace {

std::wstring utf8ToWide(const std::string& s) {
    if (s.empty()) return L"";
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring out(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), n);
    return out;
}

std::wstring joinPath(const std::wstring& a, const std::wstring& b) {
    if (a.empty()) return b;
    if (a.back() == L'\\' || a.back() == L'/') return a + b;
    return a + L"\\" + b;
}

bool fileExists(const std::wstring& p) {
    DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

bool parseInt(const std::wstring& s, int& out) {
    if (s.empty() || s.size() > 6) return false;
    int v = 0;
    for (wchar_t c : s) {
        if (c < L'0' || c > L'9') return false;
        v = v * 10 + (c - L'0');
    }
    out = v;
    return true;
}

struct WinHttpHandle {
    HINTERNET h = nullptr;
    explicit WinHttpHandle(HINTERNET x) : h(x) {}
    ~WinHttpHandle() { if (h) WinHttpCloseHandle(h); }
    WinHttpHandle(const WinHttpHandle&) = delete;
    WinHttpHandle& operator=(const WinHttpHandle&) = delete;
};

std::wstring netError(const wchar_t* what) {
    return std::wstring(what) + L" (error " + std::to_wstring(GetLastError()) + L")";
}

// GET url，收到的資料交給 sink（回 false 中止）。total 是 Content-Length（不知道時 0）。
// 回 true = 200 且讀完；cancelled = sink 喊停。
bool httpGet(const std::wstring& url, const std::function<bool(const char*, size_t, uint64_t total)>& sink,
             std::wstring& error, bool& cancelled) {
    cancelled = false;
    URL_COMPONENTS uc{sizeof(uc)};
    wchar_t host[256] = L"", path[4096] = L"";
    uc.lpszHostName = host;
    uc.dwHostNameLength = 256;
    uc.lpszUrlPath = path;
    uc.dwUrlPathLength = 4096;
    wchar_t extra[4096] = L"";
    uc.lpszExtraInfo = extra;
    uc.dwExtraInfoLength = 4096;
    if (!WinHttpCrackUrl(url.c_str(), 0, 0, &uc)) { error = L"Bad URL: " + url; return false; }
    bool https = uc.nScheme == INTERNET_SCHEME_HTTPS;

    WinHttpHandle session(WinHttpOpen(L"mpainter-updater", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                                      WINHTTP_NO_PROXY_BYPASS, 0));
    if (!session.h) { error = netError(L"Network unavailable"); return false; }
    WinHttpSetTimeouts(session.h, 10000, 10000, 15000, 30000);
    WinHttpHandle conn(WinHttpConnect(session.h, host, uc.nPort, 0));
    if (!conn.h) { error = netError(L"Could not connect"); return false; }
    std::wstring object = std::wstring(path) + extra;
    WinHttpHandle req(WinHttpOpenRequest(conn.h, L"GET", object.c_str(), nullptr, WINHTTP_NO_REFERER,
                                         WINHTTP_DEFAULT_ACCEPT_TYPES, https ? WINHTTP_FLAG_SECURE : 0));
    if (!req.h) { error = netError(L"Could not connect"); return false; }
    if (!WinHttpSendRequest(req.h, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
        !WinHttpReceiveResponse(req.h, nullptr)) {
        error = netError(L"Could not connect");
        return false;
    }
    DWORD status = 0, len = sizeof(status);
    WinHttpQueryHeaders(req.h, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                        &status, &len, WINHTTP_NO_HEADER_INDEX);
    if (status != 200) { error = L"HTTP " + std::to_wstring(status); return false; }
    uint64_t total = 0;
    wchar_t clen[32] = L"";
    len = sizeof(clen);
    if (WinHttpQueryHeaders(req.h, WINHTTP_QUERY_CONTENT_LENGTH, WINHTTP_HEADER_NAME_BY_INDEX, clen, &len,
                            WINHTTP_NO_HEADER_INDEX))
        total = _wcstoui64(clen, nullptr, 10);

    std::string buf(64 * 1024, '\0');
    for (;;) {
        DWORD got = 0;
        if (!WinHttpReadData(req.h, buf.data(), static_cast<DWORD>(buf.size()), &got)) {
            error = netError(L"Download interrupted");
            return false;
        }
        if (got == 0) break;
        if (!sink(buf.data(), got, total)) { cancelled = true; return false; }
    }
    return true;
}

} // namespace

bool parseVersion(const std::wstring& s, Version& out) {
    std::wstring t = s;
    if (!t.empty() && (t[0] == L'v' || t[0] == L'V')) t.erase(0, 1);
    size_t a = t.find(L'.');
    if (a == std::wstring::npos) return false;
    size_t b = t.find(L'.', a + 1);
    if (b == std::wstring::npos) return false;
    Version v;
    if (!parseInt(t.substr(0, a), v.major) || !parseInt(t.substr(a + 1, b - a - 1), v.minor) ||
        !parseInt(t.substr(b + 1), v.patch))
        return false;
    out = v;
    return true;
}

int compareVersions(const Version& a, const Version& b) {
    if (a.major != b.major) return a.major < b.major ? -1 : 1;
    if (a.minor != b.minor) return a.minor < b.minor ? -1 : 1;
    if (a.patch != b.patch) return a.patch < b.patch ? -1 : 1;
    return 0;
}

bool isNewerVersion(const std::wstring& latest, const std::wstring& current) {
    Version l, c;
    if (!parseVersion(latest, l) || !parseVersion(current, c)) return false;
    return compareVersions(l, c) > 0;
}

std::wstring packageZipName(const std::wstring& version) { return L"mini-painter-" + version + L"-win64.zip"; }

bool parseLatestRelease(const std::string& text, ReleaseInfo& out) {
    nlohmann::json j = nlohmann::json::parse(text, nullptr, false);
    if (j.is_discarded() || !j.is_object()) return false;
    auto str = [&](const nlohmann::json& o, const char* key) -> std::wstring {
        auto it = o.find(key);
        return it != o.end() && it->is_string() ? utf8ToWide(it->get<std::string>()) : L"";
    };
    ReleaseInfo r;
    Version v;
    std::wstring tag = str(j, "tag_name");
    if (!parseVersion(tag, v)) return false;
    r.version = std::to_wstring(v.major) + L"." + std::to_wstring(v.minor) + L"." + std::to_wstring(v.patch);
    r.notes = str(j, "body");
    r.pageUrl = str(j, "html_url");
    std::wstring zipName = packageZipName(r.version), shaName = zipName + L".sha256";
    auto assets = j.find("assets");
    if (assets != j.end() && assets->is_array()) {
        for (const auto& a : *assets) {
            if (!a.is_object()) continue;
            std::wstring name = str(a, "name"), url = str(a, "browser_download_url");
            if (_wcsicmp(name.c_str(), zipName.c_str()) == 0) r.zipUrl = url;
            else if (_wcsicmp(name.c_str(), shaName.c_str()) == 0) r.shaUrl = url;
        }
    }
    out = r;
    return true;
}

std::string parseSha256Text(const std::string& text) {
    size_t i = 0;
    while (i < text.size()) {
        while (i < text.size() && !isxdigit(static_cast<unsigned char>(text[i]))) ++i;
        size_t j = i;
        while (j < text.size() && isxdigit(static_cast<unsigned char>(text[j]))) ++j;
        if (j - i == 64) {
            std::string h = text.substr(i, 64);
            for (char& c : h) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
            return h;
        }
        // 跳過這個 token（可能是檔名裡的一段 hex）
        while (j < text.size() && !isspace(static_cast<unsigned char>(text[j]))) ++j;
        i = j;
    }
    return "";
}

std::string sha256OfFile(const std::wstring& path) {
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN,
                           nullptr);
    if (f == INVALID_HANDLE_VALUE) return "";
    std::string result;
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) == 0 &&
        BCryptCreateHash(alg, &hash, nullptr, 0, nullptr, 0, 0) == 0) {
        std::string buf(1 << 20, '\0');
        bool ok = true;
        for (;;) {
            DWORD got = 0;
            if (!ReadFile(f, buf.data(), static_cast<DWORD>(buf.size()), &got, nullptr)) { ok = false; break; }
            if (got == 0) break;
            if (BCryptHashData(hash, reinterpret_cast<PUCHAR>(buf.data()), got, 0) != 0) { ok = false; break; }
        }
        unsigned char digest[32];
        if (ok && BCryptFinishHash(hash, digest, 32, 0) == 0) {
            static const char* hex = "0123456789abcdef";
            for (unsigned char c : digest) {
                result += hex[c >> 4];
                result += hex[c & 15];
            }
        }
    }
    if (hash) BCryptDestroyHash(hash);
    if (alg) BCryptCloseAlgorithmProvider(alg, 0);
    CloseHandle(f);
    return result;
}

bool httpGetText(const std::wstring& url, std::string& body, std::wstring& error) {
    body.clear();
    error.clear();
    constexpr size_t kMax = 4u << 20; // API 回應不會這麼大；擋住意外指到大檔
    bool cancelled = false;
    bool ok = httpGet(url, [&](const char* p, size_t n, uint64_t) {
        if (body.size() + n > kMax) return false;
        body.append(p, n);
        return true;
    }, error, cancelled);
    if (cancelled) error = L"Response too large";
    return ok;
}

bool httpDownload(const std::wstring& url, const std::wstring& path,
                  const std::function<bool(uint64_t, uint64_t)>& progress, std::wstring& error) {
    error.clear();
    HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) { error = netError(L"Could not create the download file"); return false; }
    uint64_t done = 0;
    bool writeFailed = false;
    bool cancelled = false;
    bool ok = httpGet(url, [&](const char* p, size_t n, uint64_t total) {
        DWORD w = 0;
        if (!WriteFile(f, p, static_cast<DWORD>(n), &w, nullptr) || w != n) { writeFailed = true; return false; }
        done += n;
        return !progress || progress(done, total);
    }, error, cancelled);
    CloseHandle(f);
    if (writeFailed) { error = L"Could not write the download file"; ok = false; }
    else if (cancelled) error.clear();
    if (!ok) DeleteFileW(path.c_str());
    return ok;
}

bool isDirWritable(const std::wstring& dir) {
    wchar_t tmp[MAX_PATH];
    if (!GetTempFileNameW(dir.c_str(), L"mpu", 0, tmp)) return false;
    std::wstring renamed = std::wstring(tmp) + L".r";
    bool ok = MoveFileExW(tmp, renamed.c_str(), 0) != 0;
    DeleteFileW(ok ? renamed.c_str() : tmp);
    return ok;
}

namespace {

void listFiles(const std::wstring& root, const std::wstring& rel, std::vector<std::wstring>& out) {
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(joinPath(joinPath(root, rel), L"*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) continue;
        std::wstring r = rel.empty() ? fd.cFileName : rel + L"\\" + fd.cFileName;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) listFiles(root, r, out);
        else out.push_back(r);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

void rollback(std::vector<InstallStep>& steps) {
    for (auto it = steps.rbegin(); it != steps.rend(); ++it) {
        if (it->placed) DeleteFileW(it->dst.c_str());
        if (!it->old.empty()) MoveFileExW(it->old.c_str(), it->dst.c_str(), MOVEFILE_REPLACE_EXISTING);
        it->placed = false;
        it->old.clear();
    }
}

} // namespace

std::vector<InstallStep> planInstall(const std::wstring& stagingDir, const std::wstring& appDir, const std::wstring& exeName) {
    std::vector<std::wstring> rels;
    listFiles(stagingDir, L"", rels);
    std::vector<InstallStep> steps;
    for (const auto& r : rels) {
        InstallStep s;
        s.src = joinPath(stagingDir, r);
        s.dst = joinPath(appDir, _wcsicmp(r.c_str(), L"mpainter.exe") == 0 ? exeName : r);
        steps.push_back(s);
    }
    return steps;
}

bool applyInstall(std::vector<InstallStep>& steps, std::wstring& error) {
    error.clear();
    for (auto& s : steps) {
        size_t slash = s.dst.find_last_of(L"\\/");
        if (slash != std::wstring::npos) CreateDirectoryW(s.dst.substr(0, slash).c_str(), nullptr);
        DWORD attr = GetFileAttributesW(s.dst.c_str());
        if (attr != INVALID_FILE_ATTRIBUTES) {
            if (attr & FILE_ATTRIBUTE_DIRECTORY) {
                error = L"A folder is in the way: " + s.dst;
                rollback(steps);
                return false;
            }
            // 上次更新留下、還刪不掉的 .old 就換下一個名字。
            std::wstring old;
            for (int n = 1; n <= 9 && old.empty(); ++n) {
                std::wstring cand = s.dst + L".old" + (n == 1 ? L"" : std::to_wstring(n));
                if (fileExists(cand)) DeleteFileW(cand.c_str());
                if (!fileExists(cand)) old = cand;
            }
            if (old.empty() || !MoveFileExW(s.dst.c_str(), old.c_str(), 0)) {
                error = netError((L"Could not rename " + s.dst).c_str());
                rollback(steps);
                return false;
            }
            s.old = old;
        }
        if (!CopyFileW(s.src.c_str(), s.dst.c_str(), FALSE)) {
            error = netError((L"Could not write " + s.dst).c_str());
            rollback(steps);
            return false;
        }
        s.placed = true;
    }
    return true;
}

int removeOldFiles(const std::wstring& dir) {
    int failed = 0;
    for (const std::wstring& d : {dir, joinPath(dir, L"lang")}) {
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileW(joinPath(d, L"*.old*").c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE) continue;
        do {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
            std::wstring name = fd.cFileName;
            size_t pos = name.rfind(L".old");
            if (pos == std::wstring::npos || pos == 0) continue;
            bool digits = true;
            for (size_t i = pos + 4; i < name.size(); ++i) digits = digits && iswdigit(name[i]);
            if (!digits) continue;
            if (!fileExists(joinPath(d, name.substr(0, pos)))) continue; // 不是換掉的舊檔，不碰
            if (!DeleteFileW(joinPath(d, name).c_str())) ++failed;
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    return failed;
}

} // namespace mp
