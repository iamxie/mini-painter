#include "core/ShellReg.h"

#include <windows.h>

#include "core/AppInfo.h"
#include "core/FolderNav.h"
#include "core/PathUtil.h"

namespace mp {

namespace {

constexpr wchar_t kVerb[] = L"mpainter";
constexpr wchar_t kProgId[] = L"mpainter.Image";
constexpr wchar_t kAppName[] = L"mpainter";
const HKEY kRoot = HKEY_CURRENT_USER;

std::wstring quotedCommand(const std::wstring& exe) { return L"\"" + exe + L"\" \"%1\""; }
std::wstring iconValue(const std::wstring& exe) { return L"\"" + exe + L"\",0"; }

bool setString(const std::wstring& key, const wchar_t* name, const std::wstring& value) {
    HKEY h;
    if (RegCreateKeyExW(kRoot, key.c_str(), 0, nullptr, 0, KEY_WRITE, nullptr, &h, nullptr) != ERROR_SUCCESS) return false;
    LONG r = RegSetValueExW(h, name, 0, REG_SZ, reinterpret_cast<const BYTE*>(value.c_str()),
                            static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t)));
    RegCloseKey(h);
    return r == ERROR_SUCCESS;
}

// 讀字串值。機碼或值不存在回 false。
bool readString(const std::wstring& key, const wchar_t* name, std::wstring& out) {
    out.clear();
    HKEY h;
    if (RegOpenKeyExW(kRoot, key.c_str(), 0, KEY_READ, &h) != ERROR_SUCCESS) return false;
    wchar_t buf[2048];
    DWORD size = sizeof(buf), type = 0;
    LONG r = RegQueryValueExW(h, name, nullptr, &type, reinterpret_cast<BYTE*>(buf), &size);
    RegCloseKey(h);
    if (r != ERROR_SUCCESS || (type != REG_SZ && type != REG_EXPAND_SZ)) return false;
    buf[size / sizeof(wchar_t) < 2048 ? size / sizeof(wchar_t) : 2047] = L'\0';
    out = buf;
    return true;
}

std::wstring getString(const std::wstring& key, const wchar_t* name) {
    std::wstring s;
    readString(key, name, s);
    return s;
}

bool hasString(const std::wstring& key, const wchar_t* name, const std::wstring& value) {
    std::wstring s;
    return readString(key, name, s) && s == value;
}

bool deleteTree(const std::wstring& key) {
    LONG r = RegDeleteTreeW(kRoot, key.c_str());
    return r == ERROR_SUCCESS || r == ERROR_FILE_NOT_FOUND;
}

bool deleteValue(const std::wstring& key, const wchar_t* name) {
    HKEY h;
    if (RegOpenKeyExW(kRoot, key.c_str(), 0, KEY_WRITE, &h) != ERROR_SUCCESS) return true;
    LONG r = RegDeleteValueW(h, name);
    RegCloseKey(h);
    return r == ERROR_SUCCESS || r == ERROR_FILE_NOT_FOUND;
}

bool keyExists(const std::wstring& key) {
    HKEY h;
    if (RegOpenKeyExW(kRoot, key.c_str(), 0, KEY_READ, &h) != ERROR_SUCCESS) return false;
    RegCloseKey(h);
    return true;
}

// 機碼底下的子機碼名（values = false）或值名（values = true）。機碼不存在回空的。
std::vector<std::wstring> listNames(const std::wstring& key, bool values) {
    std::vector<std::wstring> out;
    HKEY h;
    if (RegOpenKeyExW(kRoot, key.c_str(), 0, KEY_READ, &h) != ERROR_SUCCESS) return out;
    for (DWORD i = 0;; ++i) {
        wchar_t name[256];
        DWORD len = 256;
        const LONG r = values ? RegEnumValueW(h, i, name, &len, nullptr, nullptr, nullptr, nullptr)
                              : RegEnumKeyExW(h, i, name, &len, nullptr, nullptr, nullptr, nullptr);
        if (r == ERROR_MORE_DATA) continue; // 名字太長的不會是副檔名
        if (r != ERROR_SUCCESS) break;
        out.emplace_back(name, len);
    }
    RegCloseKey(h);
    return out;
}

bool containsExt(const std::vector<std::wstring>& exts, const std::wstring& ext) {
    for (const auto& e : exts)
        if (_wcsicmp(e.c_str(), ext.c_str()) == 0) return true;
    return false;
}

// 右鍵選單要登錄的副檔名：圖片（含生效中的 plugin 的）加壓縮檔。壓縮檔只加右鍵，不登錄預設程式（除了 comicExtensions，見 registerProgId）。
std::vector<std::wstring> contextMenuExtensions() {
    std::vector<std::wstring> exts = supportedExtensions();
    exts.insert(exts.end(), archiveExtensions().begin(), archiveExtensions().end());
    return exts;
}

