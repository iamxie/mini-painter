#include <catch2/catch_test_macros.hpp>

#include <windows.h>

#include "core/TempDir.h"

using namespace mp;

namespace {
std::wstring testRoot(const wchar_t* name) {
    wchar_t dir[MAX_PATH];
    GetTempPathW(MAX_PATH, dir);
    return std::wstring(dir) + L"mp_test_unzip_" + name;
}
void writeFile(const std::wstring& path, const char* text) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    REQUIRE(h != INVALID_HANDLE_VALUE);
    DWORD w = 0;
    WriteFile(h, text, static_cast<DWORD>(strlen(text)), &w, nullptr);
    CloseHandle(h);
}
bool exists(const std::wstring& path) { return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES; }
} // namespace

TEST_CASE("defaultUnzipRoot / fallbackUnzipRoot / resolveUnzipRoot") {
    CHECK(defaultUnzipRoot(L"C:\\Tools") == L"C:\\Tools\\temp");
    CHECK(defaultUnzipRoot(L"C:\\Tools\\") == L"C:\\Tools\\temp");
    CHECK(defaultUnzipRoot(L"") == L".\\temp");
    std::wstring f = fallbackUnzipRoot();
    CHECK(f.find(L"mpainter\\unzip") != std::wstring::npos);
    CHECK(f != defaultUnzipRoot(L"C:\\Tools"));
    CHECK(resolveUnzipRoot(L"", L"C:\\Tools") == L"C:\\Tools\\temp");
    CHECK(resolveUnzipRoot(L"   ", L"C:\\Tools") == L"C:\\Tools\\temp");
    CHECK(resolveUnzipRoot(L" D:\\tmp\\x ", L"C:\\Tools") == L"D:\\tmp\\x");
}

TEST_CASE("directorySize：遞迴加總，不存在回 0") {
    std::wstring root = testRoot(L"size");
    removeDirectoryRecursive(root);
    CHECK(directorySize(root) == 0);
    REQUIRE(createDirectories(root + L"\\a\\b"));
    writeFile(root + L"\\one.txt", "12345");
    writeFile(root + L"\\a\\two.txt", "123");
    writeFile(root + L"\\a\\b\\three.txt", "1234567");
    CHECK(directorySize(root) == 15);
    CHECK(directorySize(root + L"\\a") == 10);
    removeDirectoryRecursive(root);
}

TEST_CASE("createDirectories / removeDirectoryRecursive：多層、唯讀檔、不存在") {
    std::wstring root = testRoot(L"rm");
    removeDirectoryRecursive(root);
    REQUIRE(createDirectories(root + L"\\a\\b"));
    REQUIRE(createDirectories(root + L"\\a\\b")); // 已存在也成功
    writeFile(root + L"\\a\\b\\f.txt", "x");
    writeFile(root + L"\\a\\ro.txt", "x");
    SetFileAttributesW((root + L"\\a\\ro.txt").c_str(), FILE_ATTRIBUTE_READONLY);
    REQUIRE(removeDirectoryRecursive(root));
    CHECK_FALSE(exists(root));
    CHECK(removeDirectoryRecursive(root)); // 不存在算成功
}

TEST_CASE("InstanceTempDir：建立帶 lock 的子資料夾，release 後整個消失") {
    std::wstring root = testRoot(L"inst");
    removeDirectoryRecursive(root);
    std::wstring kept;
    {
        InstanceTempDir t;
        REQUIRE(t.create(root));
        CHECK(t.valid());
        kept = t.path();
        CHECK(kept.rfind(root + L"\\", 0) == 0);
        CHECK(exists(kept + L"\\" + kTempLockName));
        writeFile(kept + L"\\001.jpg", "x");
        // 另一個 handle 開不起來 lock
        HANDLE h = CreateFileW((kept + L"\\" + kTempLockName).c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, 0, nullptr);
        CHECK(h == INVALID_HANDLE_VALUE);
        CHECK(GetLastError() == ERROR_SHARING_VIOLATION);
        // 兩個執行個體可以共存
        InstanceTempDir t2;
        REQUIRE(t2.create(root));
        CHECK(t2.path() != kept);
    }
    CHECK_FALSE(exists(kept));
    removeDirectoryRecursive(root);
}

TEST_CASE("sweepStaleTempDirs：活的留著，沒 lock 或 lock 沒人持有的刪掉") {
    std::wstring root = testRoot(L"sweep");
    removeDirectoryRecursive(root);
    REQUIRE(createDirectories(root));
    // 1. 活的：lock 被我們持有
    InstanceTempDir alive;
    REQUIRE(alive.create(root));
    writeFile(alive.path() + L"\\page.jpg", "x");
    // 2. 當掉留下的：有檔案、沒 lock（DELETE_ON_CLOSE 已把它刪掉）
    std::wstring crashed = root + L"\\1234-deadbeef";
    REQUIRE(createDirectories(crashed + L"\\sub"));
    writeFile(crashed + L"\\sub\\page.jpg", "x");
    // 3. lock 檔還在但沒人開著（例如硬斷電後留下）
    std::wstring orphan = root + L"\\5678-cafebabe";
    REQUIRE(createDirectories(orphan));
    writeFile(orphan + L"\\" + kTempLockName, "");
    writeFile(orphan + L"\\page.jpg", "x");
    // root 底下的散檔不動
    writeFile(root + L"\\stray.txt", "x");

    CHECK(sweepStaleTempDirs(root) == 2);
    CHECK(exists(alive.path() + L"\\page.jpg"));
    CHECK_FALSE(exists(crashed));
    CHECK_FALSE(exists(orphan));
    CHECK(exists(root + L"\\stray.txt"));
    CHECK(sweepStaleTempDirs(root) == 0);
    alive.release();
    CHECK_FALSE(exists(alive.path()));
    removeDirectoryRecursive(root);
    CHECK(sweepStaleTempDirs(root) == 0); // root 不存在
}
