#include "core/ThumbCache.h"

#include <windows.h>

#include <sqlite3.h>

#include "core/TextUtil.h"

namespace mp {

namespace {

constexpr int kBusyTimeoutMs = 3000;       // 別的視窗正在寫時等多久
constexpr double kTrimTarget = 0.9;        // 超過上限時丟到剩上限的九成，不必每存一張就丟一次

// 一個 prepared statement，離開範圍時收掉。
class Stmt {
public:
    Stmt(sqlite3* db, const char* sql) { rc_ = sqlite3_prepare_v2(db, sql, -1, &s_, nullptr); }
    ~Stmt() { sqlite3_finalize(s_); }
    Stmt(const Stmt&) = delete;
    Stmt& operator=(const Stmt&) = delete;
    bool ok() const { return rc_ == SQLITE_OK && s_; }
    int rc() const { return rc_; }
    sqlite3_stmt* get() const { return s_; }
    void text(int i, const std::string& v) { sqlite3_bind_text(s_, i, v.c_str(), static_cast<int>(v.size()), SQLITE_TRANSIENT); }
    void int64(int i, int64_t v) { sqlite3_bind_int64(s_, i, v); }
    void blob(int i, const std::vector<uint8_t>& v) {
        sqlite3_bind_blob(s_, i, v.empty() ? "" : static_cast<const void*>(v.data()), static_cast<int>(v.size()), SQLITE_TRANSIENT);
    }

private:
    sqlite3_stmt* s_ = nullptr;
    int rc_ = SQLITE_ERROR;
};

bool isCorruption(int rc) {
    const int primary = rc & 0xff;
    return primary == SQLITE_CORRUPT || primary == SQLITE_NOTADB;
}

std::string keyOf(const std::wstring& path) { return wideToUtf8(lowerCopy(path)); }

uint64_t fileBytes(const std::wstring& path) {
    WIN32_FILE_ATTRIBUTE_DATA a{};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &a)) return 0;
    return (static_cast<uint64_t>(a.nFileSizeHigh) << 32) | a.nFileSizeLow;
}

} // namespace

bool fileStamp(const std::wstring& path, ThumbStamp& out) {
    WIN32_FILE_ATTRIBUTE_DATA a{};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &a)) return false;
    if (a.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) return false;
    out.size = (static_cast<uint64_t>(a.nFileSizeHigh) << 32) | a.nFileSizeLow;
    out.mtime = static_cast<int64_t>((static_cast<uint64_t>(a.ftLastWriteTime.dwHighDateTime) << 32) | a.ftLastWriteTime.dwLowDateTime);
    return true;
}

bool isNetworkPath(const std::wstring& path) {
    if (path.size() >= 2 && (path[0] == L'\\' || path[0] == L'/') && (path[1] == L'\\' || path[1] == L'/')) {
        // \\?\C:\... 是本機路徑的長路徑寫法，\\?\UNC\... 才是網路
        if (path.size() >= 4 && path[2] == L'?' && path[3] == L'\\') return _wcsnicmp(path.c_str() + 4, L"UNC\\", 4) == 0;
        return true;
    }
    if (path.size() >= 2 && path[1] == L':') {
        const wchar_t root[] = {path[0], L':', L'\\', 0};
        return GetDriveTypeW(root) == DRIVE_REMOTE;
    }
    return false;
}

uint64_t thumbCacheFileBytes(const std::wstring& dbPath) {
    return fileBytes(dbPath) + fileBytes(dbPath + L"-wal") + fileBytes(dbPath + L"-shm");
}

ThumbCache::~ThumbCache() { close(); }

bool ThumbCache::open(const std::wstring& dbPath, uint64_t limitBytes) {
    std::lock_guard<std::mutex> lock(mu_);
    closeLocked();
    path_ = dbPath;
    limit_ = limitBytes;
    if (dbPath.empty() || limitBytes == 0 || isNetworkPath(dbPath)) return false;
    const int rc = openLocked();
    if (rc == SQLITE_OK) return true;
    // 只有「檔案壞了、不是資料庫」才刪掉重建；別的原因（例如別的視窗鎖太久）不動別人正在用的檔。
    return isCorruption(rc) && resetLocked();
}

