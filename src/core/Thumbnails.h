#pragma once
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "core/Image.h"
#include "core/ImageIO.h"
#include "core/IoError.h"

namespace mp {

class ThumbCache;

// 一張 32 位元的 HBITMAP（shell 給的縮圖）→ premultiplied 的 Image。alpha 全是 0 的當成不透明；
// 顏色大於 alpha 的（不是 premultiplied）夾到 alpha。不是 32 位元或讀不到回 nullptr。hbitmap 不會被刪。
ImagePtr imageFromHBitmap(void* hbitmap);
// Windows 縮圖快取裡已經有的那一張（只查快取，不叫 Windows 現在算）；沒有回 nullptr。比 side 大的會縮下來。
// 會初始化這條執行緒的 COM（ensureThreadCom）。
ImagePtr windowsCachedThumbnail(const std::wstring& path, int side);

// 一張縮圖從哪裡來的。
enum class ThumbSource { Windows, Cache, Decoded, Failed };
struct ThumbResult {
    ImagePtr image; // Failed 時是 nullptr
    ThumbSource source = ThumbSource::Failed;
    IoError error;  // Failed 時的原因
};
// 拿一張邊長 side 以內的縮圖：Windows 縮圖快取有就用（useWindowsCache）→ 我們的快取（cache 可以是 nullptr）→
// 自己用快速解碼算（core/ImageIO 的 decodeScaled），算好的存進我們的快取。
ThumbResult loadThumbnail(const std::wstring& path, int side, ThumbCache* cache, bool useWindowsCache);

// 一件縮圖工作：哪一批（generation）、清單裡第幾個、檔案、邊長（實際像素）。
struct ThumbJob {
    uint64_t generation = 0;
    int index = -1;
    std::wstring path;
    int side = 0;
};

// 背景算縮圖的佇列：幾條執行緒從前面拿工作。新的要求排到最前面（畫面上看得到的先算）；
// 換資料夾或換大小時 reset 換一批，排隊中的舊工作丟掉，正在算的那幾張算完也照樣交回（呼叫端看 generation 丟掉）。
class ThumbQueue {
public:
    // job 與結果交給 done（在背景執行緒呼叫，不要在裡面等 UI）。
    using Done = std::function<void(const ThumbJob& job, ThumbResult&& result)>;
    // 自己算縮圖用的函式；預設是 loadThumbnail（測試換成會計數、會卡住的）。
    using Loader = std::function<ThumbResult(const std::wstring& path, int side)>;

    ThumbQueue() = default;
    ~ThumbQueue();
    ThumbQueue(const ThumbQueue&) = delete;
    ThumbQueue& operator=(const ThumbQueue&) = delete;

    void start(int threads, Loader loader, Done done);
    // 換成新的一批：排隊中的（不是這一批的）丟掉。
    void reset(uint64_t generation);
    // 排到最前面；同一批、同一個 index 已經在排就移到最前面。不是目前這一批的不收。
    void request(const ThumbJob& job);
    // 停下來：排隊中的丟掉，等正在算的做完、執行緒結束。之後可以再 start。
    void shutdown();
    size_t pending();

private:
    void run();

    std::mutex mu_;
    std::condition_variable cv_;
    std::deque<ThumbJob> jobs_;
    uint64_t generation_ = 0;
    bool stop_ = false;
    Loader loader_;
    Done done_;
    std::vector<std::thread> threads_;
};

// 預覽：一條背景執行緒，只做最新的那一個要求（選取換得很快時，中間的不做）。
// 結果交給 done（在背景執行緒呼叫）；ticket 是呼叫端給的編號，用來認出過時的結果。
class PreviewWorker {
public:
    using Done = std::function<void(uint64_t ticket, const std::wstring& path, ScaledDecode&& result)>;
    // 解碼用的函式；預設是 decodeScaled（帶 metadata）。
    using Decoder = std::function<ScaledDecode(const std::wstring& path, int maxW, int maxH)>;

    PreviewWorker() = default;
    ~PreviewWorker();
    PreviewWorker(const PreviewWorker&) = delete;
    PreviewWorker& operator=(const PreviewWorker&) = delete;

    void start(Decoder decoder, Done done);
    // 取代還沒開始的那一個要求。
    void request(uint64_t ticket, const std::wstring& path, int maxW, int maxH);
    // 丟掉還沒開始的要求。
    void cancel();
    void shutdown();

private:
    void run();

    struct Job {
        uint64_t ticket = 0;
        std::wstring path;
        int maxW = 0, maxH = 0;
    };
    std::mutex mu_;
    std::condition_variable cv_;
    bool has_ = false, stop_ = false;
    Job job_;
    Decoder decoder_;
    Done done_;
    std::thread thread_;
};

} // namespace mp
