#pragma once
#include <windows.h>

#include <string>
#include <vector>

namespace mp {

// 登錄到哪裡。預設是 HKCU\Software\Classes（免 admin）。測試用別的子鍵。
struct ShellRegOptions {
    HKEY root = HKEY_CURRENT_USER;
    std::wstring classesBase = L"Software\\Classes";
    std::wstring appBase = L"Software\\mpainter";                   // Capabilities 放這裡
    std::wstring registeredApps = L"Software\\RegisteredApplications";
};

// 右鍵選單「用 mpainter 開啟」：每個支援的副檔名（SystemFileAssociations\.ext\shell\mpainter）加資料夾（Directory\shell\mpainter）。
bool registerContextMenu(const std::wstring& exePath, const ShellRegOptions& o = {});
bool unregisterContextMenu(const ShellRegOptions& o = {});
// 目前登錄的 exe 路徑。沒登錄回傳空字串。
std::wstring contextMenuExePath(const ShellRegOptions& o = {});

// 預設程式：ProgID mpainter.Image、Applications\mpainter.exe、Capabilities、RegisteredApplications、各副檔名的 OpenWithProgids。
// 登錄後 mpainter 會出現在「設定 > 預設應用程式」和「開啟方式」清單裡，真正設為預設要使用者自己點。
bool registerProgId(const std::wstring& exePath, const ShellRegOptions& o = {});
bool unregisterProgId(const ShellRegOptions& o = {});
std::wstring progIdExePath(const ShellRegOptions& o = {});

// 從登錄的命令字串（"C:\x\mpainter.exe" "%1"）取出 exe 路徑。
std::wstring exePathFromCommand(const std::wstring& command);

// 開「設定 > 預設應用程式」。Win10 開整頁，Win11 直接開 mpainter 那一頁（能開就開）。
bool openDefaultAppsSettings();
// 以 build 號判斷（22000 以上）。
bool isWindows11();

} // namespace mp
