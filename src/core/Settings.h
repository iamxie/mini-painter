#pragma once
#include <string>
#include <vector>

#include "core/UndoLimits.h"
#include "core/ViewFit.h"

namespace mp {

// 預先解碼最多佔多少記憶體（MB = MiB）：預設值與設定面板允許的範圍。0 = 關閉。
constexpr int kPrefetchDefaultMB = 512, kPrefetchMinMB = 0, kPrefetchMaxMB = 65536;
// 使用者輸入或 ini 裡的文字 → 上限。不是數字（含空字串）回 fallback，數字夾進範圍。
int prefetchMBFromText(const std::wstring& text, int fallback);

// 檢視設定裡的開關：名稱（同時是 ini 的 key）與預設值。只寫在這裡；
// 欄位、ini 的讀寫、比較、設定面板的勾選框都從這張表來，加一個開關只要加一行（設定面板再加一個勾選框）。
#define MP_VIEW_FLAGS(X)                                                                                       \
    X(upscaleSmall, false)    /* 符合類模式把小圖拉大到符合 */                                                 \
    X(highQuality, false)     /* 顯示縮放用 Lanczos3（false = Direct2D bilinear） */                           \
    X(scrollFlip, false)      /* 捲到底再按下翻下一頁、捲到頂再按上翻上一頁 */                                 \
    X(autoNextBook, false)    /* 翻到底再按一次，接著看同一層的下一個資料夾或壓縮檔（關掉時只提示到底了） */   \
    X(rightToLeft, false)     /* 兩頁並排：右到左（日漫） */                                                   \
    X(firstPageAlone, true)   /* 兩頁並排：第一張單獨一屏 */                                                   \
    X(warnMetadataLoss, true) /* 另存成裝不下 metadata 的格式時先問 */

// 一組檢視設定：檢視模式加上面那些開關。模式（Profile）存的、與視窗「目前的值」（Settings::view）都是這個型別。
struct ViewPrefs {
    ViewMode viewMode = ViewMode::FitWindow;
#define MP_X(name, def) bool name = def;
    MP_VIEW_FLAGS(MP_X)
#undef MP_X
    bool operator==(const ViewPrefs& o) const;
    bool operator!=(const ViewPrefs& o) const { return !(*this == o); }
};

// 一個開關：ini 的 key 與它在 ViewPrefs 裡的成員。
struct ViewFlag {
    const wchar_t* key;
    bool ViewPrefs::*member;
};
#define MP_X(name, def) {L## #name, &ViewPrefs::name},
inline constexpr ViewFlag kViewFlags[] = {MP_VIEW_FLAGS(MP_X)};
#undef MP_X

// 模式（設定集）：一組有名字的檢視設定，「顯示 › 模式」選單一點整組套上。數量不限、至少一個。
// 選了模式後整組複製成 Settings::view（目前的值），之後在選單改的是目前的值，模式本體不動（臨時調整）；
// 「儲存目前設定到模式」才寫回。目前的值與所用模式不同 = 已調整（標題列顯示 *），載入時比對算出來，不另存旗標。
struct Profile {
    std::wstring name;
    ViewPrefs prefs;
};

// 存在 exe 旁邊 mpainter.ini 的設定。portable，沒有別的存放位置。
struct Settings {
    bool askedDefault = false; // 已經問過「設為預設程式」
    int tipShown = 0;          // 物件教學提示已顯示次數
    bool contextMenu = true;   // 登錄右鍵選單「用 mpainter 開啟」
    std::wstring language;     // 語系代碼，例如 L"zh-TW"；空字串 = 跟隨系統
    ViewPrefs view;            // 目前的檢視值（見 Profile）
    bool autoCheckUpdate = true;  // 啟動時在背景查 GitHub 有沒有新版
    std::wstring skipVersion;     // 「略過這個版本」選過的版本號；自動檢查遇到它不問（手動檢查照問）
    std::wstring updateUrl;       // 檢查網址；空字串 = GitHub。只給測試用，設定面板沒有，空的時候不寫進 ini
    int undoLimit = kUndoLimitDefault; // undo 最多留幾步（1～999，讀進來時夾進範圍）；整圖操作每一步佔一張圖的記憶體
    int undoMemoryMB = kUndoMemoryDefaultMB; // undo 歷史裡的底圖最多佔多少 MB；超過丟最舊的步驟（最近一步一定留）
    int prefetchMB = kPrefetchDefaultMB;     // 預先解碼（下一屏、上一屏、下一本的第一屏）最多佔多少 MB；0 = 關閉
    std::wstring unzipDir;        // 壓縮檔解到哪裡；空字串 = exe 旁的 temp（建不了就退回 %TEMP%\mpainter\unzip）
    // 置頂檔名：清單裡的檔名（不分大小寫，只比檔名不比路徑）排在最前面，依清單順序。ini 用 | 隔開（檔名不能含 |）。
    std::vector<std::wstring> pinnedNames = {L"cover.jpg", L"cover.png", L"cover.webp"};
    // 密碼列表：開加密壓縮檔時先逐一試過，都不對才問；問到對的就加進來。ini 一行一個 password=，明文。
    // 前後空白算密碼的一部分，讀寫都不 trim。
    std::vector<std::wstring> passwords;
    // 上次關閉時的視窗位置（一般狀態的矩形，螢幕座標）與是否最大化。winW == 0 表示沒有紀錄。ini：window=x,y,w,h,max
    int winX = 0, winY = 0, winW = 0, winH = 0;
    bool winMax = false;
    // 模式清單與目前使用的模式名。ini：每個模式一段 [profile]（name= 加檢視設定），profile= 是目前用的。
    // 空清單表示 ini 裡沒有（第一次執行、舊版 ini）：呼叫端用 ensureProfiles 補內建的兩個。
    std::vector<Profile> profiles;
    std::wstring profileName;
};
constexpr int kTipMaxShown = 3;

// 設定面板「一般」頁的數字欄位：ini 的 key、成員、允許的範圍。ini 的讀寫與設定面板共用這張表。
struct IntSetting {
    const wchar_t* key;
    int Settings::*member;
    int min, max;
};
inline constexpr IntSetting kIntSettings[] = {
    {L"undoLimit", &Settings::undoLimit, kUndoLimitMin, kUndoLimitMax},
    {L"undoMemoryMB", &Settings::undoMemoryMB, kUndoMemoryMinMB, kUndoMemoryMaxMB},
    {L"prefetchMB", &Settings::prefetchMB, kPrefetchMinMB, kPrefetchMaxMB},
};
// 文字 → 這個欄位的值。不是數字（含空字串）回 fallback，數字夾進範圍。
int intSettingFromText(const IntSetting& field, const std::wstring& text, int fallback);

// 目前的值 → Profile（name 是 profileName）；Profile → 目前的值（並把 profileName 設成它的名字）。
Profile profileFromCurrent(const Settings& s);
void applyProfile(Settings& s, const Profile& p);
// 找名字（不分大小寫）。沒有回 nullptr。
const Profile* findProfile(const Settings& s, const std::wstring& name);
Profile* findProfile(Settings& s, const std::wstring& name);
// 目前的值和所用模式不同（或所用模式不存在）。
bool profileModified(const Settings& s);
// 內建的兩個模式：一般（實際大小、不放大、快速縮放、不接下一本、不並排、要 metadata 警告）、
// 漫畫（符合高度、兩頁並排、接下一本、高品質、不警告）。名字由呼叫端給（要翻譯）。
std::vector<Profile> defaultProfiles(const std::wstring& generalName, const std::wstring& comicName);
// 清單空的補內建兩個並把目前的值套成第一個（一開始就是乾淨的「一般模式」），profileName 找不到就指到第一個。回傳有沒有改動。
bool ensureProfiles(Settings& s, const std::wstring& generalName, const std::wstring& comicName);

// ini 文字 <-> Settings。不認得的 key 略過，缺的 key 用預設值。
Settings parseSettings(const std::wstring& text);
std::wstring serializeSettings(const Settings& s);

// 設定面板按「套用」或「確定」：面板交回的整份設定 fromDialog 併進 current。
// 面板不管的只有兩樣，維持 current 的值：語言（要重新啟動，由呼叫端最後處理）與目前的檢視值（view）。
// 其他欄位一律照抄，所以設定檔加了新欄位不必改這裡。回傳哪些地方變了，呼叫端照著套用。
struct DialogChanges {
    bool contextMenu = false;    // 右鍵選單的開關
    bool pinnedNames = false;    // 置頂檔名清單
    bool unzipRoot = false;      // 實際的解壓暫存位置（resolveUnzipRoot 之後不分大小寫比）
    bool undoLimits = false;     // undo 的步數或記憶體上限
    bool prefetch = false;       // 預先解碼的記憶體上限
    bool profiles = false;       // 模式清單或正在用的模式名
    bool reapplyProfile = false; // 換了模式、或正在用的那個模式內容被改了：呼叫端要重新套用 current.profileName 指的模式
    bool language = false;       // fromDialog.language 和 current.language 不同
};
DialogChanges mergeDialogSettings(Settings& current, const Settings& fromDialog, const std::wstring& exeDir);

// 同時開好幾個視窗時，各自手上的設定會互相蓋掉。存檔改成三方合併：
//   base：這個視窗上次和檔案同步時的樣子；mine：這個視窗現在的；disk：檔案裡現在的（別的視窗可能改過）。
//   這個視窗改過的欄位（mine 和 base 不同）用 mine，沒改過的用 disk。逐欄位比（檢視值每個開關各自算）。
//   密碼列表逐筆合併：disk 的每一筆都留，除了這個視窗刪掉的（base 有、mine 沒有）；這個視窗加的接在後面。
//   置頂檔名、模式清單整份當成一個欄位。視窗位置五個數字當成一個欄位。
Settings mergeSettings(const Settings& base, const Settings& mine, const Settings& disk);

// 讀檔的結果。Missing：檔案不存在（第一次執行）。Unreadable：檔案在但讀不到（被別的程式獨佔、沒權限、讀到一半失敗）。
// Unreadable 時呼叫端不能存檔：手上只有預設值，存了就把原本的設定蓋掉。
enum class SettingsLoad { Loaded, Missing, Unreadable };
// 讀檔。不是 Loaded 時 out 是預設值。
SettingsLoad loadSettings(const std::wstring& path, Settings& out);
// 寫檔（UTF-8，無 BOM）。先寫到同資料夾的暫存檔（<path>.<pid>.tmp）再取代原檔：別人同時在讀的話，讀到的不是舊的就是新的，
// 不會是半份；寫到一半當掉也不會留下截斷的設定檔。失敗回傳 false，原檔不動、暫存檔刪掉。
// systemError 不是 nullptr：失敗時寫進失敗那一步的 Win32 錯誤碼（原檔唯讀是 ERROR_ACCESS_DENIED），成功時是 0。
bool saveSettings(const std::wstring& path, const Settings& s, unsigned long* systemError = nullptr);
// 讀檔 → mergeSettings(base, mine, 檔案裡的) → 寫檔，整段用具名 mutex 包住（別的 mpainter 不會同時做同一件事）。
// 檔案不存在時直接寫 mine。檔案在但讀不到時不寫，回 false。成功時 written 是寫進檔案的內容。
// systemError 同 saveSettings；檔案在但讀不到時是 0（不知道原因）。
bool saveSettingsMerged(const std::wstring& path, const Settings& base, const Settings& mine, Settings& written,
                        unsigned long* systemError = nullptr);
// 這個路徑能不能寫（檔案存在就試開寫入，不存在就試在同資料夾建檔再刪）。
bool isPathWritable(const std::wstring& path);
// exe 路徑 -> 同資料夾的 mpainter.ini。
std::wstring settingsPathForExe(const std::wstring& exePath);

} // namespace mp
