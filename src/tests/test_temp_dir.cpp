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

TEST_CASE("isInstanceTempDirName：只認 <pid>-<8 位十六進位>") {
    CHECK(isInstanceTempDirName(L"1234-deadbeef"));
    CHECK(isInstanceTempDirName(L"7-0000000a"));
    CHECK(isInstanceTempDirName(L"4294967295-DEADBEEF"));
    for (const wchar_t* bad : {L"", L"Photos", L"2024", L"1234", L"-deadbeef", L"1234-", L"1234-deadbee", L"1234-deadbeef0",
                               L"1234-deadbeeg", L"12a4-deadbeef", L"1234_deadbeef", L"1234-dead-eef", L" 1234-deadbeef"}) {
        CAPTURE(std::wstring(bad).size());
        CHECK_FALSE(isInstanceTempDirName(bad));
    }
    // create 建出來的名稱一定要認得，否則當掉後留下的東西永遠清不掉
    std::wstring root = testRoot(L"name");
    removeDirectoryRecursive(root);
    InstanceTempDir t;
    REQUIRE(t.create(root));
    CHECK(isInstanceTempDirName(t.path().substr(t.path().find_last_of(L'\\') + 1)));
    t.release();
    removeDirectoryRecursive(root);
}

TEST_CASE("sweepStaleTempDirs：不是 mpainter 建的子資料夾不動") {
    // 使用者把暫存位置指到既有的資料夾：裡面本來就有別的東西，都沒有 lock 檔。
    std::wstring root = testRoot(L"sweep_foreign");
    removeDirectoryRecursive(root);
    REQUIRE(createDirectories(root));
    const wchar_t* foreign[] = {L"Photos", L"2024", L"1234-deadbeef-old", L"abc-12345678", L"1234-deadbee", L"1234_deadbeef",
                                L"-deadbeef", L"1234-deadbeeg"};
    for (const wchar_t* name : foreign) {
        REQUIRE(createDirectories(root + L"\\" + name + L"\\sub"));
        writeFile(root + L"\\" + name + L"\\sub\\keep.txt", "x");
    }
    // 名稱像，但 lock 開不起來的原因既不是「不存在」也不是「有人持有」（這裡 lock 是個資料夾）：不確定就不刪。
    std::wstring odd = root + L"\\4321-0badf00d";
    REQUIRE(createDirectories(odd + L"\\" + kTempLockName));
    writeFile(odd + L"\\page.jpg", "x");

    CHECK(sweepStaleTempDirs(root) == 0);
    for (size_t i = 0; i < std::size(foreign); ++i) {
        CAPTURE(i);
        CHECK(exists(root + L"\\" + foreign[i] + L"\\sub\\keep.txt"));
    }
    CHECK(exists(odd + L"\\page.jpg"));
    removeDirectoryRecursive(root);
}
