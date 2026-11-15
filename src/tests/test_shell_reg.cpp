#include <catch2/catch_test_macros.hpp>

#include <windows.h>

#include <string>

#include "core/FolderNav.h"
#include "core/ShellReg.h"

using namespace mp;

namespace {
// 全部寫到 HKCU\Software\mpainter-test-<pid> 底下，不碰真的 Classes。
std::wstring testBase() { return L"Software\\mpainter-test-" + std::to_wstring(GetCurrentProcessId()); }
ShellRegOptions testOptions() {
    ShellRegOptions o;
    o.classesBase = testBase() + L"\\Classes";
    o.appBase = testBase() + L"\\App";
    o.registeredApps = testBase() + L"\\RegisteredApplications";
    return o;
}
// 進出 scope 都刪掉整棵測試用的機碼：REQUIRE 失敗也不會留在登錄裡。
struct RegCleanup {
    RegCleanup() { RegDeleteTreeW(HKEY_CURRENT_USER, testBase().c_str()); }
    ~RegCleanup() { RegDeleteTreeW(HKEY_CURRENT_USER, testBase().c_str()); }
};

std::wstring readValue(const std::wstring& key, const wchar_t* name) {
    wchar_t buf[1024];
    DWORD size = sizeof(buf);
    if (RegGetValueW(HKEY_CURRENT_USER, key.c_str(), name, RRF_RT_REG_SZ, nullptr, buf, &size) != ERROR_SUCCESS) return L"";
    return buf;
}
} // namespace

TEST_CASE("exePathFromCommand") {
    CHECK(exePathFromCommand(L"\"C:\\a b\\mpainter.exe\" \"%1\"") == L"C:\\a b\\mpainter.exe");
    CHECK(exePathFromCommand(L"C:\\x\\mpainter.exe %1") == L"C:\\x\\mpainter.exe");
    CHECK(exePathFromCommand(L"") == L"");
}

TEST_CASE("右鍵選單登錄、查詢、移除") {
    RegCleanup guard;
    auto o = testOptions();
    CHECK(contextMenuExePath(o) == L"");
    REQUIRE(registerContextMenu(L"C:\\Tools\\mpainter.exe", o));
    CHECK(contextMenuExePath(o) == L"C:\\Tools\\mpainter.exe");
    // 每個副檔名和資料夾都有
    for (const auto& ext : supportedExtensions()) {
        std::wstring k = o.classesBase + L"\\SystemFileAssociations\\" + ext + L"\\shell\\mpainter";
        CHECK(readValue(k, nullptr) == L"Open with mpainter"); // 測試沒載語系，是英文 key
        CHECK(readValue(k + L"\\command", nullptr) == L"\"C:\\Tools\\mpainter.exe\" \"%1\"");
    }
    CHECK(readValue(o.classesBase + L"\\Directory\\shell\\mpainter\\command", nullptr) == L"\"C:\\Tools\\mpainter.exe\" \"%1\"");
    // 壓縮檔也有右鍵
    for (const auto& ext : archiveExtensions()) {
        std::wstring k = o.classesBase + L"\\SystemFileAssociations\\" + ext + L"\\shell\\mpainter";
        CHECK(readValue(k + L"\\command", nullptr) == L"\"C:\\Tools\\mpainter.exe\" \"%1\"");
    }
    // 換路徑會覆寫
    REQUIRE(registerContextMenu(L"D:\\p\\mpainter.exe", o));
    CHECK(contextMenuExePath(o) == L"D:\\p\\mpainter.exe");
    REQUIRE(unregisterContextMenu(o));
    CHECK(contextMenuExePath(o) == L"");
    CHECK(unregisterContextMenu(o)); // 再移除一次也成功
}

TEST_CASE("ProgID 登錄、查詢、移除") {
    RegCleanup guard;
    auto o = testOptions();
    CHECK(progIdExePath(o) == L"");
    REQUIRE(registerProgId(L"C:\\Tools\\mpainter.exe", o));
    CHECK(progIdExePath(o) == L"C:\\Tools\\mpainter.exe");
    CHECK(readValue(o.registeredApps, L"mpainter") == o.appBase + L"\\Capabilities");
    CHECK(readValue(o.appBase + L"\\Capabilities\\FileAssociations", L".jpg") == L"mpainter.Image");
    CHECK(readValue(o.classesBase + L"\\.png\\OpenWithProgids", L"mpainter.Image") == L"");
    // 漫畫壓縮檔登錄預設程式；一般 zip / 7z / rar 不登錄
    CHECK(readValue(o.appBase + L"\\Capabilities\\FileAssociations", L".cbz") == L"mpainter.Image");
    CHECK(readValue(o.appBase + L"\\Capabilities\\FileAssociations", L".zip") == L"");
    HKEY h = nullptr;
    CHECK(RegOpenKeyExW(HKEY_CURRENT_USER, (o.classesBase + L"\\.png\\OpenWithProgids").c_str(), 0, KEY_READ, &h) == ERROR_SUCCESS);
    RegCloseKey(h);
    REQUIRE(unregisterProgId(o));
    CHECK(progIdExePath(o) == L"");
    CHECK(readValue(o.registeredApps, L"mpainter") == L"");
    CHECK(RegOpenKeyExW(HKEY_CURRENT_USER, (o.classesBase + L"\\.cbz\\OpenWithProgids").c_str(), 0, KEY_READ, &h) == ERROR_SUCCESS);
    CHECK(readValue(o.classesBase + L"\\.cbz\\OpenWithProgids", L"mpainter.Image") == L"");
    RegCloseKey(h);
}

