#include <catch2/catch_test_macros.hpp>

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <atomic>
#include <map>
#include <string>
#include <thread>
#include <vector>

#include <bit7z/bit7zlibrary.hpp>
#include <bit7z/bitarchivewriter.hpp>
#include <bit7z/bitformat.hpp>

#include "core/Archive.h"
#include "core/TempDir.h"
#include "core/Update.h"

using namespace mp;

namespace {

std::wstring exeDirOfTests() {
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring p = exe;
    return p.substr(0, p.find_last_of(L"\\/") + 1);
}

std::wstring testRoot(const wchar_t* name) {
    wchar_t dir[MAX_PATH];
    GetTempPathW(MAX_PATH, dir);
    std::wstring root = std::wstring(dir) + L"mp_test_update_" + name;
    removeDirectoryRecursive(root);
    REQUIRE(createDirectories(root));
    return root;
}

std::string readBytes(const std::wstring& path) {
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return "<missing>";
    std::string bytes;
    char buf[4096];
    DWORD got = 0;
    while (ReadFile(f, buf, sizeof(buf), &got, nullptr) && got > 0) bytes.append(buf, got);
    CloseHandle(f);
    return bytes;
}

void writeBytes(const std::wstring& path, const std::string& bytes) {
    size_t slash = path.find_last_of(L"\\/");
    REQUIRE(createDirectories(path.substr(0, slash)));
    HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    REQUIRE(f != INVALID_HANDLE_VALUE);
    DWORD w = 0;
    WriteFile(f, bytes.data(), static_cast<DWORD>(bytes.size()), &w, nullptr);
    CloseHandle(f);
}

bool exists(const std::wstring& p) { return GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES; }

// 迷你 HTTP 伺服器：127.0.0.1 的隨機 port，路徑 → 整段原始回應。每個連線回一次就關。
class TestServer {
public:
    TestServer() {
        WSADATA wsa;
        WSAStartup(MAKEWORD(2, 2), &wsa);
        ls_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        a.sin_port = 0;
        bind(ls_, reinterpret_cast<sockaddr*>(&a), sizeof(a));
        int len = sizeof(a);
        getsockname(ls_, reinterpret_cast<sockaddr*>(&a), &len);
        port_ = ntohs(a.sin_port);
        listen(ls_, 8);
        th_ = std::thread([this] { loop(); });
    }
    ~TestServer() {
        stop_ = true;
        closesocket(ls_);
        th_.join();
        WSACleanup();
    }
    std::wstring url(const std::string& path) const {
        return L"http://127.0.0.1:" + std::to_wstring(port_) + std::wstring(path.begin(), path.end());
    }
    void ok(const std::string& path, const std::string& body) {
        routes_[path] = "HTTP/1.1 200 OK\r\nContent-Length: " + std::to_string(body.size()) +
                        "\r\nConnection: close\r\n\r\n" + body;
    }
    void status(const std::string& path, int code) {
        routes_[path] = "HTTP/1.1 " + std::to_string(code) + " X\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
    }
    void redirect(const std::string& path, const std::string& to) {
        routes_[path] = "HTTP/1.1 302 Found\r\nLocation: " + to + "\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
    }

private:
    void loop() {
        while (!stop_) {
            SOCKET c = accept(ls_, nullptr, nullptr);
            if (c == INVALID_SOCKET) break;
            std::string req;
            char buf[2048];
            while (req.find("\r\n\r\n") == std::string::npos) {
                int n = recv(c, buf, sizeof(buf), 0);
                if (n <= 0) break;
                req.append(buf, static_cast<size_t>(n));
            }
            std::string path;
            size_t sp1 = req.find(' '), sp2 = req.find(' ', sp1 + 1);
            if (sp1 != std::string::npos && sp2 != std::string::npos) path = req.substr(sp1 + 1, sp2 - sp1 - 1);
            auto it = routes_.find(path);
            std::string resp = it != routes_.end() ? it->second
                                                   : "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
            send(c, resp.data(), static_cast<int>(resp.size()), 0);
            shutdown(c, SD_SEND);
            closesocket(c);
        }
    }
    SOCKET ls_ = INVALID_SOCKET;
    int port_ = 0;
    std::atomic<bool> stop_{false};
    std::map<std::string, std::string> routes_;
    std::thread th_;
};

const char* kReleaseJson = R"({
  "tag_name": "v0.7.0",
  "html_url": "https://github.com/iamxie/mini-painter/releases/tag/v0.7.0",
  "body": "## 新功能\r\n- 自動更新",
  "assets": [
    {"name": "7z2605-src.7z", "browser_download_url": "https://example.com/src.7z"},
    {"name": "mini-painter-0.7.0-win64.zip", "browser_download_url": "https://example.com/pkg.zip"},
    {"name": "mini-painter-0.7.0-win64.zip.sha256", "browser_download_url": "https://example.com/pkg.zip.sha256"}
  ]
})";

} // namespace

