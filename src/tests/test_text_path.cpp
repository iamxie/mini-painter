// TextUtil、PathUtil、TextFile、SysInfo、IoError 的測試。
#include <catch2/catch_test_macros.hpp>

#include <windows.h>

#include <algorithm>
#include <string>
#include <vector>

#include "core/IoError.h"
#include "core/PathUtil.h"
#include "core/SysInfo.h"
#include "core/TempDir.h"
#include "core/TextFile.h"
#include "core/TextUtil.h"
#include "test_util.h"

using namespace mp;

TEST_CASE("utf8ToWide / wideToUtf8：往返、空字串、中間的 NUL") {
    CHECK(utf8ToWide("").empty());
    CHECK(wideToUtf8(L"").empty());
    CHECK(utf8ToWide("abc") == L"abc");
    CHECK(utf8ToWide("\xE4\xB8\xAD\xE6\x96\x87") == L"中文");
    CHECK(wideToUtf8(L"中文") == "\xE4\xB8\xAD\xE6\x96\x87");
    std::wstring emoji = L"a\U0001F600z"; // 代理對
    CHECK(utf8ToWide(wideToUtf8(emoji)) == emoji);
    std::string withNul("a\0b", 3);
    CHECK(utf8ToWide(withNul) == std::wstring(L"a\0b", 3));
    CHECK(utf8ToWide("\xFF") == L"\xFFFD"); // 不合法的位元組換成 U+FFFD
}

TEST_CASE("trim / lowerCopy") {
    CHECK(trim(L"  a b \t\r\n") == L"a b");
    CHECK(trim(L"").empty());
    CHECK(trim(L" \t\r\n").empty());
    CHECK(trim(L"x") == L"x");
    CHECK(lowerCopy(L"AbC.JPG") == L"abc.jpg");
    CHECK(lowerCopy(L"").empty());
}

TEST_CASE("joinPath / extensionOf") {
    CHECK(joinPath(L"C:\\a", L"b.txt") == L"C:\\a\\b.txt");
    CHECK(joinPath(L"C:\\a\\", L"b.txt") == L"C:\\a\\b.txt");
    CHECK(joinPath(L"C:/a/", L"b.txt") == L"C:/a/b.txt");
    CHECK(joinPath(L"", L"b.txt") == L"b.txt");
    CHECK(extensionOf(L"C:\\a\\b.JPG") == L".jpg");
    CHECK(extensionOf(L"b.tar.gz") == L".gz");
    CHECK(extensionOf(L"C:\\a.b\\noext").empty()); // 點在資料夾名裡
    CHECK(extensionOf(L"noext").empty());
    CHECK(extensionOf(L"").empty());
    CHECK(extensionOf(L".hidden") == L".hidden");
}

TEST_CASE("isDirectoryPath") {
    CHECK(isDirectoryPath(mp::test::tempRoot()));
    CHECK_FALSE(isDirectoryPath(mp::test::tempPath(L"no_such_dir_xyz")));
    std::wstring f = mp::test::tempPath(L"isdir_file.tmp");
    mp::test::writeBytes(f, "x");
    CHECK_FALSE(isDirectoryPath(f));
    DeleteFileW(f.c_str());
}

TEST_CASE("forEachDirEntry：不含 . 與 ..，帶大小與屬性，樣式篩選，回 false 就停") {
    std::wstring dir = mp::test::freshDir(L"dirent");
    mp::test::writeBytes(dir + L"\\a.txt", "12345");
    mp::test::writeBytes(dir + L"\\b.old", "1");
    mp::test::writeBytes(dir + L"\\ro.txt", "12");
    SetFileAttributesW((dir + L"\\ro.txt").c_str(), FILE_ATTRIBUTE_READONLY);
    REQUIRE(createDirectories(dir + L"\\sub"));

    std::vector<DirEntry> all;
    forEachDirEntry(dir, [&](const DirEntry& e) { all.push_back(e); return true; });
    REQUIRE(all.size() == 4);
    auto find = [&](const wchar_t* name) {
        auto it = std::find_if(all.begin(), all.end(), [&](const DirEntry& e) { return e.name == name; });
        REQUIRE(it != all.end());
        return *it;
    };
    CHECK(find(L"a.txt").size == 5);
    CHECK_FALSE(find(L"a.txt").directory);
    CHECK_FALSE(find(L"a.txt").readOnly);
    CHECK(find(L"ro.txt").readOnly);
    CHECK(find(L"sub").directory);
    CHECK(find(L"sub").size == 0);
    CHECK_FALSE(find(L"sub").reparsePoint);

    int txt = 0;
    forEachDirEntry(dir, [&](const DirEntry&) { ++txt; return true; }, L"*.txt");
    CHECK(txt == 2);
    int visited = 0;
    forEachDirEntry(dir, [&](const DirEntry&) { ++visited; return false; });
    CHECK(visited == 1);
    int none = 0;
    forEachDirEntry(dir + L"\\nowhere", [&](const DirEntry&) { ++none; return true; });
    CHECK(none == 0);
    CHECK(removeDirectoryRecursive(dir));
}

