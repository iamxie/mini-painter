#pragma once
#include <string>
#include <vector>

namespace mp {

// 登錄裡顯示給使用者看的文字。core 不翻譯：app 填翻譯好的，預設值是英文。
struct ShellRegText {
    std::wstring openWith = L"Open with mpainter";    // 右鍵選單的項目
    std::wstring fileType = L"mpainter image";        // ProgID 的檔案類型名
    std::wstring appDescription = L"Lightweight image viewer and annotator";
};

// 登錄到哪裡（都在 HKCU 底下，免 admin）。預設是真的位置，測試用別的子鍵。
struct ShellRegOptions {
    std::wstring classesBase = L"Software\\Classes";
    std::wstring appBase = L"Software\\mpainter";                   // Capabilities 放這裡
    std::wstring registeredApps = L"Software\\RegisteredApplications";
    ShellRegText text;
};

// 右鍵選單「用 mpainter 開啟」：每個支援的副檔名（SystemFileAssociations\.ext\shell\mpainter）加資料夾（Directory\shell\mpainter）。
bool registerContextMenu(const std::wstring& exePath, const ShellRegOptions& o = {});
bool unregisterContextMenu(const ShellRegOptions& o = {});
// 目前登錄的 exe 路徑。沒登錄回傳空字串。
std::wstring contextMenuExePath(const ShellRegOptions& o = {});
// 登錄的內容是不是就是 registerContextMenu(exePath, o) 會寫的：每個副檔名與資料夾的機碼都在，exe 路徑與顯示文字都相同。
// 啟動時用它決定要不要重新登錄：exe 搬了位置、換了語言（文字不同）、新版多了副檔名（少了機碼）都會是 false。
bool contextMenuUpToDate(const std::wstring& exePath, const ShellRegOptions& o = {});

// 預設程式：ProgID mpainter.Image、Applications\<exe 檔名>、Capabilities、RegisteredApplications、各副檔名的 OpenWithProgids。
// 登錄後 mpainter 會出現在「設定 > 預設應用程式」和「開啟方式」清單裡，真正設為預設要使用者自己點。
// Applications 底下用 exePath 實際的檔名（exe 被改名時「開啟方式」才對得上）；上次用別的檔名登錄的那一個會刪掉。
bool registerProgId(const std::wstring& exePath, const ShellRegOptions& o = {});
bool unregisterProgId(const ShellRegOptions& o = {});
std::wstring progIdExePath(const ShellRegOptions& o = {});
// 同 contextMenuUpToDate：exe 路徑、檔案類型名、每個副檔名的關聯都和 registerProgId(exePath, o) 會寫的一樣。
bool progIdUpToDate(const std::wstring& exePath, const ShellRegOptions& o = {});

// 從登錄的命令字串（"C:\x\mpainter.exe" "%1"）取出 exe 路徑。
std::wstring exePathFromCommand(const std::wstring& command);

} // namespace mp
