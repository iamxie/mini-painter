#include <catch2/catch_test_macros.hpp>

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <atomic>
#include <cwctype>
#include <map>
#include <set>
#include <string>
#include <thread>
#include <vector>


#include "core/Archive.h"
#include "core/TempDir.h"
#include "core/AppInfo.h"
#include "core/Update.h"
#include "test_archive_util.h"
#include "test_util.h"

using namespace mp;

namespace {

using mp::test::exists;
using mp::test::writeBytes;

std::wstring exeDirOfTests() { return mp::test::exeDir(); }

std::wstring testRoot(const wchar_t* name) { return mp::test::freshDir((std::wstring(L"update_") + name).c_str()); }

// 讀不到回 "<missing>"，讓「檔案不該存在」和「內容不對」在失敗訊息裡分得出來。
std::string readBytes(const std::wstring& path) { return exists(path) ? mp::test::readBytes(path) : "<missing>"; }

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
        for (SOCKET c : held_) closesocket(c);
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
    // 收下要求但一直不回應（連線握著，到伺服器關掉為止）。
    void hang(const std::string& path) { hang_.insert(path); }
    // 先送出標頭與一部分內容，然後就不送了（Content-Length 說還有更多）。
    void stall(const std::string& path, const std::string& partial, size_t promised) {
        routes_[path] = "HTTP/1.1 200 OK\r\nContent-Length: " + std::to_string(promised) + "\r\nConnection: close\r\n\r\n" + partial;
        hang_.insert(path);
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
            if (hang_.count(path)) {
                if (it != routes_.end()) send(c, it->second.data(), static_cast<int>(it->second.size()), 0);
                held_.push_back(c);
                continue;
            }
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
    std::set<std::string> hang_;
    std::vector<SOCKET> held_;
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
    REQUIRE(httpGetText(srv.url("/latest"), body).ok());
    CHECK(body == kReleaseJson);

    UpdateError e = httpGetText(srv.url("/limit"), body);
    CHECK(e.code == UpdateErrorCode::HttpStatus);
    CHECK(e.httpStatus == 403);
    CHECK(body.empty());
    e = httpGetText(srv.url("/none"), body);
    CHECK(e.code == UpdateErrorCode::HttpStatus);
    CHECK(e.httpStatus == 404);

    REQUIRE(httpGetText(srv.url("/moved"), body).ok());
    CHECK(body == kReleaseJson);

    e = httpGetText(L"http://127.0.0.1:1/x", body);
    CHECK(e.code == UpdateErrorCode::ConnectFailed);
    CHECK(e.systemError != 0);
    e = httpGetText(L"not a url", body);
    CHECK(e.code == UpdateErrorCode::BadUrl);
    CHECK(e.detail == L"not a url");

    // 回應大得不合理（查版本的 API 不會回這麼多）
    srv.ok("/huge", std::string(5u << 20, 'x'));
    e = httpGetText(srv.url("/huge"), body);
    CHECK(e.code == UpdateErrorCode::ResponseTooLarge);
    CHECK(body.empty());
}

TEST_CASE("httpDownload：寫檔、進度、取消時刪檔、失敗不留檔") {
    TestServer srv;
    std::string big(300 * 1024, 'x');
    for (size_t i = 0; i < big.size(); ++i) big[i] = static_cast<char>('a' + i % 26);
    srv.ok("/pkg.zip", big);
    srv.redirect("/dl", "/pkg.zip");
    std::wstring root = testRoot(L"dl");
    std::wstring out = root + L"\\pkg.zip";
    uint64_t lastDone = 0, lastTotal = 0;
    int calls = 0;
    REQUIRE(httpDownload(srv.url("/dl"), out, [&](uint64_t d, uint64_t t) { lastDone = d; lastTotal = t; ++calls; return true; }).ok());
    CHECK(readBytes(out) == big);
    CHECK(lastDone == big.size());
    CHECK(lastTotal == big.size());
    CHECK(calls >= 1);

    DeleteFileW(out.c_str());
    UpdateError e = httpDownload(srv.url("/dl"), out, [](uint64_t, uint64_t) { return false; });
    CHECK(e.cancelled());
    CHECK_FALSE(exists(out));

    e = httpDownload(srv.url("/missing"), out, nullptr);
    CHECK(e.code == UpdateErrorCode::HttpStatus);
    CHECK(e.httpStatus == 404);
    CHECK_FALSE(exists(out));

    // 目的地建不出檔（上層資料夾不存在）
    e = httpDownload(srv.url("/dl"), root + L"\\no\\such\\pkg.zip", nullptr);
    CHECK(e.code == UpdateErrorCode::CreateFileFailed);
    CHECK(e.systemError != 0);
}

TEST_CASE("HttpCancel：伺服器收了要求卻不回應時，取消馬上讓卡住的呼叫回來（B-24a）") {
    // 以前按了取消要等 WinHTTP 自己逾時（等回應 30 秒），這段時間進度框一直停在「取消中」。
    TestServer srv;
    srv.hang("/silent");
    HttpCancel cancel;
    std::string body;
    UpdateError result;
    const ULONGLONG t0 = GetTickCount64();
    std::thread worker([&] { result = httpGetText(srv.url("/silent"), body, &cancel); });
    Sleep(500); // 讓它連上、送出要求、卡在等回應
    CHECK_FALSE(cancel.cancelled());
    cancel.cancel();
    worker.join();
    const ULONGLONG elapsed = GetTickCount64() - t0;
    CHECK(result.cancelled());
    CHECK(cancel.cancelled());
    CHECK(elapsed < 5000); // 遠小於 30 秒的逾時

    // 取消之後才開始的要求：一開始就回 Cancelled，不連線
    srv.ok("/ok", "hello");
    CHECK(httpGetText(srv.url("/ok"), body, &cancel).cancelled());
    cancel.cancel(); // 重複取消沒有事

    // 沒被取消的要求照常做完，之後這個物件還能再用
    HttpCancel fresh;
    REQUIRE(httpGetText(srv.url("/ok"), body, &fresh).ok());
    CHECK(body == "hello");
    REQUIRE(httpGetText(srv.url("/ok"), body, &fresh).ok());
    CHECK_FALSE(fresh.cancelled());
}

TEST_CASE("HttpCancel：下載到一半伺服器不送了，取消馬上回來，不留下載到一半的檔") {
    TestServer srv;
    srv.stall("/half", std::string(1000, 'x'), 1000000);
    std::wstring out = testRoot(L"dlcancel") + L"\\pkg.zip";
    HttpCancel cancel;
    UpdateError result;
    const ULONGLONG t0 = GetTickCount64();
    std::thread worker([&] { result = httpDownload(srv.url("/half"), out, nullptr, &cancel); });
    Sleep(500);
    cancel.cancel();
    worker.join();
    CHECK(result.cancelled());
    CHECK(GetTickCount64() - t0 < 5000);
    CHECK_FALSE(exists(out));
}

TEST_CASE("fetchLatestRelease：查到最新版；回應不是預期的格式、連不上各有代碼；可以取消") {
    TestServer srv;
    srv.ok("/latest", kReleaseJson);
    srv.ok("/html", "<html>rate limited</html>");
    srv.status("/down", 503);
    srv.hang("/silent");
    ReleaseInfo r;
    REQUIRE(fetchLatestRelease(srv.url("/latest"), r).ok());
    CHECK(r.version == L"0.7.0");
    CHECK(fetchLatestRelease(srv.url("/html"), r).code == UpdateErrorCode::BadResponse);
    UpdateError e = fetchLatestRelease(srv.url("/down"), r);
    CHECK(e.code == UpdateErrorCode::HttpStatus);
    CHECK(e.httpStatus == 503);
    HttpCancel cancel;
    std::thread canceller([&] {
        Sleep(300);
        cancel.cancel();
    });
    CHECK(fetchLatestRelease(srv.url("/silent"), r, &cancel).cancelled());
    canceller.join();
}

TEST_CASE("downloadUpdate：下載、核對 .sha256、解開，並確認裡面有 mpainter.exe；各種失敗各有代碼") {
    mp::test::requireArchiveLibrary();
    std::wstring root = testRoot(L"flow");
    // 一個像樣的發佈包
    std::wstring zipPath = root + L"\\src.zip";
    mp::test::makeArchive(zipPath, bit7z::BitFormat::Zip, {{"mpainter.exe", "new exe"}, {"lang/zh-TW.txt", "zh"}, {"readme.txt", "r"}});
    const std::string zip = mp::test::readBytes(zipPath);
    const std::string sha = sha256OfFile(zipPath);
    // 沒有 exe 的包
    std::wstring noExePath = root + L"\\noexe.zip";
    mp::test::makeArchive(noExePath, bit7z::BitFormat::Zip, {{"readme.txt", "r"}});
    const std::string noExe = mp::test::readBytes(noExePath);

    TestServer srv;
    srv.ok("/pkg.zip", zip);
    srv.ok("/pkg.zip.sha256", sha + "  mini-painter-9.9.9-win64.zip\n");
    srv.ok("/wrong.sha256", std::string(64, '0') + "\n");
    srv.ok("/noexe.zip", noExe);
    srv.ok("/noexe.zip.sha256", sha256OfFile(noExePath) + "\n");
    srv.ok("/garbage.zip", "this is not a zip");
    {
        std::wstring g = root + L"\\garbage.bin";
        writeBytes(g, "this is not a zip");
        srv.ok("/garbage.zip.sha256", sha256OfFile(g) + "\n");
    }
    ReleaseInfo rel;
    rel.version = L"9.9.9";
    rel.zipUrl = srv.url("/pkg.zip");
    rel.shaUrl = srv.url("/pkg.zip.sha256");

    // 失敗的情況：跑一次，確認沒有給 staging，回傳錯誤。
    auto run = [&](const ReleaseInfo& r) {
        std::wstring w = root + L"\\fail", st;
        REQUIRE(createDirectories(w));
        UpdateError e = downloadUpdate(r, w, st);
        CHECK(st.empty()); // 失敗時不給 staging
        return e;
    };
    ReleaseInfo bad = rel;

    SECTION("成功：階段依序回報，解開的內容在 staging") {
        std::vector<DownloadStage> stages;
        uint64_t lastDone = 0;
        DownloadHooks hooks;
        hooks.onStage = [&](DownloadStage st) { stages.push_back(st); };
        hooks.progress = [&](uint64_t done, uint64_t) { lastDone = done; };
        std::wstring work = root + L"\\w1", staging;
        REQUIRE(createDirectories(work));
        REQUIRE(downloadUpdate(rel, work, staging, hooks).ok());
        CHECK(stages == std::vector<DownloadStage>{DownloadStage::Downloading, DownloadStage::Verifying, DownloadStage::Unpacking});
        CHECK(lastDone == zip.size());
        CHECK(staging == work + L"\\pkg");
        CHECK(readBytes(staging + L"\\mpainter.exe") == "new exe");
        CHECK(readBytes(staging + L"\\lang\\zh-TW.txt") == "zh");
        CHECK(readBytes(work + L"\\mini-painter-9.9.9-win64.zip") == zip);
    }
    SECTION("檢查碼對不上：不解開") {
        bad.shaUrl = srv.url("/wrong.sha256");
        CHECK(run(bad).code == UpdateErrorCode::ChecksumMismatch);
        CHECK_FALSE(exists(root + L"\\fail\\pkg\\mpainter.exe"));
    }
    SECTION(".sha256 抓不到") {
        bad.shaUrl = srv.url("/nope.sha256");
        UpdateError e = run(bad);
        CHECK(e.code == UpdateErrorCode::HttpStatus);
        CHECK(e.httpStatus == 404);
    }
    SECTION("zip 抓不到") {
        bad.zipUrl = srv.url("/nope.zip");
        CHECK(run(bad).code == UpdateErrorCode::HttpStatus);
    }
    SECTION("包裡沒有 exe") {
        bad.zipUrl = srv.url("/noexe.zip");
        bad.shaUrl = srv.url("/noexe.zip.sha256");
        CHECK(run(bad).code == UpdateErrorCode::NoExeInPackage);
    }
    SECTION("檢查碼對、但不是壓縮檔") {
        bad.zipUrl = srv.url("/garbage.zip");
        bad.shaUrl = srv.url("/garbage.zip.sha256");
        UpdateError e = run(bad);
        CHECK(e.code == UpdateErrorCode::UnpackFailed);
        CHECK_FALSE(e.archive.empty());
    }
    SECTION("一開始就取消") {
        HttpCancel cancelled;
        cancelled.cancel();
        DownloadHooks ch;
        ch.cancel = &cancelled;
        std::wstring work = root + L"\\w7", staging;
        REQUIRE(createDirectories(work));
        CHECK(downloadUpdate(rel, work, staging, ch).cancelled());
    }
    SECTION("下載完之後才取消：階段之間也會看，不解開") {
        HttpCancel late;
        DownloadHooks lh;
        lh.cancel = &late;
        lh.onStage = [&](DownloadStage st) {
            if (st == DownloadStage::Verifying) late.cancel();
        };
        std::wstring work = root + L"\\w8", staging;
        REQUIRE(createDirectories(work));
        CHECK(downloadUpdate(rel, work, staging, lh).cancelled());
        CHECK_FALSE(exists(work + L"\\pkg\\mpainter.exe"));
    }
}

TEST_CASE("downloadUpdate：解壓途中按取消 → 回報取消，不交出解好的發佈包") {
    // v0.9.9：解完不再看取消，回報成功並交出 staging，UI 就照樣換檔重開。
    mp::test::requireArchiveLibrary();
    std::wstring root = testRoot(L"unpackcancel");
    std::wstring zipPath = root + L"\\src.zip";
    mp::test::makeArchive(zipPath, bit7z::BitFormat::Zip, {{"mpainter.exe", "new exe"}});
    TestServer srv;
    srv.ok("/pkg.zip", mp::test::readBytes(zipPath));
    srv.ok("/pkg.zip.sha256", sha256OfFile(zipPath) + "\n");
    ReleaseInfo rel;
    rel.version = L"9.9.9";
    rel.zipUrl = srv.url("/pkg.zip");
    rel.shaUrl = srv.url("/pkg.zip.sha256");
    HttpCancel cancel;
    DownloadHooks hooks;
    hooks.cancel = &cancel;
    // 解壓本身停不下來：在進入解壓階段時按取消，等同「解壓期間按了取消」。
    hooks.onStage = [&](DownloadStage st) {
        if (st == DownloadStage::Unpacking) cancel.cancel();
    };
    std::wstring work = root + L"\\w", staging;
    REQUIRE(createDirectories(work));
    UpdateError e = downloadUpdate(rel, work, staging, hooks);
    CHECK(e.cancelled());
    CHECK(e.code == UpdateErrorCode::Cancelled);
    CHECK(staging.empty());

    // 沒按取消：照樣解好交出去（同一個伺服器、同一個發佈包）
    HttpCancel fresh;
    hooks.cancel = &fresh;
    hooks.onStage = nullptr;
    std::wstring work2 = root + L"\\w2";
    REQUIRE(createDirectories(work2));
    e = downloadUpdate(rel, work2, staging, hooks);
    CHECK(e.ok());
    CHECK(staging == work2 + L"\\pkg");
    CHECK(mp::test::readBytes(staging + L"\\mpainter.exe") == "new exe");
}

TEST_CASE("updateErrorMessage：每個代碼都有說明，取消與沒有錯誤是空的") {
    CHECK(std::wstring(updateErrorMessage(UpdateErrorCode::None)).empty());
    CHECK(std::wstring(updateErrorMessage(UpdateErrorCode::Cancelled)).empty());
    for (int c = static_cast<int>(UpdateErrorCode::BadUrl); c <= static_cast<int>(UpdateErrorCode::FolderNotWritable); ++c) {
        CAPTURE(c);
        CHECK_FALSE(std::wstring(updateErrorMessage(static_cast<UpdateErrorCode>(c))).empty());
    }
    UpdateError none;
    CHECK(none.ok());
    CHECK_FALSE(none.cancelled());
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

// 給下一個測試當「同一個 exe 的另一個行程」：只有用 [sleeper] 指名才會跑。
TEST_CASE("sleeper：睡一陣子就結束", "[.sleeper]") {
    Sleep(15000);
}

TEST_CASE("otherProcessesOfImage：找得到同一個 exe 的其他行程，不含自己，不分大小寫") {
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    const DWORD self = GetCurrentProcessId();
    auto has = [](const std::vector<unsigned long>& v, DWORD pid) { return std::find(v.begin(), v.end(), pid) != v.end(); };
    CHECK_FALSE(has(otherProcessesOfImage(exe, self), self));

    std::wstring cmd = L"\"" + std::wstring(exe) + L"\" \"[sleeper]\"";
    STARTUPINFOW si{sizeof(si)};
    PROCESS_INFORMATION pi{};
    REQUIRE(CreateProcessW(exe, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi));
    std::wstring upper = exe;
    for (auto& c : upper) c = static_cast<wchar_t>(towupper(c));
    auto found = otherProcessesOfImage(exe, self);
    auto foundUpper = otherProcessesOfImage(upper, self);
    auto elsewhere = otherProcessesOfImage(L"C:\\no-such-dir\\" + std::wstring(exe).substr(std::wstring(exe).find_last_of(L'\\') + 1), self);
    TerminateProcess(pi.hProcess, 0);
    WaitForSingleObject(pi.hProcess, 5000);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    CHECK(has(found, pi.dwProcessId));
    CHECK_FALSE(has(found, self));
    CHECK(has(foundUpper, pi.dwProcessId));
    CHECK(elsewhere.empty()); // 同檔名、不同資料夾不算
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
    writeBytes(app + L"\\notes.old", "user file");
    writeBytes(app + L"\\readme.txt.older", "user file");

    auto steps = planInstall(stage, app, L"mp.exe");
    REQUIRE(steps.size() == 4);
    REQUIRE(applyInstall(steps).ok());
    CHECK(readBytes(app + L"\\mp.exe") == "new exe");
    CHECK_FALSE(exists(app + L"\\mpainter.exe"));
    CHECK(readBytes(app + L"\\7z.dll") == "new dll");
    CHECK(readBytes(app + L"\\readme.txt") == "new readme"); // 原本沒有，新放
    CHECK(readBytes(app + L"\\lang\\zh-TW.txt") == "new zh");
    CHECK(readBytes(app + L"\\mp.exe.mpainter.tempold") == "old exe");
    CHECK(readBytes(app + L"\\7z.dll.mpainter.tempold") == "old dll");
    CHECK(readBytes(app + L"\\lang\\zh-TW.txt.mpainter.tempold") == "old zh");
    CHECK_FALSE(exists(app + L"\\readme.txt.mpainter.tempold"));
    CHECK(readBytes(app + L"\\mpainter.ini") == "settings");
    CHECK(readBytes(app + L"\\lang\\xx-XX.txt") == "user lang");

    CHECK(removeOldFiles(app) == 0);
    CHECK_FALSE(exists(app + L"\\mp.exe.mpainter.tempold"));
    CHECK_FALSE(exists(app + L"\\7z.dll.mpainter.tempold"));
    CHECK_FALSE(exists(app + L"\\lang\\zh-TW.txt.mpainter.tempold"));
    CHECK(readBytes(app + L"\\notes.old") == "user file");
    CHECK(readBytes(app + L"\\readme.txt.older") == "user file");
    CHECK(readBytes(app + L"\\mp.exe") == "new exe");
    CHECK(readBytes(app + L"\\7z.dll") == "new dll");
    CHECK(readBytes(app + L"\\lang\\zh-TW.txt") == "new zh");
}

TEST_CASE("removeOldFiles：使用者自己的 .old / .bak 一律不碰（X 在不在都一樣），只刪 .mpainter.tempold") {
    std::wstring app = testRoot(L"userold");
    writeBytes(app + L"\\notes.txt", "notes");
    writeBytes(app + L"\\notes.txt.old", "notes backup");
    writeBytes(app + L"\\mpainter.ini", "settings");
    writeBytes(app + L"\\mpainter.ini.old", "ini backup");
    writeBytes(app + L"\\mpainter.ini.bak", "ini bak");
    writeBytes(app + L"\\lang\\custom.txt", "custom");
    writeBytes(app + L"\\lang\\custom.txt.old", "custom backup");
    // v0.9.9 以前的更新程式留下的 X.old 也不刪（分不出是不是使用者的）
    writeBytes(app + L"\\7z.dll", "dll");
    writeBytes(app + L"\\7z.dll.old", "legacy old");
    writeBytes(app + L"\\7z.dll.old2", "legacy old2");
    // 名字像但不是：後面接的不是數字
    writeBytes(app + L"\\a.txt.mpainter.tempold.bak", "user");
    writeBytes(app + L"\\a.txt.mpainter.tempoldx", "user");
    // 是：不分大小寫；X 不在也刪（名字本身就說明是更新換下來的）
    writeBytes(app + L"\\b.txt.MPAINTER.TEMPOLD3", "old b");
    writeBytes(app + L"\\gone.txt.mpainter.tempold", "old gone");
    writeBytes(app + L"\\lang\\zh-TW.txt.mpainter.tempold12", "old zh");

    CHECK(removeOldFiles(app) == 0);
    CHECK(readBytes(app + L"\\notes.txt") == "notes");
    CHECK(readBytes(app + L"\\notes.txt.old") == "notes backup");
    CHECK(readBytes(app + L"\\mpainter.ini") == "settings");
    CHECK(readBytes(app + L"\\mpainter.ini.old") == "ini backup");
    CHECK(readBytes(app + L"\\mpainter.ini.bak") == "ini bak");
    CHECK(readBytes(app + L"\\lang\\custom.txt") == "custom");
    CHECK(readBytes(app + L"\\lang\\custom.txt.old") == "custom backup");
    CHECK(readBytes(app + L"\\7z.dll") == "dll");
    CHECK(readBytes(app + L"\\7z.dll.old") == "legacy old");
    CHECK(readBytes(app + L"\\7z.dll.old2") == "legacy old2");
    CHECK(readBytes(app + L"\\a.txt.mpainter.tempold.bak") == "user");
    CHECK(readBytes(app + L"\\a.txt.mpainter.tempoldx") == "user");
    CHECK_FALSE(exists(app + L"\\b.txt.MPAINTER.TEMPOLD3"));
    CHECK_FALSE(exists(app + L"\\gone.txt.mpainter.tempold"));
    CHECK_FALSE(exists(app + L"\\lang\\zh-TW.txt.mpainter.tempold12"));
}

TEST_CASE("applyInstall：使用者自己的 X.old 不動（X 是發佈包的檔），換下來的舊檔放在 X.mpainter.tempold") {
    std::wstring root = testRoot(L"userold2");
    std::wstring stage = root + L"\\stage", app = root + L"\\app";
    writeBytes(stage + L"\\readme.txt", "new readme");
    writeBytes(stage + L"\\lang\\ja-JP.txt", "new ja");
    writeBytes(app + L"\\readme.txt", "old readme");
    writeBytes(app + L"\\readme.txt.old", "readme backup");
    writeBytes(app + L"\\readme.txt.old2", "readme backup 2");
    writeBytes(app + L"\\lang\\ja-JP.txt", "old ja");
    writeBytes(app + L"\\lang\\ja-JP.txt.old", "ja backup");
    auto steps = planInstall(stage, app, L"mpainter.exe");
    REQUIRE(applyInstall(steps).ok());
    CHECK(readBytes(app + L"\\readme.txt") == "new readme");
    CHECK(readBytes(app + L"\\lang\\ja-JP.txt") == "new ja");
    CHECK(readBytes(app + L"\\readme.txt.old") == "readme backup");
    CHECK(readBytes(app + L"\\readme.txt.old2") == "readme backup 2");
    CHECK(readBytes(app + L"\\lang\\ja-JP.txt.old") == "ja backup");
    CHECK(readBytes(app + L"\\readme.txt.mpainter.tempold") == "old readme");
    CHECK(readBytes(app + L"\\lang\\ja-JP.txt.mpainter.tempold") == "old ja");
    CHECK(removeOldFiles(app) == 0);
    CHECK(readBytes(app + L"\\readme.txt.old") == "readme backup");
    CHECK(readBytes(app + L"\\readme.txt.old2") == "readme backup 2");
    CHECK(readBytes(app + L"\\lang\\ja-JP.txt.old") == "ja backup");
    CHECK_FALSE(exists(app + L"\\readme.txt.mpainter.tempold"));
    CHECK_FALSE(exists(app + L"\\lang\\ja-JP.txt.mpainter.tempold"));
}

TEST_CASE("applyInstall：上次的 .mpainter.tempold 還刪不掉（被占住）就用 .mpainter.tempold2") {
    std::wstring root = testRoot(L"old2");
    std::wstring stage = root + L"\\stage", app = root + L"\\app";
    writeBytes(stage + L"\\7z.dll", "v3");
    writeBytes(app + L"\\7z.dll", "v2");
    writeBytes(app + L"\\7z.dll.mpainter.tempold", "v1");
    HANDLE lock = CreateFileW((app + L"\\7z.dll.mpainter.tempold").c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    REQUIRE(lock != INVALID_HANDLE_VALUE);
    auto steps = planInstall(stage, app, L"mpainter.exe");
    REQUIRE(applyInstall(steps).ok());
    CHECK(readBytes(app + L"\\7z.dll") == "v3");
    CHECK(readBytes(app + L"\\7z.dll.mpainter.tempold") == "v1");
    CHECK(readBytes(app + L"\\7z.dll.mpainter.tempold2") == "v2");
    CHECK(removeOldFiles(app) == 1); // .mpainter.tempold 還被占住
    CloseHandle(lock);
    CHECK(removeOldFiles(app) == 0);
    CHECK_FALSE(exists(app + L"\\7z.dll.mpainter.tempold"));
    CHECK_FALSE(exists(app + L"\\7z.dll.mpainter.tempold2"));
    CHECK(readBytes(app + L"\\7z.dll") == "v3");
}

TEST_CASE("applyInstall：中途失敗整批還原，不留 .mpainter.tempold") {
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
    UpdateError err = applyInstall(steps);
    CHECK_FALSE(err.ok());
    CloseHandle(lock);
    CHECK(err.code == UpdateErrorCode::RenameFailed);
    CHECK(err.detail == app + L"\\b.txt");
    CHECK(err.systemError != 0);
    CHECK(readBytes(app + L"\\a.txt") == "old a");
    CHECK(readBytes(app + L"\\b.txt") == "old b");
    CHECK_FALSE(exists(app + L"\\c.txt"));
    CHECK_FALSE(exists(app + L"\\a.txt.mpainter.tempold"));
    CHECK_FALSE(exists(app + L"\\b.txt.mpainter.tempold"));
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
    REQUIRE(applyInstall(steps).ok());
    CHECK(readBytes(app + L"\\7z.dll") == "new dll");
    CHECK(exists(app + L"\\7z.dll.mpainter.tempold"));
    CHECK(removeOldFiles(app) == 1); // 還載入中，刪不掉
    FreeLibrary(real);
    CHECK(removeOldFiles(app) == 0);
    CHECK_FALSE(exists(app + L"\\7z.dll.mpainter.tempold"));
}

TEST_CASE("extractAll：照資料夾結構整包解開（給發佈包用）") {
    mp::test::requireArchiveLibrary();
    std::wstring root = testRoot(L"extractall");
    std::wstring zip = root + L"\\pkg.zip";
    {
        mp::test::makeArchive(zip, bit7z::BitFormat::Zip,
                              {{"mpainter.exe", "exe"}, {"lang/zh-TW.txt", "zh"}, {"readme.txt", "r"}});
    }
    REQUIRE(extractAll(zip, root + L"\\out").empty());
    CHECK(readBytes(root + L"\\out\\mpainter.exe") == "exe");
    CHECK(readBytes(root + L"\\out\\lang\\zh-TW.txt") == "zh");
    CHECK(readBytes(root + L"\\out\\readme.txt") == "r");

    writeBytes(root + L"\\bad.zip", "not a zip at all");
    ArchiveError bad = extractAll(root + L"\\bad.zip", root + L"\\out2");
    CHECK_FALSE(bad.empty());
}

TEST_CASE("發佈包有巢狀的子資料夾：安裝會把資料夾建出來；要寫的資料夾都要能寫才裝（R-32）") {
    std::wstring root = testRoot(L"nested");
    std::wstring stage = root + L"\\stage", app = root + L"\\app";
    writeBytes(stage + L"\\mpainter.exe", "exe");
    writeBytes(stage + L"\\lang\\zh-TW.txt", "zh");
    writeBytes(stage + L"\\docs\\en\\guide.txt", "guide"); // 兩層、app 裡都還沒有
    REQUIRE(createDirectories(app));
    auto steps = planInstall(stage, app, L"mpainter.exe");
    REQUIRE(steps.size() == 3);
    CHECK(installTargetsWritable(steps).ok()); // 還不存在的資料夾：看最近的、存在的上層（app）
    REQUIRE(applyInstall(steps).ok());
    CHECK(readBytes(app + L"\\docs\\en\\guide.txt") == "guide");
    CHECK(readBytes(app + L"\\lang\\zh-TW.txt") == "zh");

    // 要寫的位置上有個同名的檔擋著資料夾：建不出來，整批還原
    std::wstring app2 = root + L"\\app2";
    writeBytes(app2 + L"\\docs", "a file named docs");
    writeBytes(app2 + L"\\mpainter.exe", "old exe");
    steps = planInstall(stage, app2, L"mpainter.exe");
    UpdateError err = applyInstall(steps);
    CHECK_FALSE(err.ok());
    CHECK(readBytes(app2 + L"\\mpainter.exe") == "old exe"); // 已經換掉的也還原了
    CHECK_FALSE(exists(app2 + L"\\mpainter.exe.mpainter.tempold"));

    // 目的地是資料夾：FolderInTheWay，帶路徑
    std::wstring app3 = root + L"\\app3";
    REQUIRE(createDirectories(app3 + L"\\mpainter.exe"));
    steps = planInstall(stage, app3, L"mpainter.exe");
    err = applyInstall(steps);
    CHECK(err.code == UpdateErrorCode::FolderInTheWay);
    CHECK(err.detail == app3 + L"\\mpainter.exe");

    // 整個 app 資料夾不存在、上層也不存在（磁碟機代號不存在）：不能寫
    std::vector<InstallStep> nowhere(1);
    nowhere[0].src = stage + L"\\mpainter.exe";
    nowhere[0].dst = L"Q:\\no\\such\\place\\mpainter.exe";
    if (GetFileAttributesW(L"Q:\\") == INVALID_FILE_ATTRIBUTES) CHECK(installTargetsWritable(nowhere).ok()); // 沒有可檢查的上層：交給 applyInstall 去失敗
}

TEST_CASE("removeOldFiles：只找這一版發佈包會放檔案的資料夾（根目錄與 kPackageSubdirs）") {
    std::wstring root = testRoot(L"oldscope");
    std::wstring app = root + L"\\app";
    writeBytes(app + L"\\a.txt", "new");
    writeBytes(app + L"\\a.txt.mpainter.tempold", "old");
    writeBytes(app + L"\\lang\\zh-TW.txt", "new");
    writeBytes(app + L"\\lang\\zh-TW.txt.mpainter.tempold", "old");
    writeBytes(app + L"\\plugins\\jxl\\jxl.dll", "new");
    writeBytes(app + L"\\plugins\\jxl\\jxl.dll.mpainter.tempold", "old");
    // 別的子資料夾（例如解壓縮的暫存、使用者自己裝的 plugin、使用者自己的東西）不碰
    writeBytes(app + L"\\temp\\b.txt", "x");
    writeBytes(app + L"\\temp\\b.txt.mpainter.tempold", "user");
    writeBytes(app + L"\\plugins\\mine\\mine.dll.mpainter.tempold", "user");
    CHECK(removeOldFiles(app) == 0);
    CHECK_FALSE(exists(app + L"\\a.txt.mpainter.tempold"));
    CHECK_FALSE(exists(app + L"\\lang\\zh-TW.txt.mpainter.tempold"));
    CHECK_FALSE(exists(app + L"\\plugins\\jxl\\jxl.dll.mpainter.tempold"));
    CHECK(exists(app + L"\\temp\\b.txt.mpainter.tempold"));
    CHECK(exists(app + L"\\plugins\\mine\\mine.dll.mpainter.tempold"));
    // 發佈包現在的結構：lang 與每個隨附 plugin 的 plugins\<id>。改了打包要一起改這個表
    auto has = [](const std::wstring& dir) {
        for (const wchar_t* sub : kPackageSubdirs)
            if (_wcsicmp(sub, dir.c_str()) == 0) return true;
        return false;
    };
    CHECK(has(L"lang"));
    for (const wchar_t* id : kBundledPlugins) {
        INFO(id);
        CHECK(has(std::wstring(L"plugins\\") + id));
    }
}

// 要連外網，平常不跑：mp_tests "[.net]"。對真的 GitHub（HTTPS）在不同的時間點取消，看會不會馬上回來。
// 交握到一半、等回應、讀內容的途中都可能被取消到；每一次不是做完（ok）就是 Cancelled，而且都很快。
TEST_CASE("HttpCancel：對真的 HTTPS 伺服器在不同時間點取消", "[.net]") {
    for (int delayMs : {0, 5, 15, 30, 60, 100, 150, 250, 400}) {
        HttpCancel cancel;
        std::string body;
        UpdateError result;
        const ULONGLONG t0 = GetTickCount64();
        std::thread worker([&] { result = httpGetText(kDefaultUpdateUrl, body, &cancel); });
        Sleep(static_cast<DWORD>(delayMs));
        cancel.cancel();
        worker.join();
        const ULONGLONG elapsed = GetTickCount64() - t0;
        std::printf("NET cancel after %3d ms: %s, returned after %llu ms, body %zu bytes\n", delayMs,
                    result.ok() ? "completed" : result.cancelled() ? "cancelled" : "other error", elapsed, body.size());
        std::fflush(stdout);
        CHECK((result.ok() || result.cancelled()));
        CHECK(elapsed < static_cast<ULONGLONG>(delayMs) + 3000);
    }
}
