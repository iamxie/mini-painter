#pragma once
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

struct sqlite3;

namespace mp {

// 縮圖快取的檔名（放在壓縮檔暫存位置底下）。WAL 模式另外有同名加 -wal、-shm 的兩個檔。
// 名字是自己的：暫存位置的清理只認 <pid>-<8 位十六進位> 的資料夾，碰不到它（core/TempDir.h）。
inline constexpr wchar_t kThumbCacheFileName[] = L"thumbs.mpainter.db";

// 原檔的大小與修改時間：快取裡記的和現在的不一樣就當成沒有，重算。
struct ThumbStamp {
    uint64_t size = 0;
    int64_t mtime = 0; // FILETIME（100 奈秒）
    bool operator==(const ThumbStamp& o) const { return size == o.size && mtime == o.mtime; }
};
// 讀檔案的大小與修改時間。讀不到回 false。
bool fileStamp(const std::wstring& path, ThumbStamp& out);
// 路徑在網路磁碟上（UNC，或對應到網路的磁碟機代號）。SQLite 的 WAL 不能用在網路檔案系統，這種位置不開快取。
bool isNetworkPath(const std::wstring& path);
// 快取檔與 WAL 的兩個檔加起來多大（設定面板顯示用量）。
uint64_t thumbCacheFileBytes(const std::wstring& dbPath);

// 自己算的縮圖存在一個 SQLite 資料庫裡（使用者 2026-11-24 定）：一個檔，不在原圖的資料夾留東西。
// key 是完整路徑（轉小寫）加縮圖的邊長；另記原檔的大小與修改時間，對不上就當成沒有。
// 超過容量上限時丟最久沒用到的。WAL 模式：好幾個 mpainter 視窗可以同時讀寫同一個檔。
// 所有函式可以在任何執行緒呼叫（內部用一個 mutex 排隊）。資料庫壞掉時整個刪掉重建（縮圖本來就能重算）。
class ThumbCache {
public:
    ThumbCache() = default;
    ~ThumbCache();
    ThumbCache(const ThumbCache&) = delete;
    ThumbCache& operator=(const ThumbCache&) = delete;

    // 開（沒有就建）dbPath。網路磁碟上不開。失敗回 false，之後 get / put 都當成沒有快取。
    bool open(const std::wstring& dbPath, uint64_t limitBytes);
    void close();
    bool isOpen() const;
    const std::wstring& path() const { return path_; }

    // 有、而且 stamp 對得上就把存的位元組放進 out。對不上的那一筆順便刪掉。用到的那一筆記成最近用過。
    bool get(const std::wstring& imagePath, int side, const ThumbStamp& stamp, std::vector<uint8_t>& out);
    // 存（同一個 key 取代舊的）。存完超過上限就丟最久沒用到的。
    bool put(const std::wstring& imagePath, int side, const ThumbStamp& stamp, const std::vector<uint8_t>& data);
    // 改容量上限，超過的立刻丟掉。0 = 全部丟掉。
    void setLimit(uint64_t limitBytes);
    // 存的縮圖加起來多少位元組（只算資料，不含資料庫本身的空間）。
    uint64_t totalBytes();
    // 全部清掉，檔案縮回去。
    void clear();
    // 筆數（測試用）。
    int count();

private:
    int openLocked(); // 回 SQLite 的結果碼
    void closeLocked();
    bool resetLocked(); // 資料庫壞了：關掉、刪檔、重建
    bool execLocked(const char* sql);
    uint64_t sumBytesLocked();
    void trimLocked();
    bool failedLocked(int rc); // rc 是壞掉的那種錯誤就重建，回 true

    std::mutex mu_;
    sqlite3* db_ = nullptr;
    std::wstring path_;
    uint64_t limit_ = 0;
    uint64_t approxTotal_ = 0; // 估計的總量：開檔時算一次，之後每存一筆加上去；超過上限才重新精算（別的視窗也在寫）
};

} // namespace mp