TEST_CASE("parseVersion：有無 v、三段數字；其他格式拒絕") {
    Version v;
    REQUIRE(parseVersion(L"0.6.0", v));
    CHECK((v.major == 0 && v.minor == 6 && v.patch == 0));
    REQUIRE(parseVersion(L"v1.12.3", v));
    CHECK((v.major == 1 && v.minor == 12 && v.patch == 3));
    CHECK(parseVersion(L"V2.0.10", v));
    CHECK_FALSE(parseVersion(L"", v));
    CHECK_FALSE(parseVersion(L"1.2", v));
    CHECK_FALSE(parseVersion(L"1.2.3-beta", v));
    CHECK_FALSE(parseVersion(L"1..3", v));
    CHECK_FALSE(parseVersion(L"nightly", v));
}

TEST_CASE("isNewerVersion：逐段數字比較（不是字串比較）") {
    CHECK(isNewerVersion(L"v0.7.0", L"0.6.0"));
    CHECK(isNewerVersion(L"0.10.0", L"0.9.9"));
    CHECK(isNewerVersion(L"1.0.0", L"0.99.99"));
    CHECK(isNewerVersion(L"0.6.1", L"0.6.0"));
    CHECK_FALSE(isNewerVersion(L"0.6.0", L"0.6.0"));
    CHECK_FALSE(isNewerVersion(L"0.5.9", L"0.6.0"));
    CHECK_FALSE(isNewerVersion(L"garbage", L"0.6.0"));
}

TEST_CASE("parseLatestRelease：版本、說明、網頁、發佈包與 .sha256；中文說明") {
    ReleaseInfo r;
    REQUIRE(parseLatestRelease(kReleaseJson, r));
    CHECK(r.version == L"0.7.0");
    CHECK(r.pageUrl == L"https://github.com/iamxie/mini-painter/releases/tag/v0.7.0");
    CHECK(r.notes == L"## 新功能\r\n- 自動更新");
    CHECK(r.zipUrl == L"https://example.com/pkg.zip");
    CHECK(r.shaUrl == L"https://example.com/pkg.zip.sha256");
    CHECK(packageZipName(L"0.7.0") == L"mini-painter-0.7.0-win64.zip");
}

TEST_CASE("parseLatestRelease：asset 缺了不算失敗；JSON 壞掉、標籤不是版本號才失敗") {
    ReleaseInfo r;
    REQUIRE(parseLatestRelease(R"({"tag_name":"v0.8.1","body":null,"assets":[]})", r));
    CHECK(r.version == L"0.8.1");
    CHECK(r.notes.empty());
    CHECK(r.zipUrl.empty());
    CHECK(r.shaUrl.empty());
    // 別的版本的發佈包不算
    REQUIRE(parseLatestRelease(R"({"tag_name":"v0.8.1","assets":[{"name":"mini-painter-0.8.0-win64.zip","browser_download_url":"x"}]})", r));
    CHECK(r.zipUrl.empty());
    CHECK_FALSE(parseLatestRelease("{not json", r));
    CHECK_FALSE(parseLatestRelease("[]", r));
    CHECK_FALSE(parseLatestRelease(R"({"tag_name":"latest"})", r));
    CHECK_FALSE(parseLatestRelease(R"({"message":"API rate limit exceeded"})", r));
}

