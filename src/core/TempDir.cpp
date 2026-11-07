#include "core/TempDir.h"

#include <random>
#include <vector>

namespace mp {

namespace {

std::wstring joinPath(const std::wstring& a, const std::wstring& b) {
    if (a.empty()) return b;
    if (a.back() == L'\\' || a.back() == L'/') return a + b;
    return a + L"\\" + b;
}

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
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileExW(joinPath(dir, L"*").c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr,
                                FIND_FIRST_EX_LARGE_FETCH);
    if (h == INVALID_HANDLE_VALUE) return 0;
    uint64_t total = 0;
    do {
        if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) total += directorySize(joinPath(dir, fd.cFileName));
        else total += (static_cast<uint64_t>(fd.nFileSizeHigh) << 32) | fd.nFileSizeLow;
    } while (FindNextFileW(h, &fd));
    FindClose(h);
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
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileExW(joinPath(dir, L"*").c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr, 0);
    bool ok = true;
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) continue;
            std::wstring p = joinPath(dir, fd.cFileName);
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_READONLY) SetFileAttributesW(p.c_str(), FILE_ATTRIBUTE_NORMAL);
            if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && !(fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT))
                ok = removeDirectoryRecursive(p) && ok;
            else if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
                ok = RemoveDirectoryW(p.c_str()) && ok; // 連結點只拆連結，不進去刪
            else
                ok = DeleteFileW(p.c_str()) && ok;
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
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
    if (lock_ == INVALID_HANDLE_VALUE) return;
    CloseHandle(lock_); // DELETE_ON_CLOSE：lock 檔隨之消失
    lock_ = INVALID_HANDLE_VALUE;
    removeDirectoryRecursive(path_);
    path_.clear();
}

int sweepStaleTempDirs(const std::wstring& root) {
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileExW(joinPath(root, L"*").c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr, 0);
    if (h == INVALID_HANDLE_VALUE) return 0;
    std::vector<std::wstring> stale;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
        if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) continue;
        std::wstring dir = joinPath(root, fd.cFileName);
        HANDLE lock = openLock(dir, OPEN_EXISTING, 0);
        if (lock != INVALID_HANDLE_VALUE) {
            CloseHandle(lock); // 開得起來 = 沒人持有
            stale.push_back(dir);
        } else if (GetLastError() != ERROR_SHARING_VIOLATION) {
            stale.push_back(dir); // 沒有 lock 檔（主人當掉後系統刪了它）
        }
        // ERROR_SHARING_VIOLATION：另一個 mpainter 還在用，跳過
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    int removed = 0;
    for (const auto& d : stale)
        if (removeDirectoryRecursive(d)) ++removed;
    return removed;
}

} // namespace mp
