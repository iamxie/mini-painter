#pragma once
#include <windows.h>

#include <string>
#include <vector>

#include "core/Lang.h"
#include "core/Settings.h"

namespace mp {

// 一個視窗的設定：啟動時讀設定檔與選語系，之後握著目前的值，存檔時和別的視窗寫的合併。
// 設定檔（mpainter.ini）在 exe 旁邊；額外的語系檔在 exe 旁邊的 lang\。
class AppSettings {
public:
    // 目前的值。直接讀寫，改完呼叫 save()。
    Settings values;

    // 啟動時呼叫一次。exePath：這個 exe 的完整路徑。module：內建語系（繁中）的資源在哪個模組，nullptr 就沒有內建的。
    // 讀設定檔（在、但被別的程式獨佔時重試，最多約 1 秒），再依設定與系統語系選一個語系啟用。
    void load(const std::wstring& exePath, HMODULE module);

    // 存檔。別的視窗可能也改過設定檔：只把這個視窗改過的欄位寫進去，其他的留檔案裡現在的值（core 的 mergeSettings），
    // 再把別的視窗記下、不影響這個視窗畫面的幾樣收下來（密碼、問過預設程式了、提示看過幾次、略過的版本）。
    // 啟動時讀不到設定檔的話整次執行都不存（手上是預設值，存了會蓋掉原本的設定）。回傳有沒有寫進去。
    bool save();

    const std::wstring& exePath() const { return exePath_; }
    std::wstring exeDir() const; // exe 所在資料夾，結尾帶反斜線
    const std::wstring& path() const { return path_; } // 設定檔的路徑
    // 啟動時設定檔在但讀不到：這次執行用預設值，而且不存檔。
    bool unreadable() const { return unreadable_; }
    // 設定檔寫不寫得進去（最近一次存檔或 refreshWritable 的結果）。
    bool writable() const { return writable_; }
    void refreshWritable();
    // 最近一次 save() 寫不進去的原因（Win32 錯誤碼）。存成功、沒有存（unreadable）、或原因不明時是 0。
    unsigned long saveError() const { return saveError_; }
    // 可選的語系，順序同語言選單：英文、內建的、lang\ 裡掃到的（同代碼的檔蓋掉內建的）。
    const std::vector<LangTable>& languages() const { return langs_; }

private:
    void initLanguage(HMODULE module);

    Settings base_; // 上次和設定檔同步時的 values：存檔時用它分辨哪些欄位是這個視窗改的
    std::wstring exePath_, path_;
    bool writable_ = true;
    bool unreadable_ = false;
    unsigned long saveError_ = 0;
    std::vector<LangTable> langs_;
};

} // namespace mp
