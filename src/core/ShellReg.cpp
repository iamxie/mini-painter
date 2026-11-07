#include "core/ShellReg.h"

#include <shellapi.h>

#include "core/FolderNav.h"
#include "core/Lang.h"

namespace mp {

namespace {

constexpr wchar_t kVerb[] = L"mpainter";
constexpr wchar_t kProgId[] = L"mpainter.Image";
constexpr wchar_t kAppName[] = L"mpainter";

std::wstring quotedCommand(const std::wstring& exe) { return L"\"" + exe + L"\" \"%1\""; }

bool setString(HKEY root, const std::wstring& key, const wchar_t* name, const std::wstring& value) {
    HKEY h;
    if (RegCreateKeyExW(root, key.c_str(), 0, nullptr, 0, KEY_WRITE, nullptr, &h, nullptr) != ERROR_SUCCESS) return false;
    LONG r = RegSetValueExW(h, name, 0, REG_SZ, reinterpret_cast<const BYTE*>(value.c_str()),
                            static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t)));
    RegCloseKey(h);
    return r == ERROR_SUCCESS;
}

std::wstring getString(HKEY root, const std::wstring& key, const wchar_t* name) {
    HKEY h;
    if (RegOpenKeyExW(root, key.c_str(), 0, KEY_READ, &h) != ERROR_SUCCESS) return L"";
    wchar_t buf[2048];
    DWORD size = sizeof(buf), type = 0;
    LONG r = RegQueryValueExW(h, name, nullptr, &type, reinterpret_cast<BYTE*>(buf), &size);
    RegCloseKey(h);
    if (r != ERROR_SUCCESS || (type != REG_SZ && type != REG_EXPAND_SZ)) return L"";
    buf[size / sizeof(wchar_t) < 2048 ? size / sizeof(wchar_t) : 2047] = L'\0';
    return buf;
}

bool deleteTree(HKEY root, const std::wstring& key) {
    LONG r = RegDeleteTreeW(root, key.c_str());
    return r == ERROR_SUCCESS || r == ERROR_FILE_NOT_FOUND;
}

bool deleteValue(HKEY root, const std::wstring& key, const wchar_t* name) {
    HKEY h;
    if (RegOpenKeyExW(root, key.c_str(), 0, KEY_WRITE, &h) != ERROR_SUCCESS) return true;
    LONG r = RegDeleteValueW(h, name);
    RegCloseKey(h);
    return r == ERROR_SUCCESS || r == ERROR_FILE_NOT_FOUND;
}

std::vector<std::wstring> contextMenuKeys(const ShellRegOptions& o) {
    std::vector<std::wstring> keys;
    for (const auto& ext : supportedExtensions())
        keys.push_back(o.classesBase + L"\\SystemFileAssociations\\" + ext + L"\\shell\\" + kVerb);
    // 壓縮檔只加右鍵，不登錄預設程式（除了 comicExtensions，見 registerProgId）。
    for (const auto& ext : archiveExtensions())
        keys.push_back(o.classesBase + L"\\SystemFileAssociations\\" + ext + L"\\shell\\" + kVerb);
    keys.push_back(o.classesBase + L"\\Directory\\shell\\" + kVerb);
    return keys;
}

bool writeVerb(const ShellRegOptions& o, const std::wstring& verbKey, const std::wstring& exePath) {
    return setString(o.root, verbKey, nullptr, tr(L"Open with mpainter")) &&
           setString(o.root, verbKey, L"Icon", L"\"" + exePath + L"\",0") &&
           setString(o.root, verbKey + L"\\command", nullptr, quotedCommand(exePath));
}

} // namespace

std::wstring exePathFromCommand(const std::wstring& command) {
    if (command.empty()) return L"";
    if (command[0] == L'"') {
        size_t end = command.find(L'"', 1);
        return end == std::wstring::npos ? command.substr(1) : command.substr(1, end - 1);
    }
    size_t sp = command.find(L' ');
    return sp == std::wstring::npos ? command : command.substr(0, sp);
}

bool registerContextMenu(const std::wstring& exePath, const ShellRegOptions& o) {
    bool ok = true;
    for (const auto& k : contextMenuKeys(o)) ok = writeVerb(o, k, exePath) && ok;
    return ok;
}

bool unregisterContextMenu(const ShellRegOptions& o) {
    bool ok = true;
    for (const auto& k : contextMenuKeys(o)) ok = deleteTree(o.root, k) && ok;
    return ok;
}

std::wstring contextMenuExePath(const ShellRegOptions& o) {
    // 以第一個副檔名為準。
    auto keys = contextMenuKeys(o);
    return exePathFromCommand(getString(o.root, keys.front() + L"\\command", nullptr));
}