TEST_CASE("parseSha256Text：sha256sum 格式、大寫、只有 hash；檔名裡的 hex 不誤認") {
    const std::string h = "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad";
    CHECK(parseSha256Text(h + "  mini-painter-0.7.0-win64.zip\n") == h);
    CHECK(parseSha256Text("BA7816BF8F01CFEA414140DE5DAE2223B00361A396177A9CB410FF61F20015AD") == h);
    CHECK(parseSha256Text("\xEF\xBB\xBF" + h + "\r\n") == h);
    CHECK(parseSha256Text("deadbeef-file " + h) == h);
    CHECK(parseSha256Text("not a hash") == "");
    CHECK(parseSha256Text(h.substr(0, 63)) == "");
}

TEST_CASE("sha256OfFile：已知值；檔案不存在回空") {
    std::wstring root = testRoot(L"sha");
    writeBytes(root + L"\\abc.txt", "abc");
    CHECK(sha256OfFile(root + L"\\abc.txt") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    writeBytes(root + L"\\empty.txt", "");
    CHECK(sha256OfFile(root + L"\\empty.txt") == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    CHECK(sha256OfFile(root + L"\\nope.txt") == "");
}

TEST_CASE("httpGetText：200 取內容、非 200 回 HTTP 碼、跟著轉址、連不上回錯誤") {
    TestServer srv;
    srv.ok("/latest", kReleaseJson);
    srv.status("/limit", 403);
    srv.redirect("/moved", "/latest");
    std::string body;
    std::wstring err;
    REQUIRE(httpGetText(srv.url("/latest"), body, err));
    CHECK(body == kReleaseJson);
    CHECK(err.empty());

    CHECK_FALSE(httpGetText(srv.url("/limit"), body, err));
    CHECK(err == L"HTTP 403");
    CHECK_FALSE(httpGetText(srv.url("/none"), body, err));
    CHECK(err == L"HTTP 404");

    REQUIRE(httpGetText(srv.url("/moved"), body, err));
    CHECK(body == kReleaseJson);

    CHECK_FALSE(httpGetText(L"http://127.0.0.1:1/x", body, err));
    CHECK_FALSE(err.empty());
    CHECK_FALSE(httpGetText(L"not a url", body, err));
    CHECK_FALSE(err.empty());
}

TEST_CASE("httpDownload：寫檔、進度、取消時刪檔且 error 空、失敗不留檔") {
    TestServer srv;
    std::string big(300 * 1024, 'x');
    for (size_t i = 0; i < big.size(); ++i) big[i] = static_cast<char>('a' + i % 26);
    srv.ok("/pkg.zip", big);
    srv.redirect("/dl", "/pkg.zip");
    std::wstring root = testRoot(L"dl");
    std::wstring out = root + L"\\pkg.zip";
    std::wstring err;
    uint64_t lastDone = 0, lastTotal = 0;
    int calls = 0;
    REQUIRE(httpDownload(srv.url("/dl"), out, [&](uint64_t d, uint64_t t) { lastDone = d; lastTotal = t; ++calls; return true; }, err));
    CHECK(readBytes(out) == big);
    CHECK(lastDone == big.size());
    CHECK(lastTotal == big.size());
    CHECK(calls >= 1);

    DeleteFileW(out.c_str());
    CHECK_FALSE(httpDownload(srv.url("/dl"), out, [](uint64_t, uint64_t) { return false; }, err));
    CHECK(err.empty());
    CHECK_FALSE(exists(out));

    CHECK_FALSE(httpDownload(srv.url("/missing"), out, nullptr, err));
    CHECK(err == L"HTTP 404");
    CHECK_FALSE(exists(out));
}

TEST_CASE("isDirWritable：temp 可寫、不存在的資料夾不行，試寫不留檔") {
    std::wstring root = testRoot(L"writable");
    CHECK(isDirWritable(root));
    CHECK_FALSE(isDirWritable(root + L"\\no\\such"));
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((root + L"\\*").c_str(), &fd);
    int n = 0;
    if (h != INVALID_HANDLE_VALUE) {
        do { if (wcscmp(fd.cFileName, L".") && wcscmp(fd.cFileName, L"..")) ++n; } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    CHECK(n == 0);
}

TEST_CASE("planInstall / applyInstall / removeOldFiles：換掉發佈包的檔，exe 對應改過的名字，使用者的檔不動") {
    std::wstring root = testRoot(L"install");
    std::wstring stage = root + L"\\stage", app = root + L"\\app";
    writeBytes(stage + L"\\mpainter.exe", "new exe");
    writeBytes(stage + L"\\7z.dll", "new dll");
    writeBytes(stage + L"\\readme.txt", "new readme");
    writeBytes(stage + L"\\lang\\zh-TW.txt", "new zh");
    writeBytes(app + L"\\mp.exe", "old exe");
    writeBytes(app + L"\\7z.dll", "old dll");
    writeBytes(app + L"\\mpainter.ini", "settings");
    writeBytes(app + L"\\lang\\zh-TW.txt", "old zh");
    writeBytes(app + L"\\lang\\xx-XX.txt", "user lang");
    writeBytes(app + L"\\notes.old", "user file");        // 不是換掉的舊檔：notes 不存在
    writeBytes(app + L"\\readme.txt.older", "user file"); // 不是 .old / .oldN

    auto steps = planInstall(stage, app, L"mp.exe");
    REQUIRE(steps.size() == 4);
    std::wstring err;
    REQUIRE(applyInstall(steps, err));
    CHECK(readBytes(app + L"\\mp.exe") == "new exe");
    CHECK_FALSE(exists(app + L"\\mpainter.exe"));
    CHECK(readBytes(app + L"\\7z.dll") == "new dll");
    CHECK(readBytes(app + L"\\readme.txt") == "new readme"); // 原本沒有，新放
    CHECK(readBytes(app + L"\\lang\\zh-TW.txt") == "new zh");
    CHECK(readBytes(app + L"\\mp.exe.old") == "old exe");
    CHECK(readBytes(app + L"\\7z.dll.old") == "old dll");
    CHECK(readBytes(app + L"\\lang\\zh-TW.txt.old") == "old zh");
    CHECK_FALSE(exists(app + L"\\readme.txt.old"));
    CHECK(readBytes(app + L"\\mpainter.ini") == "settings");
    CHECK(readBytes(app + L"\\lang\\xx-XX.txt") == "user lang");

    CHECK(removeOldFiles(app) == 0);
    CHECK_FALSE(exists(app + L"\\mp.exe.old"));
    CHECK_FALSE(exists(app + L"\\7z.dll.old"));
    CHECK_FALSE(exists(app + L"\\lang\\zh-TW.txt.old"));
    CHECK(exists(app + L"\\notes.old"));
    CHECK(exists(app + L"\\readme.txt.older"));
}

TEST_CASE("applyInstall：上次的 .old 還刪不掉（被占住）就用 .old2") {
    std::wstring root = testRoot(L"old2");
    std::wstring stage = root + L"\\stage", app = root + L"\\app";
    writeBytes(stage + L"\\7z.dll", "v3");
    writeBytes(app + L"\\7z.dll", "v2");
    writeBytes(app + L"\\7z.dll.old", "v1");
    HANDLE lock = CreateFileW((app + L"\\7z.dll.old").c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    REQUIRE(lock != INVALID_HANDLE_VALUE);
    auto steps = planInstall(stage, app, L"mpainter.exe");
    std::wstring err;
    REQUIRE(applyInstall(steps, err));
    CHECK(readBytes(app + L"\\7z.dll") == "v3");
    CHECK(readBytes(app + L"\\7z.dll.old2") == "v2");
    CHECK(removeOldFiles(app) == 1); // .old 還被占住
    CloseHandle(lock);
    CHECK(removeOldFiles(app) == 0);
    CHECK_FALSE(exists(app + L"\\7z.dll.old"));
    CHECK_FALSE(exists(app + L"\\7z.dll.old2"));
}

TEST_CASE("applyInstall：中途失敗整批還原，不留 .old") {
    std::wstring root = testRoot(L"rollback");
    std::wstring stage = root + L"\\stage", app = root + L"\\app";
    writeBytes(stage + L"\\a.txt", "new a");
    writeBytes(stage + L"\\b.txt", "new b");
    writeBytes(stage + L"\\c.txt", "new c");
    writeBytes(app + L"\\a.txt", "old a");
    writeBytes(app + L"\\b.txt", "old b");
    // b.txt 被占住（沒給 FILE_SHARE_DELETE）：改名會失敗
    HANDLE lock = CreateFileW((app + L"\\b.txt").c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    REQUIRE(lock != INVALID_HANDLE_VALUE);
    auto steps = planInstall(stage, app, L"mpainter.exe");
    std::wstring err;
    CHECK_FALSE(applyInstall(steps, err));
    CloseHandle(lock);
    CHECK_FALSE(err.empty());
    CHECK(readBytes(app + L"\\a.txt") == "old a");
    CHECK(readBytes(app + L"\\b.txt") == "old b");
    CHECK_FALSE(exists(app + L"\\c.txt"));
    CHECK_FALSE(exists(app + L"\\a.txt.old"));
    CHECK_FALSE(exists(app + L"\\b.txt.old"));
}

TEST_CASE("applyInstall：已載入的 DLL 能改名換掉（執行中的 exe 同理），舊檔等卸載後才刪得掉") {
    std::wstring root = testRoot(L"loaded");
    std::wstring stage = root + L"\\stage", app = root + L"\\app";
    REQUIRE(createDirectories(app));
    REQUIRE(CopyFileW((exeDirOfTests() + L"7z.dll").c_str(), (app + L"\\7z.dll").c_str(), FALSE));
    HMODULE real = LoadLibraryW((app + L"\\7z.dll").c_str());
    REQUIRE(real != nullptr);
    writeBytes(stage + L"\\7z.dll", "new dll");
    auto steps = planInstall(stage, app, L"mpainter.exe");
    std::wstring err;
    REQUIRE(applyInstall(steps, err));
    CHECK(readBytes(app + L"\\7z.dll") == "new dll");
    CHECK(exists(app + L"\\7z.dll.old"));
    CHECK(removeOldFiles(app) == 1); // 還載入中，刪不掉
    FreeLibrary(real);
    CHECK(removeOldFiles(app) == 0);
    CHECK_FALSE(exists(app + L"\\7z.dll.old"));
}

TEST_CASE("extractAll：照資料夾結構整包解開（給發佈包用）") {
    REQUIRE(loadArchiveLibrary(exeDirOfTests() + L"7z.dll"));
    std::wstring root = testRoot(L"extractall");
    std::wstring zip = root + L"\\pkg.zip";
    {
        static bit7z::Bit7zLibrary lib(bit7z::to_tstring(exeDirOfTests() + L"7z.dll"));
        bit7z::BitArchiveWriter w(lib, bit7z::BitFormat::Zip);
        std::vector<bit7z::buffer_t> bufs = {{'e', 'x', 'e'}, {'z', 'h'}, {'r'}};
        w.addFile(bufs[0], "mpainter.exe");
        w.addFile(bufs[1], "lang/zh-TW.txt");
        w.addFile(bufs[2], "readme.txt");
        w.compressTo(bit7z::to_tstring(zip));
    }
    std::wstring err;
    REQUIRE(extractAll(zip, root + L"\\out", err));
    CHECK(readBytes(root + L"\\out\\mpainter.exe") == "exe");
    CHECK(readBytes(root + L"\\out\\lang\\zh-TW.txt") == "zh");
    CHECK(readBytes(root + L"\\out\\readme.txt") == "r");

    writeBytes(root + L"\\bad.zip", "not a zip at all");
    CHECK_FALSE(extractAll(root + L"\\bad.zip", root + L"\\out2", err));
    CHECK_FALSE(err.empty());
}