void ThumbCache::close() {
    std::lock_guard<std::mutex> lock(mu_);
    closeLocked();
}

bool ThumbCache::isOpen() const { return db_ != nullptr; }

int ThumbCache::openLocked() {
    int rc = sqlite3_open_v2(wideToUtf8(path_).c_str(), &db_, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, nullptr);
    if (rc != SQLITE_OK) {
        closeLocked();
        return rc;
    }
    sqlite3_busy_timeout(db_, kBusyTimeoutMs);
    // auto_vacuum 要在建表之前設（新檔才有效）：刪掉的縮圖之後用 incremental_vacuum 把空間還回去。
    const char* const setup[] = {
        "PRAGMA auto_vacuum=INCREMENTAL",
        "PRAGMA journal_mode=WAL",
        "PRAGMA synchronous=NORMAL",
        "CREATE TABLE IF NOT EXISTS thumbs(key TEXT NOT NULL, side INTEGER NOT NULL, fsize INTEGER NOT NULL,"
        " mtime INTEGER NOT NULL, used INTEGER NOT NULL, bytes INTEGER NOT NULL, data BLOB NOT NULL, PRIMARY KEY(key, side))",
        "CREATE INDEX IF NOT EXISTS thumbs_used ON thumbs(used)",
    };
    for (const char* sql : setup) {
        rc = sqlite3_exec(db_, sql, nullptr, nullptr, nullptr);
        if (rc != SQLITE_OK) {
            closeLocked();
            return rc;
        }
    }
    approxTotal_ = sumBytesLocked();
    return SQLITE_OK;
}

void ThumbCache::closeLocked() {
    if (db_) sqlite3_close_v2(db_);
    db_ = nullptr;
    approxTotal_ = 0;
}

bool ThumbCache::resetLocked() {
    closeLocked();
    DeleteFileW(path_.c_str());
    DeleteFileW((path_ + L"-wal").c_str());
    DeleteFileW((path_ + L"-shm").c_str());
    return openLocked() == SQLITE_OK;
}

bool ThumbCache::failedLocked(int rc) {
    if (!isCorruption(rc)) return false;
    resetLocked();
    return true;
}

bool ThumbCache::execLocked(const char* sql) {
    if (!db_) return false;
    const int rc = sqlite3_exec(db_, sql, nullptr, nullptr, nullptr);
    return rc == SQLITE_OK;
}

uint64_t ThumbCache::sumBytesLocked() {
    if (!db_) return 0;
    Stmt q(db_, "SELECT IFNULL(SUM(bytes), 0) FROM thumbs");
    if (!q.ok() || sqlite3_step(q.get()) != SQLITE_ROW) return 0;
    return static_cast<uint64_t>(sqlite3_column_int64(q.get(), 0));
}

bool ThumbCache::get(const std::wstring& imagePath, int side, const ThumbStamp& stamp, std::vector<uint8_t>& out) {
    std::lock_guard<std::mutex> lock(mu_);
    if (!db_) return false;
    const std::string key = keyOf(imagePath);
    int rc;
    {
        Stmt q(db_, "SELECT fsize, mtime, data FROM thumbs WHERE key=?1 AND side=?2");
        if (!q.ok()) { failedLocked(q.rc()); return false; }
        q.text(1, key);
        q.int64(2, side);
        rc = sqlite3_step(q.get());
        if (rc == SQLITE_ROW) {
            const ThumbStamp have{static_cast<uint64_t>(sqlite3_column_int64(q.get(), 0)), sqlite3_column_int64(q.get(), 1)};
            if (have == stamp) {
                const auto* p = static_cast<const uint8_t*>(sqlite3_column_blob(q.get(), 2));
                out.assign(p, p + sqlite3_column_bytes(q.get(), 2));
            } else {
                rc = SQLITE_DONE; // 原檔改過：舊的那一筆刪掉
                Stmt d(db_, "DELETE FROM thumbs WHERE key=?1 AND side=?2");
                if (d.ok()) {
                    d.text(1, key);
                    d.int64(2, side);
                    sqlite3_step(d.get());
                }
                return false;
            }
        }
    }
    if (rc != SQLITE_ROW) {
        if (rc != SQLITE_DONE) failedLocked(rc);
        return false;
    }
    Stmt u(db_, "UPDATE thumbs SET used=(SELECT IFNULL(MAX(used), 0) + 1 FROM thumbs) WHERE key=?1 AND side=?2");
    if (u.ok()) {
        u.text(1, key);
        u.int64(2, side);
        sqlite3_step(u.get()); // 記不了最近用過（例如別的視窗正鎖著）不影響這次讀到的
    }
    return true;
}