TEST_CASE("forEachDirEntry：連結點標成 reparsePoint，directorySize 與 removeDirectoryRecursive 不跟進去") {
    std::wstring dir = mp::test::freshDir(L"junction");
    std::wstring target = mp::test::freshDir(L"junction_target");
    mp::test::writeBytes(target + L"\\keep.txt", "1234567");
    mp::test::writeBytes(dir + L"\\own.txt", "123");
    // mklink /J 不需要管理員權限。
    std::wstring cmd = L"cmd /c mklink /J \"" + dir + L"\\link\" \"" + target + L"\" >nul";
    STARTUPINFOW si{sizeof(si)};
    PROCESS_INFORMATION pi{};
    REQUIRE(CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi));
    WaitForSingleObject(pi.hProcess, 10000);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    REQUIRE(mp::test::exists(dir + L"\\link\\keep.txt"));

    bool sawLink = false;
    forEachDirEntry(dir, [&](const DirEntry& e) {
        if (e.name == L"link") sawLink = e.directory && e.reparsePoint;
        return true;
    });
    CHECK(sawLink);
    CHECK(directorySize(dir) == 3); // 連結另一頭的 7 bytes 不算
    CHECK(removeDirectoryRecursive(dir));
    CHECK(mp::test::exists(target + L"\\keep.txt")); // 只拆連結，另一頭的檔還在
}

TEST_CASE("readFileBytes / readUtf8File：分得出不存在與讀不到；BOM 去掉") {
    std::wstring p = mp::test::tempPath(L"text.txt");
    std::string bytes;
    std::wstring text;
    DeleteFileW(p.c_str());
    CHECK(readFileBytes(p, bytes) == FileRead::Missing);
    CHECK(readUtf8File(p, text) == FileRead::Missing);
    CHECK(readUtf8File(mp::test::tempPath(L"no_dir\\x.txt"), text) == FileRead::Missing);

    mp::test::writeBytes(p, "\xEF\xBB\xBF" "a=\xE4\xB8\xAD\r\n");
    CHECK(readFileBytes(p, bytes) == FileRead::Ok);
    CHECK(bytes.size() == 10); // 原樣，含 BOM
    CHECK(readUtf8File(p, text) == FileRead::Ok);
    CHECK(text == L"a=中\r\n");
    mp::test::writeBytes(p, "");
    CHECK(readUtf8File(p, text) == FileRead::Ok);
    CHECK(text.empty());
    // 超過一次讀取的緩衝區
    mp::test::writeBytes(p, std::string(20000, 'x'));
    CHECK(readFileBytes(p, bytes) == FileRead::Ok);
    CHECK(bytes.size() == 20000);

    // 被別的程式獨佔：檔案在，但讀不到。
    HANDLE h = CreateFileW(p.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    REQUIRE(h != INVALID_HANDLE_VALUE);
    CHECK(readFileBytes(p, bytes) == FileRead::Unreadable);
    CHECK(bytes.empty());
    CHECK(readUtf8File(p, text) == FileRead::Unreadable);
    CloseHandle(h);
    DeleteFileW(p.c_str());
}

TEST_CASE("windowsBuildNumber / isWindows11：和系統登錄的 build 號同一個世代") {
    unsigned build = windowsBuildNumber();
    // Windows 10 的第一版是 10240。讀到修訂號（幾百到幾千）的話這裡會失敗。
    CHECK(build >= 10240);
    wchar_t buf[32];
    DWORD size = sizeof(buf);
    REQUIRE(RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion", L"CurrentBuildNumber",
                         RRF_RT_REG_SZ, nullptr, buf, &size) == ERROR_SUCCESS);
    unsigned osBuild = wcstoul(buf, nullptr, 10);
    // kernel32 的檔案版本停在那一代的基底（19045 的系統是 19041），不會剛好相等，但不會差超過一代。
    CHECK(build <= osBuild);
    CHECK(osBuild - build < 1000);
    CHECK(isWindows11() == (osBuild >= 22000));
}

TEST_CASE("ioErrorMessage：每個代碼都有說明，只有 UnsupportedSaveFormat 與 PluginFailed 帶 %s") {
    CHECK(std::wstring(ioErrorMessage(IoErrorCode::None)).empty());
    for (int c = static_cast<int>(IoErrorCode::FileUnreadable); c <= static_cast<int>(IoErrorCode::ReplaceFailed); ++c) {
        const auto code = static_cast<IoErrorCode>(c);
        std::wstring m = ioErrorMessage(code);
        INFO("code " << c);
        CHECK_FALSE(m.empty());
        CHECK((m.find(L'%') != std::wstring::npos) == (code == IoErrorCode::UnsupportedSaveFormat || code == IoErrorCode::PluginFailed));
    }
    IoError e;
    CHECK(e.ok());
    e.code = IoErrorCode::DecodeFailed;
    CHECK_FALSE(e.ok());
}

TEST_CASE("clampedIntFromText：夾進範圍、不是數字回 fallback、位數太多照正負號夾") {
    CHECK(clampedIntFromText(L"20", 99, 1, 999) == 20);
    CHECK(clampedIntFromText(L" 20", 99, 1, 999) == 20);
    CHECK(clampedIntFromText(L"0", 99, 1, 999) == 1);
    CHECK(clampedIntFromText(L"5000", 99, 1, 999) == 999);
    CHECK(clampedIntFromText(L"-4", 99, 1, 999) == 1);
    CHECK(clampedIntFromText(L"", 99, 1, 999) == 99);
    CHECK(clampedIntFromText(L"abc", 99, 1, 999) == 99);
    CHECK(clampedIntFromText(L"12abc", 99, 1, 999) == 12); // stoi 的規則：只取開頭的數字
    CHECK(clampedIntFromText(L"99999999999999999999", 7, 1, 999) == 999);
    CHECK(clampedIntFromText(L"-99999999999999999999", 7, 1, 999) == 1);
}
