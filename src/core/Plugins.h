#pragma once
#include <memory>
#include <string>
#include <vector>

#include "core/Image.h"
#include "core/ImageFormat.h"
#include "core/IoError.h"
#include "core/PluginManifest.h"
#include "core/PluginRun.h"

struct MpPluginFuncs; // src/plugins/sdk/mpainter_plugin.h

namespace mp {

struct Settings; // core/Settings.h

// 讀圖 plugin：plugins\<id>\<id>.dll，介面見 src/plugins/sdk/mpainter_plugin.h。
// 流程：scanPlugins（只讀資訊）→ 呼叫端決定要啟用哪些、排除衝突 → loadPlugins → setActivePlugins。
// 之後副檔名表（ImageFormat）、開檔（ImageIO）、壓縮檔裡沒副檔名的檔（Archive）都會問生效中的 plugin。

// 這個主程式的 plugin 介面版本（= SDK 的 MP_PLUGIN_API_VERSION），與還接受的最舊版本。
// 載入時照資訊宣告的版本和 plugin 講話：v1 的 probe 只給檔頭（MP_PLUGIN_PROBE_BYTES），v2 給整個檔。
constexpr unsigned kPluginApiVersion = 2;
constexpr unsigned kPluginApiOldest = 1;
bool pluginApiSupported(unsigned api);

// 檔頭是這個內建格式（或認不得）的檔，要先問 plugin、沒有 plugin 認才走內建：TIFF 是很多格式的容器
// （DNG、NEF、CR2 的檔頭都和 TIFF 一樣）；交給系統 codec 解的格式（HEIF，ImageFormatInfo::systemCodec）
// 讓自帶解碼器的 plugin 接手。PNG、JPEG、WebP、BMP 直接走內建，不問 plugin。
bool askPluginsFirst(ImageFormat format);

// exe 旁的 plugins 資料夾。
std::wstring pluginsDirForExe(const std::wstring& exeDir);
// 發佈包隨附的（core/AppInfo.h 的 kBundledPlugins，不分大小寫）。
bool isBundledPlugin(const std::wstring& id);

// plugins 資料夾裡的一個 plugin：掃描的結果，只讀了資訊、沒有執行它的程式。
struct PluginEntry {
    std::wstring id; // 資料夾名
    std::wstring dllPath;
    ManifestRead manifest;
    // 宣告的副檔名裡，內建已經在用的（圖片格式與壓縮檔；交給系統 codec 的 HEIF 不算）：不交給這個 plugin，設定頁註明。
    std::vector<std::wstring> builtInOverlap;
};
// 掃 pluginsDir 底下的子資料夾 <id>，有 <id>.dll 的才算。照 id 排序（不分大小寫）。資料夾不在回空的。
std::vector<PluginEntry> scanPlugins(const std::wstring& pluginsDir);
// 這個 plugin 會接手的副檔名：資訊合格時，宣告的扣掉內建的；不合格是空的。
std::vector<std::wstring> pluginEffectiveExtensions(const PluginEntry& e);

// 一個 plugin 載入的結果（設定頁顯示用）。
enum class PluginLoadError {
    None,
    BadManifest,  // 資訊讀不到或不合格（PluginEntry::manifest 有原因）
    ApiMismatch,  // 資訊宣告的介面版本這個主程式不支援
    LoadFailed,   // LoadLibrary 失敗（systemError）
    NoEntry,      // 沒有匯出 mpPluginGetFuncs
    Incompatible, // mpPluginGetFuncs 回 NULL、版本不對、或少了函式
};
struct PluginLoadResult {
    std::wstring id;
    PluginLoadError error = PluginLoadError::None;
    unsigned long systemError = 0;
};
// 不必載入就看得出來的問題：資訊讀不到或不合格（BadManifest）、介面版本不支援（ApiMismatch）。沒問題回 None。
PluginLoadError pluginStaticCheck(const PluginEntry& e);

// 兩個以上要啟用的 plugin 認同一個副檔名。
struct PluginConflict {
    std::wstring extension;
    std::vector<std::wstring> ids; // 照 entries 的順序
};
// enabledIds（不分大小寫）裡的 plugin 之間的衝突，照副檔名排序。用不了的（pluginStaticCheck 有問題）不算。
std::vector<PluginConflict> findPluginConflicts(const std::vector<PluginEntry>& entries, const std::vector<std::wstring>& enabledIds);
// 設定頁要勾 id 時：它和 enabledIds 裡的哪些 plugin 搶哪些副檔名（enabledIds 之間原本的衝突不算）。
std::vector<PluginConflict> pluginConflictsIfEnabled(const std::vector<PluginEntry>& entries, const std::vector<std::wstring>& enabledIds,
                                                     const std::wstring& id);
// 衝突牽涉到的 id，不重複，照出現順序。
std::vector<std::wstring> conflictingIds(const std::vector<PluginConflict>& conflicts);

// 載入好的一組 plugin。建好之後只讀，任何執行緒都可以用；解構時 FreeLibrary（拿著 shared_ptr 的呼叫不會被拆掉）。
class PluginSet {
public:
    struct Plugin {
        std::wstring id;
        PluginManifest manifest;
        std::vector<std::wstring> extensions; // 實際接手的副檔名
        void* module = nullptr;               // HMODULE
        const MpPluginFuncs* funcs = nullptr;
    };

