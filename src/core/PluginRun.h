#pragma once
#include <string>
#include <vector>

namespace mp {

// 認出「plugin 害程式當掉」。plugin 在行程裡跑，當掉就是整個程式當掉，事後要知道是不是它。做法：
//   - 每個行程一個紀錄檔（固定大小、記憶體映射），每條在 plugin 裡的執行緒佔一格，記著哪個 plugin、哪個函式、
//     哪個檔；離開 plugin 就清掉。只是記憶體寫入；行程死掉時映射的內容仍會寫回檔案。
//   - unhandled exception filter：當掉時在同一個檔記下出事的執行緒、模組、例外代碼（也只寫記憶體）。
//   - 下次啟動讀已經結束的行程留下的檔，用 judgePluginRun 判斷。
// 抓不到的：plugin 弄壞記憶體、當下沒事、之後在主程式裡才爆。會誤判的：__fastfail（/GS、abort）不經過 filter，
// 那時剛好有 plugin 在跑就算它的；在 plugin 裡被強制結束（工作管理員、關機）也算它的。

// 紀錄檔放在 <dir>\<pid>-<8 位十六進位>.run。dir 由呼叫端決定（app 用暫存根目錄底下的 plugin-runs）。
constexpr wchar_t kPluginRunsDirName[] = L"plugin-runs";

// 開始記錄：建本行程的紀錄檔並記下 exe 路徑（dir 不在就建）。整個行程一份，已經開了就直接回 true。建不起來回 false
// （之後照常呼叫 plugin，只是當掉時認不出來）。
bool startPluginRunLog(const std::wstring& dir, const std::wstring& exePath);
// 正常結束時呼叫：之後還在 plugin 裡的背景執行緒被系統結束，不算 plugin 的。檔案留著，下次啟動刪。
void markPluginRunClean();
// 測試用：關掉紀錄。clean = false 等於行程在這一刻死掉（格子不清、不標正常結束）。
void stopPluginRunLogForTest(bool clean);
// 目前紀錄檔的路徑；沒開是空字串。
std::wstring pluginRunLogPath();

// 裝 unhandled exception filter（行程開始時一次）：當掉時記下出事的執行緒、模組、例外代碼，再交給原本的 filter（WER）。
void installPluginCrashFilter();

// 呼叫 plugin 時包住：進入時佔一格記下來，離開時清掉。紀錄沒開、或格子用完時什麼都不做。
class PluginCallScope {
public:
    PluginCallScope(const std::wstring& pluginId, const char* func, const std::wstring& path);
    ~PluginCallScope();
    PluginCallScope(const PluginCallScope&) = delete;
    PluginCallScope& operator=(const PluginCallScope&) = delete;

private:
    int slot_ = -1;
};

// 判斷結果：這個 plugin 害程式當掉。
struct PluginCrash {
    std::wstring pluginId;
    std::wstring func;               // L"probe"、L"readSize"、L"decode"、L"load"；只從模組認出來時是空的
    std::wstring path;               // 當時在處理的檔；不知道時是空的
    unsigned long exceptionCode = 0; // 0 = 沒經過 filter（__fastfail、被強制結束）
};

// 一個紀錄檔的內容。
struct PluginRunRecord {
    struct Busy {
        std::wstring pluginId, func, path;
        unsigned long threadId = 0;
    };
    bool cleanExit = false;
    bool crashed = false; // filter 跑到了
    unsigned long crashThreadId = 0, exceptionCode = 0;
    std::wstring crashModule; // 例外發生的位址屬於哪個模組（完整路徑）；查不到是空的
    std::vector<Busy> busy;   // 結束時還在 plugin 裡的執行緒
};

// 規則：正常結束 → 不怪。filter 有跑到 → 出事的執行緒正在 plugin 裡就怪那個 plugin；否則出事的模組在
// pluginsDir\<id>\ 底下就怪 <id>；都不是就不怪（別的執行緒剛好在 plugin 裡也不怪）。filter 沒跑到 → 還在 plugin 裡
// 的都怪（同一個 plugin 只算一次）。
std::vector<PluginCrash> judgePluginRun(const PluginRunRecord& rec, const std::wstring& pluginsDir);

// 啟動時呼叫：讀 dir 裡已經結束的行程留下的、同一個 exe 的紀錄檔，照 judgePluginRun 判斷，讀完刪掉。
// 還在跑的行程的（開不了）、別的 exe 的不碰。兩個 mpainter 同時啟動時只有一個讀得到同一個檔。
std::vector<PluginCrash> collectPluginCrashes(const std::wstring& dir, const std::wstring& exePath, const std::wstring& pluginsDir);

} // namespace mp
