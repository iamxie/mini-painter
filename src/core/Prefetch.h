#pragma once
#include <atomic>
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

#include "core/Archive.h"
#include "core/ImageIO.h"

namespace mp {

// 預先解碼：一條背景執行緒先解好翻頁方向的下一屏（高品質模式連縮圖一起做），翻走的上一屏也先留著；
// 翻到一本的盡頭時先準備同一層的下一本（壓縮檔先整包解壓）。只留「目前、下一屏、上一屏、下一本的第一屏」，
// 其他一律放掉；目前這屏以外的總量有上限。

// 寬高。
using Size2 = std::pair<int, int>;
// 一屏裡各頁的寬高 → 各頁高品質縮圖的尺寸（同序；{0, 0} 表示這頁不縮）。只算解碼成功的頁。
using TargetSizes = std::function<std::vector<Size2>(const std::vector<Size2>& pages)>;

// 換本預備的要求：目前這本 fromBook 往 dir 方向的鄰居（同一層的下一本或上一本）。
struct BookRequest {
    std::wstring fromBook;               // 目前這本：資料夾路徑或壓縮檔路徑
    int dir = 1;
    bool twoPage = false;                // 兩頁並排：要讀每頁寬高、算第一屏 / 最後一屏
    bool firstAlone = true;
    bool shiftOne = false;
    std::vector<std::wstring> pinned;    // 置頂檔名
    std::vector<std::wstring> passwords; // 壓縮檔加密時試的密碼列表
    std::wstring archiveRoot;            // 壓縮檔解到這底下的新子資料夾；空 = 不預開壓縮檔
    bool operator==(const BookRequest& o) const;
    bool operator!=(const BookRequest& o) const { return !(*this == o); }
};

// 準備好的一本。
struct BookPlan {
    std::wstring book;                 // 那一本的路徑；空 = 沒有鄰居
    bool archive = false;
    ArchiveResult extracted;           // 壓縮檔：解壓結果（status、expected / written；files / names 已套置頂）
    std::wstring dest;                 // 壓縮檔：解到哪（解失敗時已刪、這裡是空的）
    std::wstring password;             // 壓縮檔：用到的密碼（密碼列表裡的；空 = 沒加密）
    std::vector<std::wstring> files;   // 閱讀順序，已套置頂
    std::map<std::wstring, bool> wide; // 兩頁並排：每頁是不是寬大於高
    std::vector<std::wstring> edge;    // 跳過去第一眼看到的那一屏（dir > 0 第一屏、< 0 最後一屏），1～2 頁
    // 資料夾有圖，或壓縮檔解成功（含 Incomplete）。
    bool usable() const;
};

// 同步準備 book（Prefetcher 在背景執行緒呼叫，公開給測試）。資料夾：列檔、置頂。壓縮檔：解到 dest，加密時先試密碼列表，
// 都不對就不解（status 是 NeedPassword）。兩頁並排時讀每頁寬高。progress 回 false = 取消解壓。
BookPlan planBook(const std::wstring& book, const BookRequest& req, const std::wstring& dest,
                  const std::function<bool(uint64_t done, uint64_t total)>& progress);

class Prefetcher {
public:
    using Decoder = std::function<LoadResult(const std::wstring& path)>;
    // decode 空的話用 loadImage（測試可以換成會計數、會卡住的）。
    explicit Prefetcher(Decoder decode = {});
    ~Prefetcher();
    Prefetcher(const Prefetcher&) = delete;
    Prefetcher& operator=(const Prefetcher&) = delete;