TEST_CASE("B-21：contextMenuUpToDate 看路徑、文字、每個副檔名的機碼") {
    RegCleanup guard;
    auto o = testOptions();
    const std::wstring exe = L"C:\\Tools\\mpainter.exe";
    CHECK_FALSE(contextMenuUpToDate(exe, o)); // 還沒登錄
    REQUIRE(registerContextMenu(exe, o));
    CHECK(contextMenuUpToDate(exe, o));
    CHECK(contextMenuUpToDate(L"c:\\tools\\MPAINTER.EXE", o) == false); // Icon 值逐字比：大小寫不同就重登，結果一樣是對的
    // exe 搬了位置
    CHECK_FALSE(contextMenuUpToDate(L"D:\\p\\mpainter.exe", o));
    // 換了語言：v0.9.5 只比路徑，選單文字停在舊語言
    auto zh = o;
    zh.text.openWith = L"用 mpainter 開啟";
    CHECK_FALSE(contextMenuUpToDate(exe, zh));
    REQUIRE(registerContextMenu(exe, zh));
    CHECK(contextMenuUpToDate(exe, zh));
    CHECK(readValue(o.classesBase + L"\\SystemFileAssociations\\.png\\shell\\mpainter", nullptr) == L"用 mpainter 開啟");
    // 舊版登錄時還沒有的副檔名（這裡刪掉一個模擬）：v0.9.5 不會補
    REQUIRE(RegDeleteTreeW(HKEY_CURRENT_USER, (o.classesBase + L"\\SystemFileAssociations\\.webp\\shell\\mpainter").c_str()) == ERROR_SUCCESS);
    CHECK_FALSE(contextMenuUpToDate(exe, zh));
    REQUIRE(registerContextMenu(exe, zh));
    CHECK(contextMenuUpToDate(exe, zh));
}

TEST_CASE("B-21：progIdUpToDate；Applications 底下用 exe 實際的檔名，改名後舊的那一個刪掉") {
    RegCleanup guard;
    auto o = testOptions();
    const std::wstring exe = L"C:\\Tools\\mpainter.exe";
    CHECK_FALSE(progIdUpToDate(exe, o));
    REQUIRE(registerProgId(exe, o));
    CHECK(progIdUpToDate(exe, o));
    CHECK(readValue(o.classesBase + L"\\Applications\\mpainter.exe\\shell\\open\\command", nullptr) == L"\"C:\\Tools\\mpainter.exe\" \"%1\"");
    // 換了語言
    auto zh = o;
    zh.text.fileType = L"mpainter 影像";
    CHECK_FALSE(progIdUpToDate(exe, zh));
    REQUIRE(registerProgId(exe, zh));
    CHECK(progIdUpToDate(exe, zh));
    CHECK(readValue(o.classesBase + L"\\mpainter.Image", L"FriendlyTypeName") == L"mpainter 影像");
    // 少了一個副檔名的關聯
    HKEY h = nullptr;
    REQUIRE(RegOpenKeyExW(HKEY_CURRENT_USER, (o.appBase + L"\\Capabilities\\FileAssociations").c_str(), 0, KEY_WRITE, &h) == ERROR_SUCCESS);
    REQUIRE(RegDeleteValueW(h, L".webp") == ERROR_SUCCESS);
    RegCloseKey(h);
    CHECK_FALSE(progIdUpToDate(exe, zh));
    REQUIRE(registerProgId(exe, zh));
    CHECK(progIdUpToDate(exe, zh));
    // exe 被改名：登錄在新檔名底下，舊檔名的機碼不留（v0.9.5 一律寫 Applications\mpainter.exe）
    const std::wstring renamed = L"C:\\Tools\\viewer.exe";
    CHECK_FALSE(progIdUpToDate(renamed, zh));
    REQUIRE(registerProgId(renamed, zh));
    CHECK(progIdUpToDate(renamed, zh));
    CHECK(readValue(o.classesBase + L"\\Applications\\viewer.exe\\shell\\open\\command", nullptr) == L"\"C:\\Tools\\viewer.exe\" \"%1\"");
    CHECK(readValue(o.classesBase + L"\\Applications\\mpainter.exe\\shell\\open\\command", nullptr) == L"");
    REQUIRE(unregisterProgId(zh));
    CHECK(readValue(o.classesBase + L"\\Applications\\viewer.exe\\shell\\open\\command", nullptr) == L"");
    CHECK(progIdExePath(zh) == L"");
}