bool ThumbCache::put(const std::wstring& imagePath, int side, const ThumbStamp& stamp, const std::vector<uint8_t>& data) {
    std::lock_guard<std::mutex> lock(mu_);
    if (!db_) return false;
    Stmt q(db_, "INSERT OR REPLACE INTO thumbs(key, side, fsize, mtime, used, bytes, data)"
                " VALUES(?1, ?2, ?3, ?4, (SELECT IFNULL(MAX(used), 0) + 1 FROM thumbs), ?5, ?6)");
    if (!q.ok()) { failedLocked(q.rc()); return false; }
    q.text(1, keyOf(imagePath));
    q.int64(2, side);
    q.int64(3, static_cast<int64_t>(stamp.size));
    q.int64(4, stamp.mtime);
    q.int64(5, static_cast<int64_t>(data.size()));
    q.blob(6, data);
    const int rc = sqlite3_step(q.get());
    if (rc != SQLITE_DONE) {
        failedLocked(rc);
        return false;
    }
    approxTotal_ += data.size();
    if (approxTotal_ > limit_) trimLocked();
    return true;
}

void ThumbCache::trimLocked() {
    approxTotal_ = sumBytesLocked(); // 別的視窗也在寫：丟之前精算一次
    if (approxTotal_ <= limit_) return;
    const uint64_t target = static_cast<uint64_t>(static_cast<double>(limit_) * kTrimTarget);
    uint64_t excess = approxTotal_ - target, freed = 0;
    int64_t cut = -1;
    {
        Stmt q(db_, "SELECT used, bytes FROM thumbs ORDER BY used ASC");
        if (!q.ok()) return;
        while (freed < excess && sqlite3_step(q.get()) == SQLITE_ROW) {
            cut = sqlite3_column_int64(q.get(), 0);
            freed += static_cast<uint64_t>(sqlite3_column_int64(q.get(), 1));
        }
    }
    if (cut < 0) return;
    Stmt d(db_, "DELETE FROM thumbs WHERE used <= ?1");
    if (!d.ok()) return;
    d.int64(1, cut);
    if (sqlite3_step(d.get()) != SQLITE_DONE) return;
    execLocked("PRAGMA incremental_vacuum");
    approxTotal_ = sumBytesLocked();
}

void ThumbCache::setLimit(uint64_t limitBytes) {
    std::lock_guard<std::mutex> lock(mu_);
    limit_ = limitBytes;
    if (!db_) return;
    if (limit_ == 0) {
        execLocked("DELETE FROM thumbs");
        execLocked("PRAGMA incremental_vacuum");
        approxTotal_ = 0;
        return;
    }
    trimLocked();
}

uint64_t ThumbCache::totalBytes() {
    std::lock_guard<std::mutex> lock(mu_);
    return sumBytesLocked();
}

void ThumbCache::clear() {
    std::lock_guard<std::mutex> lock(mu_);
    if (!db_) return;
    execLocked("DELETE FROM thumbs");
    execLocked("PRAGMA incremental_vacuum");
    execLocked("PRAGMA wal_checkpoint(TRUNCATE)");
    approxTotal_ = 0;
}

int ThumbCache::count() {
    std::lock_guard<std::mutex> lock(mu_);
    if (!db_) return 0;
    Stmt q(db_, "SELECT COUNT(*) FROM thumbs");
    if (!q.ok() || sqlite3_step(q.get()) != SQLITE_ROW) return 0;
    return sqlite3_column_int(q.get(), 0);
}

} // namespace mp