    // 告訴背景要準備什麼。current：目前畫面上的頁（留著、不算上限、背景不處理）；others：依優先順序的其他屏
    // （通常是下一屏、上一屏）；book：換本預備，沒有給 nullopt；target：高品質縮圖尺寸，空 = 不縮；limitBytes：
    // current 以外最多佔多少。不在這些清單裡的立刻放掉；超過上限的從優先順序低的放掉。換了 book 時，正在解的壓縮檔取消、
    // 沒被接手的解壓資料夾在背景刪掉。
    void setWanted(std::vector<std::wstring> current, std::vector<std::vector<std::wstring>> others,
                   std::optional<BookRequest> book, TargetSizes target, size_t limitBytes);
    // 取消換本預備，等背景停下來才回傳（要刪暫存資料夾之前呼叫）。沒被接手的解壓資料夾在背景刪掉。
    void dropBook();
    // 停掉背景執行緒，放掉所有圖，刪掉沒被接手的解壓資料夾。之後不能再用（解構也會做）。
    void shutdown();

    // 拿 path 的解碼結果：有就給（解碼後檔案被改過就丟掉、當作沒有）；背景正在解這一頁就等它解完；沒有回 nullopt。
    std::optional<LoadResult> take(const std::wstring& path);
    // 自己解好的頁放進來（目前畫面上的頁），翻走再翻回來時不用重解。
    void put(const std::wstring& path, const LoadResult& r);
    // image 的高品質縮圖，尺寸要剛好 tw×th；背景正在縮這一張就等它。沒有回 nullptr。
    ImagePtr scaled(const ImagePtr& image, int tw, int th);
    // 換本：背景準備的就是 book 的話，等它做完再回傳（解壓資料夾從此歸呼叫端）；不是這本、或沒有在準備，回 nullopt。
    // 等的時候約每 50 ms 呼叫一次 progress（可為空）；它回 false 就取消解壓，回傳的 extracted.status 是 Cancelled。
    std::optional<BookPlan> takeBook(const std::wstring& book, const std::function<bool(uint64_t, uint64_t)>& progress);

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
        LoadResult result;
        Stamp stamp;
        ImagePtr scaled;
    };
    struct PathLess { // Windows 路徑不分大小寫
        bool operator()(const std::wstring& a, const std::wstring& b) const;
    };
    enum class Kind { None, Decode, Scale, Book };
    struct Task {
        Kind kind = Kind::None;
        std::wstring path;
        ImagePtr image;
        int tw = 0, th = 0;
        BookRequest book;
    };

    static Stamp stampOf(const std::wstring& path);
    static size_t bytesOf(const Entry& e);
    std::vector<std::vector<std::wstring>> screensLocked() const; // 依優先順序：下一本的那一屏、others
    bool isWantedLocked(const std::wstring& path) const;
    bool isCurrentLocked(const std::wstring& path) const;
    size_t othersBytesLocked() const;
    void pruneLocked();
    void resetBookLocked(std::optional<BookRequest> book); // 換掉換本預備：正在做的取消，沒被接手的解壓資料夾排進刪除
    bool pickLocked(Task& t);
    void run();
    void runBook(const BookRequest& req);

    Decoder decode_;
    mutable std::mutex m_;
    std::condition_variable work_; // 叫醒背景
    std::condition_variable done_; // 叫醒等結果的
    std::thread worker_;
    bool stop_ = false;
    bool idle_ = false;
    bool full_ = false;            // 這一輪已經到上限，setWanted 之前不再解新的

    std::map<std::wstring, Entry, PathLess> entries_;
    std::vector<std::wstring> current_;
    std::vector<std::vector<std::wstring>> others_;
    TargetSizes target_;
    size_t limit_ = 0;
    Task busy_;                    // 背景正在做的

    std::optional<BookRequest> bookReq_;
    std::optional<BookPlan> bookPlan_;
    bool bookRunning_ = false;
    bool bookTaken_ = false;       // 呼叫端拿走了（或取消了）：同一個要求不再重做，解壓資料夾不歸這裡刪
    std::wstring bookTarget_;      // 背景查到的鄰居；空 = 還沒查到，kNoBook = 沒有鄰居
    std::atomic<bool> bookCancel_{false};
    std::atomic<uint64_t> bookDone_{0}, bookTotal_{0};
    std::vector<std::wstring> orphanDirs_; // 要在背景刪的解壓資料夾
    int destSeq_ = 0;
};

} // namespace mp
