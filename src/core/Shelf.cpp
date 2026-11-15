#include "core/Shelf.h"

#include "core/Archive.h"
#include "core/FolderNav.h"

namespace mp {

namespace {

std::wstring stripSlash(const std::wstring& path) {
    std::wstring p = path;
    while (p.size() > 3 && (p.back() == L'\\' || p.back() == L'/')) p.pop_back();
    return p;
}

} // namespace

PathKind classifyPath(const std::wstring& path) {
    if (isDirectoryPath(path)) return PathKind::Folder;
    const std::wstring ext = extensionOf(path);
    if (isArchiveExtension(ext) || (!isSupportedExtension(ext) && looksLikeArchive(path))) return PathKind::Archive;
    return PathKind::Image;
}

std::wstring firstInFolder(const std::wstring& dir) {
    auto files = listImageFiles(dir);
    if (!files.empty()) return files.front();
    auto books = listBooks(dir);
    return books.empty() ? L"" : books.front();
}

bool hasImageFiles(const std::wstring& dir) {
    bool found = false;
    forEachDirEntry(dir, [&](const DirEntry& e) {
        found = !e.directory && isSupportedExtension(extensionOf(e.name));
        return !found;
    });
    return found;
}

std::vector<std::wstring> listBooks(const std::wstring& parentDir) {
    std::vector<std::wstring> out;
    forEachDirEntry(parentDir, [&](const DirEntry& e) {
        std::wstring p = joinPath(parentDir, e.name);
        if (e.directory) {
            if (hasImageFiles(p)) out.push_back(p);
            return true;
        }
        std::wstring ext = extensionOf(e.name);
        if (isArchiveExtension(ext) || (!isSupportedExtension(ext) && looksLikeArchive(p))) out.push_back(p);
        return true;
    });
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
