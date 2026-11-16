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
#include <vector>

#include "core/Archive.h"

namespace mp {

// 換本預備：翻到一本的盡頭時，背景先準備同一層的下一本（或上一本）。壓縮檔先整包解壓，
// 兩頁並排時先讀每頁寬高。跳過去時由畫面那一側接手（BookPreparer::take）。

// 換本預備的要求：目前這本 fromBook 往 dir 方向的鄰居。
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
    bool notPrepared = false;          // 壓縮檔，但這次不能預開（沒給解壓的位置、7z.dll 還沒載入）：跳過去時照一般的流程開
    ArchiveResult extracted;           // 壓縮檔：解壓結果（status、expected / written；files / names 已套置頂）
    std::wstring dest;                 // 壓縮檔：解到哪（解失敗時已刪、這裡是空的）
    std::wstring password;             // 壓縮檔：用到的密碼（密碼列表裡的；空 = 沒加密）
    bool passwordsTried = false;       // 壓縮檔：密碼列表試過一輪、都不對（接手的人不必再試，直接問使用者）
    std::vector<std::wstring> files;   // 閱讀順序，已套置頂
    std::map<std::wstring, bool> wide; // 兩頁並排：每頁是不是寬大於高
    std::vector<std::wstring> edge;    // 跳過去第一眼看到的那一屏（dir > 0 第一屏、< 0 最後一屏），1～2 頁
    // 資料夾有圖，或壓縮檔解成功（含 Incomplete）。
    bool usable() const;
};

// 某個檔是不是寬大於高（讀檔頭）。兩頁並排的配對用；畫面與換本預備都問這一個。讀不到當成不是。
bool isWidePage(const std::wstring& path);

// 同步準備 book（BookPreparer 在背景執行緒呼叫，公開給測試）。資料夾：列檔、置頂。壓縮檔：解到 dest，加密時試密碼列表
// （extractWithPasswords），都不對就不解（status 是 NeedPassword）。兩頁並排時讀每頁寬高。
// progress 回 false = 取消（解壓、試密碼、讀寬高的途中都會看），這時 extracted.status 是 Cancelled。
BookPlan planBook(const std::wstring& book, const BookRequest& req, const std::wstring& dest,
                  const std::function<bool(uint64_t done, uint64_t total)>& progress);

// 一條背景執行緒，一次準備一本。
//
// 狀態：
//   Idle    沒有要求
//   Queued  有要求，還沒開始
//   Running 背景正在查鄰居、解壓
//   Ready   準備好了（plan 有內容；可能是「沒有鄰居」或「解不開」）
//   Taken   被接手了（take 拿走，或等的時候取消）：同一個要求不再重做，解壓資料夾歸接手的人
// 換了要求就回到 Queued / Idle；正在做的那一份作廢（解到一半的資料夾在背景刪掉）。
//
// 執行緒：每個函式旁邊寫了「誰可以呼叫、會不會等」。回呼（onReady、take 的 progress）都在沒有持鎖的時候呼叫。
class BookPreparer {
public:
    // onReady：準備好一本的時候在背景執行緒呼叫一次（給 Prefetcher 知道可以開始解那一本的第一屏）。
    explicit BookPreparer(std::function<void()> onReady = {});
    ~BookPreparer();
    BookPreparer(const BookPreparer&) = delete;
    BookPreparer& operator=(const BookPreparer&) = delete;

    // 任何執行緒，不等。換一個要求（nullopt = 不要了）。和目前的要求相同就不動。
    void request(std::optional<BookRequest> req);
    // 任何執行緒，會等：取消要求，等背景停手、而且沒被接手的解壓資料夾都刪完才回來。
    // 呼叫端接著要刪整個暫存資料夾時用，這樣不會兩邊同時刪同一棵樹。
    void drop();
    // 任何執行緒，會等：背景準備的就是 book 的話，等它做完再回傳（解壓資料夾從此歸呼叫端）；不是這本、沒有在準備、
    // 已經被接手過，回 nullopt。等的時候約每 50 ms 呼叫一次 progress（可為空）；它回 false 就取消，
    // 回傳的 extracted.status 是 Cancelled。
    std::optional<BookPlan> take(const std::wstring& book, const std::function<bool(uint64_t, uint64_t)>& progress);
    // 任何執行緒，不等：準備好而且能用的那一本，跳過去第一眼看到的那一屏（被接手之後也還給）；沒有回空的。
    std::vector<std::wstring> edge() const;
    // 任何執行緒，會等：停掉背景執行緒，刪掉沒被接手的解壓資料夾。之後不能再用（解構也會做）。
    void shutdown();
    // 測試用，會等：背景沒事做（沒有排隊的要求、沒有在刪資料夾）。
    void waitIdle();

private:
    enum class State { Idle, Queued, Running, Ready, Taken };
    enum class Neighbor { Unknown, None, Found };

    void run();
    void prepare(const BookRequest& req, uint64_t generation);
    void orphanLocked(const std::wstring& dir); // 排進「背景刪掉」的清單

    std::function<void()> onReady_;
    mutable std::mutex m_;
    std::condition_variable work_; // 叫醒背景
    std::condition_variable done_; // 叫醒等結果的
    std::thread worker_;
    bool stop_ = false;
    bool workerBusy_ = false;      // 背景正在準備一本，或正在刪資料夾

    State state_ = State::Idle;
    uint64_t generation_ = 0;      // 每換一次要求加一：背景做完時對不上就作廢
    std::optional<BookRequest> req_;
    BookPlan plan_;                // state_ 是 Ready / Taken 時有內容
    Neighbor neighbor_ = Neighbor::Unknown;
    std::wstring neighborPath_;
    std::atomic<bool> cancel_{false};
    std::atomic<uint64_t> progressDone_{0}, progressTotal_{0};
    std::vector<std::wstring> orphanDirs_; // 要在背景刪的解壓資料夾
    int destSeq_ = 0;
};

} // namespace mp
