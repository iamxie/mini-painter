#pragma once
#include <windows.h>

#include <cstdint>
#include <string>

namespace mp {

// 壓縮檔解壓的暫存位置。預設 exe 旁的 temp（portable，使用者看得到）；設定裡填了路徑就用設定的；
// 預設位置建不了（exe 在唯讀的地方）時由呼叫端退回 fallbackUnzipRoot()（%TEMP%\mpainter\unzip）。
std::wstring defaultUnzipRoot(const std::wstring& exeDir);
std::wstring fallbackUnzipRoot();
std::wstring resolveUnzipRoot(const std::wstring& setting, const std::wstring& exeDir);

// 遞迴加總資料夾裡所有檔案的大小。不存在回 0。不進連結點。
uint64_t directorySize(const std::wstring& dir);

// 遞迴刪資料夾（含唯讀檔）。不存在也算成功。
bool removeDirectoryRecursive(const std::wstring& dir);
// 逐層建資料夾。已存在算成功。
bool createDirectories(const std::wstring& dir);

// 本執行個體的暫存資料夾：root\<pid>-<隨機>\，裡面一個獨佔開啟的 lock 檔。
// 解構（或 release）時關鎖、刪整個資料夾。lock 用 FILE_FLAG_DELETE_ON_CLOSE，程式當掉時系統會把它刪掉，
// 下次任何一個 mpainter 啟動時 sweepStaleTempDirs 看到沒有 lock 就會清掉剩下的檔案。
class InstanceTempDir {
public:
    InstanceTempDir() = default;
    ~InstanceTempDir() { release(); }
    InstanceTempDir(const InstanceTempDir&) = delete;
    InstanceTempDir& operator=(const InstanceTempDir&) = delete;

    // 建資料夾並鎖住。失敗回傳 false（root 不可寫）。
    bool create(const std::wstring& root);
    void release();
    bool valid() const { return lock_ != INVALID_HANDLE_VALUE; }
    const std::wstring& path() const { return path_; }

private:
    std::wstring path_;
    HANDLE lock_ = INVALID_HANDLE_VALUE;
};

// 掃 root 底下每個子資料夾：沒有 lock 檔、或 lock 能獨佔開啟（主人已結束）就整個刪掉。
// 回傳刪掉的資料夾數。root 不存在回傳 0。
int sweepStaleTempDirs(const std::wstring& root);

constexpr wchar_t kTempLockName[] = L"lock";

} // namespace mp
