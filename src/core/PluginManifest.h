#pragma once
#include <string>
#include <vector>

namespace mp {

// plugin 自己宣告的資訊，放在 DLL 的 RCDATA 資源 MPAINTER_PLUGIN 裡（格式見 src/plugins/sdk/mpainter_plugin.h）。
// 主程式只當資料讀，不執行 DLL 的程式：還沒被使用者啟用的 plugin 也能列出來、檢查衝突。
struct PluginFormatDecl {
    std::wstring name;                    // 顯示名（英文，例如 L"JPEG XL image"）
    std::vector<std::wstring> extensions; // 小寫、含點，宣告的順序
};

struct PluginManifest {
    unsigned api = 0;     // plugin 照哪一版介面寫的
    std::wstring name;    // 例如 L"JPEG XL"
    std::wstring version; // plugin 自己的版本，只顯示
    std::vector<PluginFormatDecl> formats; // 格式的 index 就是這裡的順序
};

// 資訊文字（UTF-8，可以有 BOM）→ PluginManifest。不認得的 key 略過、空行與 # 開頭的行略過。
// 必要的：api（正整數）、name、version、至少一個 format=<名稱>|<副檔名>[;<副檔名>...]。
// 副檔名轉成小寫；要以點開頭、點後面至少一個字，不能有空白與 * ? ; | / \ : 這些字。
// 不合格回 false，problem 是英文說明（不翻譯，給回報問題用）。
bool parsePluginManifest(const std::string& utf8, PluginManifest& out, std::wstring& problem);

enum class ManifestError {
    None,
    Unreadable, // DLL 開不了（不存在、不是 PE 檔）。systemError 是 GetLastError()
    NotFound,   // 沒有 MPAINTER_PLUGIN 這個資源：不是 mpainter 的 plugin
    Invalid,    // 資源在，內容不合格（problem 有說明）
};

struct ManifestRead {
    PluginManifest manifest;
    ManifestError error = ManifestError::None;
    std::wstring problem;          // Invalid 時的說明
    unsigned long systemError = 0; // Unreadable 時
    bool ok() const { return error == ManifestError::None; }
};

// 以資料檔的方式開 DLL（不執行 DllMain）讀資訊。
ManifestRead readPluginManifest(const std::wstring& dllPath);

} // namespace mp
