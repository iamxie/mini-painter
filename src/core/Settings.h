#pragma once
#include <string>
#include <vector>

#include "core/Document.h"

namespace mp {

// 預先解碼最多佔多少記憶體（MB = MiB）：預設值與設定面板允許的範圍。0 = 關閉。
constexpr int kPrefetchDefaultMB = 512, kPrefetchMinMB = 0, kPrefetchMaxMB = 65536;
// 使用者輸入或 ini 裡的文字 → 上限。不是數字（含空字串）回 fallback，數字夾進範圍。
int prefetchMBFromText(const std::wstring& text, int fallback);

// 模式（設定集）：一組有名字的檢視設定，「顯示 › 模式」選單一點整組套上。數量不限、至少一個。
// Settings 裡同名的欄位是「目前的值」：選了模式後整組複製過來，之後在選單改的是目前的值，模式本體不動（臨時調整）；
// 「儲存目前設定到模式」才寫回。目前的值與所用模式不同 = 已調整（標題列顯示 *），載入時比對算出來，不另存旗標。
struct Profile {
    std::wstring name;
    std::wstring viewMode = L"fitwindow"; // fitwindow / fitwidth / fitheight / actual / twopage
    bool upscaleSmall = false;
    bool highQuality = false;
    bool scrollFlip = false;
    bool autoNextBook = false;
    bool rightToLeft = false;
    bool firstPageAlone = true;
    bool warnMetadataLoss = true;
};
// 兩個模式的內容（不比名字）是否相同。
bool samePrefs(const Profile& a, const Profile& b);

// 存在 exe 旁邊 mpainter.ini 的設定。portable，沒有別的存放位置。
struct Settings {
    bool askedDefault = false; // 已經問過「設為預設程式」
    int tipShown = 0;          // 物件教學提示已顯示次數
    bool contextMenu = true;   // 登錄右鍵選單「用 mpainter 開啟」
    std::wstring language;     // 語系代碼，例如 L"zh-TW"；空字串 = 跟隨系統
    // 檢視相關的「目前的值」（欄位與 Profile 一一對應，見 profileFromCurrent / applyProfile）
    std::wstring viewMode = L"fitwindow"; // fitwindow / fitwidth / fitheight / actual / twopage
    bool upscaleSmall = false;  // 符合類模式把小圖拉大到符合
    bool highQuality = false;   // 顯示縮放用 Lanczos3（false = Direct2D bilinear）
    bool scrollFlip = false;    // 捲到底再按下翻下一頁、捲到頂再按上翻上一頁
    bool autoNextBook = false;  // 翻到底再按一次，接著看同一層的下一個資料夾或壓縮檔（看漫畫用；關掉時只提示到底了）
    bool rightToLeft = false;   // 兩頁並排：右到左（日漫）
    bool firstPageAlone = true; // 兩頁並排：第一張單獨一屏
    bool warnMetadataLoss = true; // 另存成裝不下 metadata 的格式時先問
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
    // 模式清單與目前使用的模式名。ini：每個模式一段 [profile]（name= 加上面八個欄位），profile= 是目前用的。
    // 空清單表示 ini 裡沒有（第一次執行、舊版 ini）：呼叫端用 ensureProfiles 補內建的兩個。
    std::vector<Profile> profiles;
    std::wstring profileName;
};
constexpr int kTipMaxShown = 3;

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

// 讀檔。檔案不存在或讀不到時回傳 false，out 是預設值。
bool loadSettings(const std::wstring& path, Settings& out);
// 寫檔（UTF-8，無 BOM）。失敗回傳 false。
bool saveSettings(const std::wstring& path, const Settings& s);
// 這個路徑能不能寫（檔案存在就試開寫入，不存在就試在同資料夾建檔再刪）。
bool isPathWritable(const std::wstring& path);
// exe 路徑 -> 同資料夾的 mpainter.ini。
std::wstring settingsPathForExe(const std::wstring& exePath);

} // namespace mp