std::wstring contextMenuKeyOf(const ShellRegOptions& o, const std::wstring& ext) {
    return o.classesBase + L"\\SystemFileAssociations\\" + ext + L"\\shell\\" + kVerb;
}

std::vector<std::wstring> contextMenuKeys(const ShellRegOptions& o) {
    std::vector<std::wstring> keys;
    for (const auto& ext : contextMenuExtensions()) keys.push_back(contextMenuKeyOf(o, ext));
    keys.push_back(o.classesBase + L"\\Directory\\shell\\" + kVerb);
    return keys;
}

// 登錄裡有我們的右鍵項目、但現在不支援的副檔名：停用了的 plugin 的、舊版才有的。只看點開頭的子機碼。
std::vector<std::wstring> staleContextMenuExtensions(const ShellRegOptions& o) {
    std::vector<std::wstring> out;
    const std::vector<std::wstring> wanted = contextMenuExtensions();
    for (const auto& name : listNames(o.classesBase + L"\\SystemFileAssociations", false))
        if (!name.empty() && name[0] == L'.' && !containsExt(wanted, name) && keyExists(contextMenuKeyOf(o, name)))
            out.push_back(name);
    return out;
}

// 預設程式要關聯的副檔名：圖片（含生效中的 plugin 的）加漫畫壓縮檔。
std::vector<std::wstring> progIdExtensions() {
    std::vector<std::wstring> exts = supportedExtensions();
    exts.insert(exts.end(), comicExtensions().begin(), comicExtensions().end());
    return exts;
}

std::wstring capabilitiesKeyOf(const ShellRegOptions& o) { return o.appBase + L"\\Capabilities"; }

// Capabilities 裡登錄過、但現在不關聯的副檔名（同上）。
std::vector<std::wstring> staleProgIdExtensions(const ShellRegOptions& o) {
    std::vector<std::wstring> out;
    const std::vector<std::wstring> wanted = progIdExtensions();
    for (const auto& name : listNames(capabilitiesKeyOf(o) + L"\\FileAssociations", true))
        if (!name.empty() && name[0] == L'.' && !containsExt(wanted, name)) out.push_back(name);
    return out;
}

std::wstring progKeyOf(const ShellRegOptions& o) { return o.classesBase + L"\\" + kProgId; }
std::wstring applicationKeyOf(const ShellRegOptions& o, const std::wstring& exeName) {
    return o.classesBase + L"\\Applications\\" + exeName;
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
    for (const auto& k : contextMenuKeys(o)) {
        ok = setString(k, nullptr, o.text.openWith) && ok;
        ok = setString(k, L"Icon", iconValue(exePath)) && ok;
        ok = setString(k + L"\\command", nullptr, quotedCommand(exePath)) && ok;
    }
    for (const auto& ext : staleContextMenuExtensions(o)) ok = deleteTree(contextMenuKeyOf(o, ext)) && ok;
    return ok;
}

bool unregisterContextMenu(const ShellRegOptions& o) {
    bool ok = true;
    for (const auto& ext : staleContextMenuExtensions(o)) ok = deleteTree(contextMenuKeyOf(o, ext)) && ok;
    for (const auto& k : contextMenuKeys(o)) ok = deleteTree(k) && ok;
    return ok;
}

std::wstring contextMenuExePath(const ShellRegOptions& o) {
    // 以第一個副檔名為準。
    auto keys = contextMenuKeys(o);
    return exePathFromCommand(getString(keys.front() + L"\\command", nullptr));
}

bool contextMenuUpToDate(const std::wstring& exePath, const ShellRegOptions& o) {
    for (const auto& k : contextMenuKeys(o)) {
        if (!hasString(k, nullptr, o.text.openWith) || !hasString(k, L"Icon", iconValue(exePath))) return false;
        if (_wcsicmp(exePathFromCommand(getString(k + L"\\command", nullptr)).c_str(), exePath.c_str()) != 0) return false;
    }
    return staleContextMenuExtensions(o).empty();
}