bool registerProgId(const std::wstring& exePath, const ShellRegOptions& o) {
    bool ok = true;
    std::wstring progKey = o.classesBase + L"\\" + kProgId;
    ok = setString(o.root, progKey, nullptr, tr(L"mpainter image")) && ok;
    ok = setString(o.root, progKey, L"FriendlyTypeName", tr(L"mpainter image")) && ok;
    ok = setString(o.root, progKey + L"\\DefaultIcon", nullptr, L"\"" + exePath + L"\",0") && ok;
    ok = setString(o.root, progKey + L"\\shell\\open\\command", nullptr, quotedCommand(exePath)) && ok;

    std::wstring appKey = o.classesBase + L"\\Applications\\mpainter.exe";
    ok = setString(o.root, appKey, L"FriendlyAppName", kAppName) && ok;
    ok = setString(o.root, appKey + L"\\shell\\open\\command", nullptr, quotedCommand(exePath)) && ok;
    ok = setString(o.root, appKey + L"\\DefaultIcon", nullptr, L"\"" + exePath + L"\",0") && ok;

    std::wstring cap = o.appBase + L"\\Capabilities";
    ok = setString(o.root, cap, L"ApplicationName", kAppName) && ok;
    ok = setString(o.root, cap, L"ApplicationDescription", tr(L"Lightweight image viewer and annotator")) && ok;
    auto assoc = [&](const std::vector<std::wstring>& exts) {
        for (const auto& ext : exts) {
            ok = setString(o.root, cap + L"\\FileAssociations", ext.c_str(), kProgId) && ok;
            ok = setString(o.root, appKey + L"\\SupportedTypes", ext.c_str(), L"") && ok;
            ok = setString(o.root, o.classesBase + L"\\" + ext + L"\\OpenWithProgids", kProgId, L"") && ok;
        }
    };
    assoc(supportedExtensions());
    assoc(comicExtensions());
    ok = setString(o.root, o.registeredApps, kAppName, cap) && ok;
    return ok;
}

bool unregisterProgId(const ShellRegOptions& o) {
    bool ok = true;
    ok = deleteTree(o.root, o.classesBase + L"\\" + kProgId) && ok;
    ok = deleteTree(o.root, o.classesBase + L"\\Applications\\mpainter.exe") && ok;
    ok = deleteTree(o.root, o.appBase) && ok;
    ok = deleteValue(o.root, o.registeredApps, kAppName) && ok;
    for (const auto& ext : supportedExtensions())
        ok = deleteValue(o.root, o.classesBase + L"\\" + ext + L"\\OpenWithProgids", kProgId) && ok;
    for (const auto& ext : comicExtensions())
        ok = deleteValue(o.root, o.classesBase + L"\\" + ext + L"\\OpenWithProgids", kProgId) && ok;
    return ok;
}

std::wstring progIdExePath(const ShellRegOptions& o) {
    return exePathFromCommand(getString(o.root, o.classesBase + L"\\" + kProgId + L"\\shell\\open\\command", nullptr));
}

bool isWindows11() {
    // GetVersionEx 會被 manifest 影響，直接讀 kernel32 的版本資源最可靠。
    HMODULE k = GetModuleHandleW(L"kernel32.dll");
    if (!k) return false;
    wchar_t path[MAX_PATH];
    if (!GetModuleFileNameW(k, path, MAX_PATH)) return false;
    DWORD dummy = 0;
    DWORD size = GetFileVersionInfoSizeW(path, &dummy);
    if (!size) return false;
    std::vector<BYTE> data(size);
    if (!GetFileVersionInfoW(path, 0, size, data.data())) return false;
    VS_FIXEDFILEINFO* ffi = nullptr;
    UINT len = 0;
    if (!VerQueryValueW(data.data(), L"\\", reinterpret_cast<void**>(&ffi), &len) || !ffi) return false;
    DWORD build = LOWORD(ffi->dwFileVersionLS);
    return HIWORD(ffi->dwFileVersionMS) >= 10 && build >= 22000;
}

bool openDefaultAppsSettings() {
    // Win11 可以直接開到某個程式的頁面；Win10 不認得 registeredAppUser，退回整頁。
    const wchar_t* uri = isWindows11() ? L"ms-settings:defaultapps?registeredAppUser=mpainter" : L"ms-settings:defaultapps";
    auto r = reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr, L"open", uri, nullptr, nullptr, SW_SHOWNORMAL));
    if (r > 32) return true;
    r = reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr, L"open", L"ms-settings:defaultapps", nullptr, nullptr, SW_SHOWNORMAL));
    return r > 32;
}

} // namespace mp
