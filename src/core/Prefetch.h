#pragma once
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "core/BookPrep.h"
#include "core/ImageIO.h"

namespace mp {

// 預先解碼：一條背景執行緒先解好翻頁方向的下一屏（高品質模式連縮圖一起做），翻走的上一屏也先留著；
// 翻到一本的盡頭時另一條背景執行緒先準備同一層的下一本（BookPreparer，壓縮檔先整包解壓），準備好之後這裡接著解它的第一屏。
// 只留「目前、下一屏、上一屏、下一本的第一屏」，其他一律放掉。
//
// 上限的規則只有一條（fitsLocked / pruneLocked）：目前這屏不算；其他屏依優先順序一屏一屏累加，
// 放不下一整屏就不留那一屏，它後面的也都不留。背景開始解一屏之前先讀檔頭估整屏的大小，放不下就不解；
// 縮圖的大小事先就知道，放不下就不縮。

// 寬高。
using Size2 = std::pair<int, int>;
// 一屏裡各頁的寬高 → 各頁高品質縮圖的尺寸（同序；{0, 0} 表示這頁不縮）。只算解碼成功的頁。
// 會在背景執行緒被呼叫（不持鎖），所以只能用以值捕捉的東西，不能碰視窗或 this。
using TargetSizes = std::function<std::vector<Size2>(const std::vector<Size2>& pages)>;

// 執行緒：每個函式旁邊寫了「誰可以呼叫、會不會等」。「會等」的最久等到背景手上那一件做完（解一頁、縮一張）。
class Prefetcher {
public:
    using Decoder = std::function<LoadResult(const std::wstring& path)>;
    // decode 空的話用 loadImage（測試可以換成會計數、會卡住的）。decode 在背景執行緒被呼叫。
    explicit Prefetcher(Decoder decode = {});
    ~Prefetcher();
    Prefetcher(const Prefetcher&) = delete;
    Prefetcher& operator=(const Prefetcher&) = delete;

    // UI 執行緒，不等。告訴背景要準備什麼。current：目前畫面上的頁（留著、不算上限、背景不處理）；others：依優先順序的其他屏
    // （通常是下一屏、上一屏）；book：換本預備，沒有給 nullopt；target：高品質縮圖尺寸，空 = 不縮；limitBytes：
    // current 以外最多佔多少。不在這些清單裡的立刻放掉；放不下的那一屏起都放掉。
    void setWanted(std::vector<std::wstring> current, std::vector<std::vector<std::wstring>> others,
                   std::optional<BookRequest> book, TargetSizes target, size_t limitBytes);
    // UI 執行緒，會等。取消換本預備：等背景停手、沒被接手的解壓資料夾刪完才回來（要刪暫存資料夾之前呼叫）。
    void dropBook();
    // UI 執行緒，會等。停掉背景執行緒，放掉所有圖，刪掉沒被接手的解壓資料夾。之後不能再用（解構也會做）。
    void shutdown();

    // UI 執行緒，會等。path 的解碼結果（回傳的是複本，快取裡那一份還在）：有而且解成功就給；背景正在解這一頁就等它解完。
    // 沒有、解碼後檔案被改過、或當時解失敗（例如檔案剛好被別的程式鎖住），回 nullopt：呼叫端自己再解一次。
    std::optional<LoadResult> get(const std::wstring& path);
    // UI 執行緒，不等。自己解好的頁放進來（目前畫面上的頁），翻走再翻回來時不用重解。
    void put(const std::wstring& path, const LoadResult& r);
    // UI 執行緒，會等。image 的高品質縮圖，尺寸要剛好 tw×th；背景正在縮這一張就等它。沒有回 nullptr。
    ImagePtr scaled(const ImagePtr& image, int tw, int th);
    // 任何執行緒（實際上是進度框的工作執行緒，這時 UI 執行緒在跑訊息迴圈），會等。見 BookPreparer::take。
    std::optional<BookPlan> takeBook(const std::wstring& book, const std::function<bool(uint64_t, uint64_t)>& progress) {
        return books_.take(book, progress);
    }

    // 測試用：current 以外的圖（含縮圖）佔多少 bytes；等背景沒事做。
    size_t heldBytes() const;
    void waitIdle();

private:
    struct Stamp {
        uint64_t size = 0, writeTime = 0;
        bool ok = false;
        bool operator==(const Stamp& o) const { return ok == o.ok && size == o.size && writeTime == o.writeTime; }
    };
    struct Entry {
        LoadResult result; // result.image 是 nullptr = 當時解失敗（留著只是讓背景不要一直重試，get 不會把它給出去）
        Stamp stamp;
        ImagePtr scaled;
        // 這一頁高品質縮圖該是多大（{0, 0} = 不縮）；wantGen 對不上 targetGen_ 表示還沒算過。
        Size2 want{0, 0};
        uint64_t wantGen = 0;
    };
    struct PathLess { // Windows 路徑不分大小寫
        bool operator()(const std::wstring& a, const std::wstring& b) const;
    };
    using Screen = std::vector<std::wstring>;
    enum class Kind { None, Decode, Targets, Scale };
    struct Task {
        Kind kind = Kind::None;
        std::wstring path;           // Decode、Scale：哪一頁
        Screen screen;               // Decode：它所在的那一屏（估整屏的大小用）
        ImagePtr image;              // Scale：要縮的圖
        int tw = 0, th = 0;
        std::vector<std::wstring> paths; // Targets：這一屏解好的頁
        std::vector<ImagePtr> images;
        TargetSizes target;
        uint64_t targetGen = 0;
    };

    static Stamp stampOf(const std::wstring& path);
    static size_t bytesOf(const Entry& e);
    std::vector<Screen> screensLocked() const; // 依優先順序：下一本的那一屏、others
    bool isWantedLocked(const std::wstring& path) const;
    bool isCurrentLocked(const std::wstring& path) const;
    size_t othersBytesLocked() const;
    // current 以外再多 extra 還在上限內。
    bool fitsLocked(size_t extra) const { return othersBytesLocked() + extra <= limit_; }
    void pruneLocked();
    void dropScreenLocked(const Screen& screen); // 這一屏放不下：它的頁都不留（current 除外）
    // 有東西被放掉、上限或要準備的清單變了：之前「放不下、先停手」的判斷要重來。
    void spaceChangedLocked() { full_ = false; }
    bool pickLocked(Task& t);
    void run();
    void runDecode(const Task& t);
    void runTargets(const Task& t);
    void runScale(const Task& t);

    Decoder decode_;
    mutable std::mutex m_;
    std::condition_variable work_; // 叫醒背景
    std::condition_variable done_; // 叫醒等結果的
    std::thread worker_;
    bool stop_ = false;
    bool idle_ = false;
    bool full_ = false;            // 下一件要做的放不下：有東西被放掉之前不再解新的（見 spaceChangedLocked）

    std::map<std::wstring, Entry, PathLess> entries_;
    Screen current_;
    std::vector<Screen> others_;
    TargetSizes target_;
    uint64_t targetGen_ = 0;       // 每次 setWanted 加一（縮圖尺寸可能變了）
    size_t limit_ = 0;
    Task busy_;                    // 背景正在做的

    BookPreparer books_;           // 宣告在最後：先解構，它的 onReady 不會在這個物件拆到一半時進來
};

} // namespace mp
