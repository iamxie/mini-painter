#include "test_util.h"
#include "test_archive_util.h"

#include <catch2/catch_test_case_info.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/reporters/catch_reporter_event_listener.hpp>
#include <catch2/reporters/catch_reporter_registrars.hpp>

#include <windows.h>

#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <mutex>
#include <thread>

#include <bit7z/bitarchivewriter.hpp>

#include "core/Archive.h"
#include "core/FolderNav.h"
#include "core/ImageIO.h"
#include "core/Lang.h"
#include "core/PluginRun.h"
#include "core/Plugins.h"
#include "core/TempDir.h"

namespace mp::test {

namespace {

const wchar_t kRootPrefix[] = L"mp_tests_";

std::wstring systemTemp() {
    wchar_t dir[MAX_PATH];
    GetTempPathW(MAX_PATH, dir);
    return dir;
}

bool processAlive(DWORD pid) {
    HANDLE h = OpenProcess(SYNCHRONIZE, FALSE, pid);
    if (!h) return GetLastError() == ERROR_ACCESS_DENIED;
    bool alive = WaitForSingleObject(h, 0) == WAIT_TIMEOUT;
    CloseHandle(h);
    return alive;
}

// 上次當掉留下的根目錄（行程已經不在的）清掉。
void sweepDeadRoots() {
    std::wstring tmp = systemTemp();
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((tmp + kRootPrefix + L"*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
        DWORD pid = wcstoul(fd.cFileName + wcslen(kRootPrefix), nullptr, 10);
        if (pid != 0 && pid != GetCurrentProcessId() && !processAlive(pid)) removeDirectoryRecursive(tmp + fd.cFileName);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

// 一個 test case 跑超過時間上限就結束行程。Catch2 自己沒有 timeout，卡住的測試會讓 CI 等到整個 job 逾時。
int timeoutSec() {
    wchar_t buf[16];
    DWORD n = GetEnvironmentVariableW(L"MP_TEST_TIMEOUT_SEC", buf, 16);
    int v = (n > 0 && n < 16) ? _wtoi(buf) : 0;
    return v > 0 ? v : kTestCaseTimeoutSec;
}

class Watchdog {
public:
    Watchdog() : limit_(timeoutSec()), th_([this] { run(); }) {}
    ~Watchdog() {
        {
            std::lock_guard<std::mutex> lk(m_);
            quit_ = true;
        }
        cv_.notify_all();
        th_.join();
    }
    void arm(std::string name) {
        std::lock_guard<std::mutex> lk(m_);
        name_ = std::move(name);
        deadline_ = std::chrono::steady_clock::now() + std::chrono::seconds(limit_);
        armed_ = true;
        cv_.notify_all();
    }
    void disarm() {
        std::lock_guard<std::mutex> lk(m_);
        armed_ = false;
        cv_.notify_all();
    }

private:
    void run() {
        std::unique_lock<std::mutex> lk(m_);
        while (!quit_) {
            if (!armed_) {
                cv_.wait(lk);
                continue;
            }
            if (cv_.wait_until(lk, deadline_) == std::cv_status::timeout && armed_ &&
                std::chrono::steady_clock::now() >= deadline_) {
                std::fprintf(stderr, "\nTIMEOUT: test case ran longer than %d s: %s\n", limit_, name_.c_str());
                std::fflush(stderr);
                TerminateProcess(GetCurrentProcess(), 3);
            }
        }
    }
    const int limit_;
    std::mutex m_;
    std::condition_variable cv_;
    std::chrono::steady_clock::time_point deadline_;
    std::string name_;
    bool armed_ = false;
    bool quit_ = false;
    std::thread th_;
};

class Listener : public Catch::EventListenerBase {
public:
    using Catch::EventListenerBase::EventListenerBase;
    void testRunStarting(const Catch::TestRunInfo&) override { sweepDeadRoots(); }
    void testCaseStarting(const Catch::TestCaseInfo& info) override {
        // 行程全域的狀態：每個 test case 都從空的語系、沒有 plugin、沒有當掉紀錄開始，不靠別的測試收尾。
        setActiveLang({});
        setActivePlugins(nullptr);
        stopPluginRunLogForTest(true);
        dog_.arm(info.name);
    }
    void testCaseEnded(const Catch::TestCaseStats&) override { dog_.disarm(); }
    void testRunEnded(const Catch::TestRunStats&) override { removeDirectoryRecursive(tempRoot()); }

private:
    Watchdog dog_;
};
CATCH_REGISTER_LISTENER(Listener)

// 驗證看門狗用，平常不跑（標籤以 . 開頭）：MP_TEST_TIMEOUT_SEC=2 mp_tests "[.hang]" 應該印出 TIMEOUT、結束碼 3。
TEST_CASE("test_util：卡住的 test case 會被看門狗結束", "[.hang]") {
    std::this_thread::sleep_for(std::chrono::hours(1));
}

} // namespace

std::wstring exeDir() {
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring p = exe;
    return p.substr(0, p.find_last_of(L"\\/") + 1);
}

std::wstring dllPath() { return exeDir() + L"7z.dll"; }

void requireArchiveLibrary() {
    if (archiveLibraryLoaded()) return;
    if (!exists(dllPath())) FAIL("7z.dll is not next to mp_tests.exe. Configure with -DMP_7Z_DLL=<path to the official 7z.dll>.");
    REQUIRE(loadArchiveLibrary(dllPath()));
}

std::wstring fixturePath(const wchar_t* name) {
    std::wstring beside = exeDir() + L"fixtures\\" + name;
    if (exists(beside)) return beside;
    return std::wstring(MP_TEST_FIXTURES) + L"/" + name;
}

const std::wstring& tempRoot() {
    static const std::wstring root = [] {
        std::wstring r = systemTemp() + kRootPrefix + std::to_wstring(GetCurrentProcessId());
        removeDirectoryRecursive(r);
        createDirectories(r);
        return r;
    }();
    return root;
}

std::wstring tempPath(const wchar_t* name) { return tempRoot() + L"\\" + name; }

std::wstring freshDir(const wchar_t* name) {
    std::wstring dir = tempPath(name);
    removeDirectoryRecursive(dir);
    REQUIRE(createDirectories(dir));
    return dir;
}

bool exists(const std::wstring& path) { return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES; }

std::string readBytes(const std::wstring& path) {
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    REQUIRE(f != INVALID_HANDLE_VALUE);
    std::string bytes;
    char buf[4096];
    DWORD got = 0;
    while (ReadFile(f, buf, sizeof(buf), &got, nullptr) && got > 0) bytes.append(buf, got);
    CloseHandle(f);
    return bytes;
}

void writeBytes(const std::wstring& path, const std::string& bytes) {
    size_t slash = path.find_last_of(L"\\/");
    if (slash != std::wstring::npos) REQUIRE(createDirectories(path.substr(0, slash)));
    HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    REQUIRE(f != INVALID_HANDLE_VALUE);
    DWORD w = 0;
    BOOL ok = WriteFile(f, bytes.data(), static_cast<DWORD>(bytes.size()), &w, nullptr);
    CloseHandle(f);
    REQUIRE(ok);
    REQUIRE(w == bytes.size());
}

Image solidImage(int w, int h, uint8_t b, uint8_t g, uint8_t r) {
    Image img(w, h);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            uint8_t* p = img.px(x, y);
            p[0] = b;
            p[1] = g;
            p[2] = r;
            p[3] = 255;
        }
    return img;
}

void writePng(const std::wstring& path, int w, int h) {
    size_t slash = path.find_last_of(L"\\/");
    if (slash != std::wstring::npos) REQUIRE(createDirectories(path.substr(0, slash)));
    REQUIRE(saveImage(solidImage(w, h), path));
}

std::string pngBytes(int w, int h) {
    std::wstring tmp = tempPath(L"png_bytes.png");
    writePng(tmp, w, h);
    std::string bytes = readBytes(tmp);
    DeleteFileW(tmp.c_str());
    return bytes;
}

std::vector<std::wstring> listImagesRecursive(const std::wstring& dir) {
    std::vector<std::wstring> out;
    forEachDirEntry(dir, [&](const DirEntry& e) {
        std::wstring p = joinPath(dir, e.name);
        if (e.directory) {
            auto sub = listImagesRecursive(p);
            out.insert(out.end(), sub.begin(), sub.end());
        } else if (isSupportedExtension(extensionOf(e.name))) {
            out.push_back(p);
        }
        return true;
    });
    sortLogical(out);
    return out;
}

LangGuard::LangGuard() { setActiveLang({}); }
LangGuard::~LangGuard() { setActiveLang({}); }

bit7z::Bit7zLibrary& sevenZip() {
    if (!exists(dllPath())) FAIL("7z.dll is not next to mp_tests.exe. Configure with -DMP_7Z_DLL=<path to the official 7z.dll>.");
    static bit7z::Bit7zLibrary lib(bit7z::to_tstring(dllPath()));
    return lib;
}

void makeArchive(const std::wstring& out, const bit7z::BitInOutFormat& fmt, const std::vector<ArchiveEntry>& entries,
                 const MakeArchiveOptions& mo) {
    bit7z::BitArchiveWriter w(sevenZip(), fmt);
    if (!mo.password.empty())
        w.setPassword(mo.password, mo.cryptHeaders ? bit7z::EncryptionScope::DataAndHeaders : bit7z::EncryptionScope::DataOnly);
    w.setSolidMode(mo.solid);
    std::vector<bit7z::buffer_t> buffers; // addFile 只存參考，buffer 要活到 compressTo 之後
    buffers.reserve(entries.size());
    for (const auto& e : entries) {
        buffers.emplace_back(e.content.begin(), e.content.end());
        w.addFile(buffers.back(), e.path);
    }
    w.compressTo(bit7z::to_tstring(out));
}

} // namespace mp::test
