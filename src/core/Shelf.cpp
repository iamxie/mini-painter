#include "core/Shelf.h"

#include <windows.h>

#include "core/Archive.h"
#include "core/FolderNav.h"

namespace mp {

namespace {

std::wstring withSlash(const std::wstring& dir) {
    if (dir.empty() || dir.back() == L'\\' || dir.back() == L'/') return dir;
    return dir + L"\\";
}

std::wstring stripSlash(const std::wstring& path) {
    std::wstring p = path;
    while (p.size() > 3 && (p.back() == L'\\' || p.back() == L'/')) p.pop_back();
    return p;
}

} // namespace

bool hasImageFiles(const std::wstring& dir) {
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileExW((withSlash(dir) + L"*").c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr,
                                FIND_FIRST_EX_LARGE_FETCH);
    if (h == INVALID_HANDLE_VALUE) return false;
    bool found = false;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        if (isSupportedExtension(extensionOf(fd.cFileName))) { found = true; break; }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return found;
}

std::vector<std::wstring> listBooks(const std::wstring& parentDir) {
    std::vector<std::wstring> out;
    std::wstring base = withSlash(parentDir);
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileExW((base + L"*").c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr,
                                FIND_FIRST_EX_LARGE_FETCH);
    if (h == INVALID_HANDLE_VALUE) return out;
    do {
        if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) continue;
        std::wstring p = base + fd.cFileName;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (hasImageFiles(p)) out.push_back(p);
            continue;
        }
        std::wstring ext = extensionOf(fd.cFileName);
        if (isArchiveExtension(ext) || (!isSupportedExtension(ext) && looksLikeArchive(p))) out.push_back(p);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    sortLogical(out);
    return out;
}

std::wstring neighborBook(const std::wstring& bookPath, int dir) {
    std::wstring book = stripSlash(bookPath);
    size_t slash = book.find_last_of(L"\\/");
    if (slash == std::wstring::npos || slash == 0) return L"";
    std::wstring parent = book.substr(0, slash);
    if (parent.size() == 2 && parent[1] == L':') parent += L"\\"; // "D:" -> "D:\"
    auto books = listBooks(parent);
    for (size_t i = 0; i < books.size(); ++i) {
        if (_wcsicmp(stripSlash(books[i]).c_str(), book.c_str()) != 0) continue;
        if (dir > 0) return i + 1 < books.size() ? books[i + 1] : L"";
        return i > 0 ? books[i - 1] : L"";
    }
    return L"";
}

} // namespace mp
