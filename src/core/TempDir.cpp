#include "core/TempDir.h"

#include <windows.h>

#include <random>
#include <vector>

#include "core/PathUtil.h"

namespace mp {

namespace {

// 開 lock 檔：share 一律 0，所以同一時間只有一個 handle 開得起來。
HANDLE openLock(const std::wstring& dir, DWORD disposition, DWORD flags) {
    return CreateFileW(joinPath(dir, kTempLockName).c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, disposition,
                       FILE_ATTRIBUTE_NORMAL | flags, nullptr);
}

} // namespace

std::wstring defaultUnzipRoot(const std::wstring& exeDir) {
    return joinPath(exeDir.empty() ? L"." : exeDir, L"temp");
}

std::wstring fallbackUnzipRoot() {
    wchar_t buf[MAX_PATH];
    DWORD n = GetTempPathW(MAX_PATH, buf);
    std::wstring t = n > 0 && n < MAX_PATH ? buf : L".";
    return joinPath(t, L"mpainter\\unzip");
}

std::wstring resolveUnzipRoot(const std::wstring& setting, const std::wstring& exeDir) {
    size_t a = setting.find_first_not_of(L" \t");
    if (a == std::wstring::npos) return defaultUnzipRoot(exeDir);
    size_t b = setting.find_last_not_of(L" \t");
    return setting.substr(a, b - a + 1);
}

uint64_t directorySize(const std::wstring& dir) {
    uint64_t total = 0;
    forEachDirEntry(dir, [&](const DirEntry& e) {
        if (e.reparsePoint) return true;
        total += e.directory ? directorySize(joinPath(dir, e.name)) : e.size;
        return true;
    });
    return total;
}

bool createDirectories(const std::wstring& dir) {
    if (dir.empty()) return false;
    DWORD attr = GetFileAttributesW(dir.c_str());
    if (attr != INVALID_FILE_ATTRIBUTES) return (attr & FILE_ATTRIBUTE_DIRECTORY) != 0;
    size_t slash = dir.find_last_of(L"\\/");
    if (slash != std::wstring::npos && slash > 0 && dir[slash - 1] != L':') {
        if (!createDirectories(dir.substr(0, slash))) return false;
    }
    if (CreateDirectoryW(dir.c_str(), nullptr)) return true;
    return GetLastError() == ERROR_ALREADY_EXISTS;
}

bool removeDirectoryRecursive(const std::wstring& dir) {
    DWORD attr = GetFileAttributesW(dir.c_str());
    if (attr == INVALID_FILE_ATTRIBUTES) return true;
    if (!(attr & FILE_ATTRIBUTE_DIRECTORY)) return false;
    bool ok = true;
    forEachDirEntry(dir, [&](const DirEntry& e) {
        std::wstring p = joinPath(dir, e.name);
        if (e.readOnly) SetFileAttributesW(p.c_str(), FILE_ATTRIBUTE_NORMAL);
        if (e.directory && !e.reparsePoint) ok = removeDirectoryRecursive(p) && ok;
        else if (e.directory) ok = RemoveDirectoryW(p.c_str()) && ok; // 連結點只拆連結，不進去刪
        else ok = DeleteFileW(p.c_str()) && ok;
        return true;
    });
    SetFileAttributesW(dir.c_str(), FILE_ATTRIBUTE_NORMAL);
    return RemoveDirectoryW(dir.c_str()) && ok;
}

bool InstanceTempDir::create(const std::wstring& root) {
    release();
    if (!createDirectories(root)) return false;
    std::mt19937 rng(static_cast<unsigned>(GetTickCount64() ^ GetCurrentProcessId()));
    for (int attempt = 0; attempt < 8; ++attempt) {
        wchar_t name[64];
        swprintf(name, 64, L"%lu-%08x", GetCurrentProcessId(), rng());
        std::wstring dir = joinPath(root, name);
        if (!CreateDirectoryW(dir.c_str(), nullptr)) continue;
        HANDLE h = openLock(dir, CREATE_NEW, FILE_FLAG_DELETE_ON_CLOSE);
        if (h == INVALID_HANDLE_VALUE) {
            RemoveDirectoryW(dir.c_str());
            continue;
        }
        path_ = dir;
        lock_ = h;
        return true;
    }
    return false;
}

void InstanceTempDir::release() {
    if (!lock_) return;
    CloseHandle(lock_); // DELETE_ON_CLOSE：lock 檔隨之消失
    lock_ = nullptr;
    removeDirectoryRecursive(path_);
    path_.clear();
}

bool isInstanceTempDirName(const std::wstring& name) {
    size_t dash = name.find(L'-');
    if (dash == std::wstring::npos || dash == 0 || name.size() != dash + 1 + 8) return false;
    for (size_t i = 0; i < dash; ++i)
        if (name[i] < L'0' || name[i] > L'9') return false;
    for (size_t i = dash + 1; i < name.size(); ++i) {
        wchar_t c = name[i];
        if (!((c >= L'0' && c <= L'9') || (c >= L'a' && c <= L'f') || (c >= L'A' && c <= L'F'))) return false;
    }
    return true;
}

int sweepStaleTempDirs(const std::wstring& root) {
    std::vector<std::wstring> stale;
    forEachDirEntry(root, [&](const DirEntry& e) {
        // root 是使用者可以改的設定，可能指到本來就有東西的資料夾：只認自己建的名稱，其他的不碰。
        if (!e.directory || !isInstanceTempDirName(e.name)) return true;
        std::wstring dir = joinPath(root, e.name);
        HANDLE lock = openLock(dir, OPEN_EXISTING, 0);
        if (lock != INVALID_HANDLE_VALUE) {
            CloseHandle(lock); // 開得起來 = 沒人持有
            stale.push_back(dir);
        } else if (GetLastError() == ERROR_FILE_NOT_FOUND) {
            stale.push_back(dir); // 沒有 lock 檔（主人當掉後系統刪了它）
        }
        // ERROR_SHARING_VIOLATION：另一個 mpainter 還在用，跳過。其他錯誤（沒權限…）看不出狀況，也跳過。
        return true;
    });
    int removed = 0;
    for (const auto& d : stale)
        if (removeDirectoryRecursive(d)) ++removed;
    return removed;
}

} // namespace mp
