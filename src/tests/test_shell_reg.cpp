#include <catch2/catch_test_macros.hpp>

#include "core/FolderNav.h"
#include "core/ShellReg.h"

using namespace mp;

namespace {
// 全部寫到 HKCU\Software\mpainter-test 底下，測完刪掉，不碰真的 Classes。
ShellRegOptions testOptions() {
    ShellRegOptions o;
    o.classesBase = L"Software\\mpainter-test\\Classes";
    o.appBase = L"Software\\mpainter-test\\App";
    o.registeredApps = L"Software\\mpainter-test\\RegisteredApplications";
    return o;
}
void cleanup() { RegDeleteTreeW(HKEY_CURRENT_USER, L"Software\\mpainter-test"); }

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
    cleanup();
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
    cleanup();
}

TEST_CASE("ProgID 登錄、查詢、移除") {
    cleanup();
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
    cleanup();
}

TEST_CASE("isWindows11 不會炸") {
    bool v = isWindows11();
    CHECK((v == true || v == false));
}