    PluginSet() = default;
    ~PluginSet();
    PluginSet(const PluginSet&) = delete;
    PluginSet& operator=(const PluginSet&) = delete;

    const std::vector<Plugin>& plugins() const { return plugins_; }
    // 哪個 plugin 接手這個副檔名（含點，不分大小寫）；沒有回 nullptr。
    const Plugin* pluginForExtension(const std::wstring& ext) const;

private:
    friend std::shared_ptr<PluginSet> loadPlugins(const std::vector<PluginEntry>&, const std::vector<std::wstring>&,
                                                  std::vector<PluginLoadResult>&);
    std::vector<Plugin> plugins_;
};

// 照 entries 的順序載入 enabledIds 裡的 plugin，每個試過的都回報在 results。衝突要先由呼叫端排除；
// 萬一還有，副檔名給先載入的那個。DllMain 與 mpPluginGetFuncs 在 PluginCallScope 裡執行（當掉認得出來）。
std::shared_ptr<PluginSet> loadPlugins(const std::vector<PluginEntry>& entries, const std::vector<std::wstring>& enabledIds,
                                       std::vector<PluginLoadResult>& results);

// 生效中的那一組。啟動時設一次（測試會換）；沒設時是 nullptr，只有內建格式。
void setActivePlugins(std::shared_ptr<const PluginSet> set);
std::shared_ptr<const PluginSet> activePlugins();

// ---- 啟用狀態與啟動流程（app 用）----

// 設定裡這個 plugin 算不算啟用：有紀錄看紀錄（只有 Enabled 算）；沒紀錄的，隨附的算啟用、其他的停用。
bool pluginEnabled(const Settings& s, const std::wstring& id);
// entries 裡算啟用的 id，照 entries 的順序。
std::vector<std::wstring> enabledPluginIds(const std::vector<PluginEntry>& entries, const Settings& s);

// 同一批 plugin 搶的副檔名合成一組（給使用者看的訊息一組一則）。
struct PluginConflictGroup {
    std::vector<std::wstring> ids;        // 照 conflicts 裡第一次出現的順序
    std::vector<std::wstring> extensions; // 照副檔名排序
};
std::vector<PluginConflictGroup> groupPluginConflicts(const std::vector<PluginConflict>& conflicts);

// 啟動時的結果。crashes 與 conflicts 裡的 plugin 這次都停用了，狀態已經寫進 Settings（呼叫端要存檔、告訴使用者）。
struct PluginStartup {
    std::vector<PluginEntry> entries;           // 掃到的全部（設定頁也用）
    std::vector<PluginCrash> crashes;           // 上次害程式當掉、這次停用的（同一個 plugin 只留一筆）
    std::vector<PluginConflictGroup> conflicts; // 互相衝突、這次全部停用的
    std::vector<PluginLoadResult> results;      // 試著載入的每一個
    bool settingsChanged = false;
};
// app 啟動時呼叫一次：
//   1. 讀 runDirs 裡上次的當掉紀錄，怪到的 plugin 標成 Crashed。
//   2. 掃 pluginsDir，算出要啟用的；彼此衝突的全部標成 Conflict、不載入。
//   3. 有要載入的才裝 filter、開這次的當掉紀錄（runDirs 依序試，第一個建得起來的）。
//   4. 載入、setActivePlugins。
PluginStartup startPlugins(const std::wstring& pluginsDir, const std::vector<std::wstring>& runDirs, const std::wstring& exePath,
                           Settings& s);

// ---- 給 ImageFormat / ImageIO / Archive 用（都看生效中的那一組）----

bool pluginHandlesExtension(const std::wstring& ext);
// 全部接手的副檔名，照 plugin、宣告的順序。
std::vector<std::wstring> pluginExtensions();

// 一個檔是哪個 plugin 的哪個格式。拿著 set，呼叫期間 plugin 不會被卸載。
struct PluginMatch {
    std::shared_ptr<const PluginSet> set;
    const PluginSet::Plugin* plugin = nullptr;
    int format = -1;

    explicit operator bool() const { return plugin != nullptr; }
    // 格式的顯示名（英文翻譯 key，plugin 給的）。
    std::wstring formatName() const;
    // 這個格式實際接手的第一個副檔名（給壓縮檔裡沒副檔名的檔補上）；這個格式的都被內建拿走時用這個 plugin 的第一個。
    std::wstring extension() const;
};
// 整個檔映射進來，問每個 plugin 的 probe（v1 的只給檔頭），第一個認的接手。
// 讀不到、空檔、probe 期間檔案被截短都回空的。
PluginMatch matchPluginFile(const std::wstring& path);

// 讀寬高（plugin 的 readSize）。失敗回 false。
bool pluginReadSize(const PluginMatch& m, const std::wstring& path, int& width, int& height);

struct PluginDecoded {
    StraightImage image; // 失敗時是空的
    Dpi dpi;             // plugin 沒給或不合理時是 96
    IoError error;
};
// 解碼（plugin 的 decode）。不丟例外：配置不到記憶體回 OutOfMemory，plugin 回報失敗是 PluginFailed
// （detail 是英文，含 plugin id、回傳值與它的說明，不翻譯）。
PluginDecoded pluginDecode(const PluginMatch& m, const std::wstring& path);

} // namespace mp