bool registerProgId(const std::wstring& exePath, const ShellRegOptions& o) {
    bool ok = true;
    // 上次用別的 exe 檔名登錄過：那一個 Applications 機碼不會再被用到。
    const std::wstring exeName = fileNameOf(exePath);
    const std::wstring oldName = fileNameOf(progIdExePath(o));
    if (!oldName.empty() && _wcsicmp(oldName.c_str(), exeName.c_str()) != 0) deleteTree(applicationKeyOf(o, oldName));

    const std::wstring progKey = progKeyOf(o);
    ok = setString(progKey, nullptr, o.text.fileType) && ok;
    ok = setString(progKey, L"FriendlyTypeName", o.text.fileType) && ok;
    ok = setString(progKey + L"\\DefaultIcon", nullptr, iconValue(exePath)) && ok;
    ok = setString(progKey + L"\\shell\\open\\command", nullptr, quotedCommand(exePath)) && ok;

    const std::wstring appKey = applicationKeyOf(o, exeName);
    ok = setString(appKey, L"FriendlyAppName", kAppName) && ok;
    ok = setString(appKey + L"\\shell\\open\\command", nullptr, quotedCommand(exePath)) && ok;
    ok = setString(appKey + L"\\DefaultIcon", nullptr, iconValue(exePath)) && ok;

    const std::wstring cap = capabilitiesKeyOf(o);
    ok = setString(cap, L"ApplicationName", kAppName) && ok;
    ok = setString(cap, L"ApplicationDescription", o.text.appDescription) && ok;
    // 不再關聯的（停用了的 plugin 的）先拿掉。要在寫新的之前讀：讀的是 Capabilities 裡登錄過的清單。
    for (const auto& ext : staleProgIdExtensions(o)) {
        ok = deleteValue(cap + L"\\FileAssociations", ext.c_str()) && ok;
        ok = deleteValue(appKey + L"\\SupportedTypes", ext.c_str()) && ok;
        ok = deleteValue(o.classesBase + L"\\" + ext + L"\\OpenWithProgids", kProgId) && ok;
    }
    for (const auto& ext : progIdExtensions()) {
        ok = setString(cap + L"\\FileAssociations", ext.c_str(), kProgId) && ok;
        ok = setString(appKey + L"\\SupportedTypes", ext.c_str(), L"") && ok;
        ok = setString(o.classesBase + L"\\" + ext + L"\\OpenWithProgids", kProgId, L"") && ok;
    }
    ok = setString(o.registeredApps, kAppName, cap) && ok;
    return ok;
}

bool unregisterProgId(const ShellRegOptions& o) {
    bool ok = true;
    // 各副檔名的 OpenWithProgids：現在關聯的加上登錄過、現在不關聯的（Capabilities 等一下就整個刪了，先讀）。
    std::vector<std::wstring> exts = progIdExtensions();
    for (const auto& ext : staleProgIdExtensions(o)) exts.push_back(ext);
    const std::wstring exeName = fileNameOf(progIdExePath(o));
    if (!exeName.empty()) ok = deleteTree(applicationKeyOf(o, exeName)) && ok;
    ok = deleteTree(applicationKeyOf(o, kPackageExeName)) && ok; // v0.9.5 以前一律登錄在這個名字底下
    ok = deleteTree(progKeyOf(o)) && ok;
    ok = deleteTree(o.appBase) && ok;
    ok = deleteValue(o.registeredApps, kAppName) && ok;
    for (const auto& ext : exts) ok = deleteValue(o.classesBase + L"\\" + ext + L"\\OpenWithProgids", kProgId) && ok;
    return ok;
}

std::wstring progIdExePath(const ShellRegOptions& o) {
    return exePathFromCommand(getString(progKeyOf(o) + L"\\shell\\open\\command", nullptr));
}

bool progIdUpToDate(const std::wstring& exePath, const ShellRegOptions& o) {
    if (_wcsicmp(progIdExePath(o).c_str(), exePath.c_str()) != 0) return false;
    const std::wstring progKey = progKeyOf(o), appKey = applicationKeyOf(o, fileNameOf(exePath));
    const std::wstring cap = o.appBase + L"\\Capabilities";
    if (!hasString(progKey, nullptr, o.text.fileType) || !hasString(progKey, L"FriendlyTypeName", o.text.fileType)) return false;
    if (!hasString(progKey + L"\\DefaultIcon", nullptr, iconValue(exePath))) return false;
    if (!hasString(appKey + L"\\shell\\open\\command", nullptr, quotedCommand(exePath))) return false;
    if (!hasString(cap, L"ApplicationDescription", o.text.appDescription)) return false;
    if (!hasString(o.registeredApps, kAppName, cap)) return false;
    for (const auto& ext : progIdExtensions()) {
        if (!hasString(cap + L"\\FileAssociations", ext.c_str(), kProgId)) return false;
        if (!hasString(appKey + L"\\SupportedTypes", ext.c_str(), L"")) return false;
        if (!hasString(o.classesBase + L"\\" + ext + L"\\OpenWithProgids", kProgId, L"")) return false;
    }
    return staleProgIdExtensions(o).empty();
}

} // namespace mp
