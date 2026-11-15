#include "core/PathUtil.h"

#include <windows.h>

#include <cwctype>

namespace mp {

std::wstring joinPath(const std::wstring& a, const std::wstring& b) {
    if (a.empty()) return b;
    if (a.back() == L'\\' || a.back() == L'/') return a + b;
    return a + L"\\" + b;
}

std::wstring extensionOf(const std::wstring& path) {
    size_t slash = path.find_last_of(L"\\/");
    size_t dot = path.find_last_of(L'.');
    if (dot == std::wstring::npos || (slash != std::wstring::npos && dot < slash)) return L"";
    std::wstring ext = path.substr(dot);
    for (auto& ch : ext) ch = static_cast<wchar_t>(std::towlower(ch));
    return ext;
}

std::wstring fileNameOf(const std::wstring& path) {
    const size_t s = path.find_last_of(L"\\/");
    return s == std::wstring::npos ? path : path.substr(s + 1);
}

bool isDirectoryPath(const std::wstring& path) {
    DWORD attr = GetFileAttributesW(path.c_str());
    return attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY);
}

void forEachDirEntry(const std::wstring& dir, const std::function<bool(const DirEntry&)>& fn, const wchar_t* pattern) {
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileExW(joinPath(dir, pattern).c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr,
                                FIND_FIRST_EX_LARGE_FETCH);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) continue;
        DirEntry e;
        e.name = fd.cFileName;
        e.directory = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        e.reparsePoint = (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
        e.readOnly = (fd.dwFileAttributes & FILE_ATTRIBUTE_READONLY) != 0;
        e.size = e.directory ? 0 : (static_cast<uint64_t>(fd.nFileSizeHigh) << 32) | fd.nFileSizeLow;
        if (!fn(e)) break;
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

} // namespace mp
