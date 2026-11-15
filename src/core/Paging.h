#pragma once
#include <functional>
#include <map>
#include <string>
#include <vector>

#include "core/PathUtil.h"
#include "core/Spreads.h"

namespace mp {

// 翻頁、兩頁並排的配對快取、預先解碼要準備哪些頁、標題列的字、壓縮檔內的檔名：
// MainWindow 裡不碰視窗的那部分邏輯，搬到這裡才測得到。

// ---- 兩頁並排的配對快取 ----

// 目前這本（資料夾清單）的配對結果，連同「每一頁是不是寬圖」的紀錄。
// 清單（內容或順序）、選項變了，下一次 get 會自己重算，不必有人記得通知；
// 只有檔案內容變了（存檔改了寬高）要呼叫 invalidateFile，這個類別看不到。
class SpreadCache {
public:
    // 某個檔是不是寬大於高（要讀檔頭）。每個檔只會被問一次。
    using IsWide = std::function<bool(const std::wstring& path)>;

    // 這份清單在這組選項下的配對。和上一次的清單、選項都相同就直接回上次的結果。
    // 寬圖紀錄只留目前清單裡的檔（換了資料夾或壓縮檔，舊的不會再用到）。
    const std::vector<Spread>& get(const std::vector<std::wstring>& files, bool firstAlone, bool shiftOne, const IsWide& isWide);
    // 最近一次 get 的結果（還沒 get 過是空的）。
    const std::vector<Spread>& spreads() const { return spreads_; }
    // 這個檔的寬高變了：忘掉它的紀錄，下一次 get 重新讀、重新配對。路徑不分大小寫。
    void invalidateFile(const std::wstring& path);
    // 別處已經知道這個檔是不是寬圖（換本預備在背景讀過）：先記下來，get 就不用再問。
    void remember(const std::wstring& path, bool wide);
    // 全部忘掉。
    void clear();

private:
    struct PathLess { // Windows 路徑不分大小寫
        bool operator()(const std::wstring& a, const std::wstring& b) const;
    };
    std::vector<Spread> spreads_;
    std::vector<std::wstring> files_;
    bool firstAlone_ = false, shiftOne_ = false, valid_ = false;
    std::map<std::wstring, bool, PathLess> wide_;
};

// ---- 翻頁 ----

// 往 dir（+1 下一頁、-1 上一頁）翻一次要開清單裡的哪一個檔；已經在那個方向的盡頭、或不在清單裡，回 -1。
// spreads 為 nullptr 是單頁；兩頁並排給配對結果，翻的是一屏，回傳那一屏的第一頁。
int pageTurnTarget(int dir, int index, int count, const std::vector<Spread>* spreads);

// 預先解碼要準備的頁。
struct WantedPages {
    std::vector<std::vector<std::wstring>> others; // 依優先順序的其他屏：翻頁方向的下一屏，再來是上一屏
    bool atEdge = false;                           // 翻頁方向已經沒有下一屏（到一本的盡頭）
};
// files 是清單、index 是目前這頁、lastDir 是最近一次翻頁的方向。spreads 同 pageTurnTarget。
WantedPages wantedPages(const std::vector<std::wstring>& files, int index, int lastDir, const std::vector<Spread>* spreads);

// ---- 標題列 ----

struct TitleInfo {
    std::wstring name;        // 目前這頁的顯示名；空 = 沒有開檔
    std::wstring secondName;  // 兩頁並排時同一屏的另一頁；空 = 單頁
    bool dirty = false;       // 有沒存的修改：檔名後面加 *
    bool rightToLeft = false; // 兩頁並排：右到左（日漫），檔名的左右順序跟著畫面
    int index = -1;           // 目前這頁在清單裡的位置（從 0 起）；-1 = 不在清單
    int count = 0;            // 清單的檔數；只有一個檔時不顯示頁碼
    int spreadFirst = -1;     // 兩頁並排：這一屏兩頁的位置；-1 = 不知道
    int spreadSecond = -1;
    std::wstring profile;     // 目前的模式名；空 = 不顯示
    bool profileModified = false; // 有臨時調整：模式名後面加 *
    std::wstring appName;
};
// 「檔名* (n/N) - 模式名* - mpainter」；兩頁並排是「左 | 右* (a-b/N) - …」。
std::wstring composeTitle(const TitleInfo& t);

// ---- 壓縮檔內的檔名 ----

// path 是不是 dir 底下的檔（不分大小寫；dir 自己不算）。dir 是空的回 false。
bool isInsideDir(const std::wstring& dir, const std::wstring& path);
// 壓縮檔解到 dir：files 是解出來的檔（磁碟檔名是序號）、names 是同序的壓縮檔內名字。
// path 在壓縮檔內的名字；不在 dir 底下回空字串；在 dir 底下但對不到（不該發生）回磁碟檔名。
std::wstring archiveEntryName(const std::wstring& dir, const std::vector<std::wstring>& files,
                              const std::vector<std::wstring>& names, const std::wstring& path);

} // namespace mp
