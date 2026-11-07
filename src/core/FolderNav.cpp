#include "core/FolderNav.h"

#include <windows.h>
#include <shlwapi.h>

#include <algorithm>
#include <cwctype>

namespace mp {

static const wchar_t* const kExts[] = {L".bmp", L".dib", L".tif", L".tiff", L".jpg", L".jpeg", L".jpe", L".jfif",
                                       L".png", L".webp"};
static const wchar_t* const kArchiveExts[] = {L".zip", L".7z", L".rar", L".cbz", L".cb7", L".cbr"};
static const wchar_t* const kComicExts[] = {L".cbz", L".cb7", L".cbr"};

static std::wstring lowerCopy(const std::wstring& s) {
    std::wstring lower = s;
    for (auto& ch : lower) ch = static_cast<wchar_t>(std::towlower(ch));
    return lower;
}

bool isSupportedExtension(const std::wstring& ext) {
    std::wstring lower = lowerCopy(ext);
    for (auto* e : kExts)
        if (lower == e) return true;
    return false;
}

const std::vector<std::wstring>& supportedExtensions() {
    static const std::vector<std::wstring> v(std::begin(kExts), std::end(kExts));
    return v;
}

bool isArchiveExtension(const std::wstring& ext) {
    std::wstring lower = lowerCopy(ext);
    for (auto* e : kArchiveExts)
        if (lower == e) return true;
    return false;
}

const std::vector<std::wstring>& archiveExtensions() {
    static const std::vector<std::wstring> v(std::begin(kArchiveExts), std::end(kArchiveExts));
    return v;
}

const std::vector<std::wstring>& comicExtensions() {
    static const std::vector<std::wstring> v(std::begin(kComicExts), std::end(kComicExts));
    return v;
}

void sortLogical(std::vector<std::wstring>& paths) {
    std::sort(paths.begin(), paths.end(),
              [](const std::wstring& a, const std::wstring& b) { return StrCmpLogicalW(a.c_str(), b.c_str()) < 0; });
}

void movePinnedFirst(std::vector<std::wstring>& paths, const std::vector<std::wstring>& pinnedNames) {
    if (pinnedNames.empty() || paths.empty()) return;
    auto rank = [&](const std::wstring& p) -> size_t {
        std::wstring name = p.substr(p.find_last_of(L"\\/") + 1);
        for (size_t i = 0; i < pinnedNames.size(); ++i)
            if (_wcsicmp(name.c_str(), pinnedNames[i].c_str()) == 0) return i;
        return pinnedNames.size();
    };
    std::stable_sort(paths.begin(), paths.end(),
                     [&](const std::wstring& a, const std::wstring& b) { return rank(a) < rank(b); });
}

void movePinnedFirst(std::vector<std::wstring>& paths, std::vector<std::wstring>& names,
                     const std::vector<std::wstring>& pinnedNames) {
    if (pinnedNames.empty() || paths.empty() || paths.size() != names.size()) return;
    auto rank = [&](const std::wstring& p) -> size_t {
        std::wstring name = p.substr(p.find_last_of(L"\\/") + 1);
        for (size_t i = 0; i < pinnedNames.size(); ++i)
            if (_wcsicmp(name.c_str(), pinnedNames[i].c_str()) == 0) return i;
        return pinnedNames.size();
    };
    std::vector<size_t> order(paths.size());
    for (size_t i = 0; i < order.size(); ++i) order[i] = i;
    std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) { return rank(names[a]) < rank(names[b]); });
    std::vector<std::wstring> p2, n2;
    p2.reserve(paths.size());
    n2.reserve(names.size());
    for (size_t i : order) { p2.push_back(std::move(paths[i])); n2.push_back(std::move(names[i])); }
    paths = std::move(p2);
    names = std::move(n2);
}

std::wstring sanitizeFileName(const std::wstring& name) {
    std::wstring out = name;
    for (auto& ch : out)
        if (ch < 32 || wcschr(L"\\/:*?\"<>|", ch)) ch = L'_';
    return out;
}

bool isDirectoryPath(const std::wstring& path) {
    DWORD attr = GetFileAttributesW(path.c_str());
    return attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY);
}

std::wstring extensionOf(const std::wstring& path) {
    size_t slash = path.find_last_of(L"\\/");
    size_t dot = path.find_last_of(L'.');
    if (dot == std::wstring::npos || (slash != std::wstring::npos && dot < slash)) return L"";
    std::wstring ext = path.substr(dot);
    for (auto& ch : ext) ch = static_cast<wchar_t>(std::towlower(ch));
    return ext;
}

std::vector<std::wstring> listImageFiles(const std::wstring& dir) {
    std::vector<std::wstring> out;
    std::wstring pattern = dir;
    if (!pattern.empty() && pattern.back() != L'\\' && pattern.back() != L'/') pattern += L'\\';
    std::wstring base = pattern;
    pattern += L'*';
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileExW(pattern.c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr,
                                FIND_FIRST_EX_LARGE_FETCH);
    if (h == INVALID_HANDLE_VALUE) return out;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        if (isSupportedExtension(extensionOf(fd.cFileName))) out.push_back(base + fd.cFileName);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    sortLogical(out);
    return out;
}

void FolderNav::load(const std::wstring& filePath) {
    size_t slash = filePath.find_last_of(L"\\/");
    std::wstring dir = slash == std::wstring::npos ? L"." : filePath.substr(0, slash);
    auto files = listImageFiles(dir);
    movePinnedFirst(files, pinned_);
    setFiles(std::move(files), filePath);
}

void FolderNav::setFiles(std::vector<std::wstring> files, const std::wstring& current) {
    files_ = std::move(files);
    index_ = -1;
    for (size_t i = 0; i < files_.size(); ++i) {
        if (_wcsicmp(files_[i].c_str(), current.c_str()) == 0) {
            index_ = static_cast<int>(i);
            break;
        }
    }
}

std::wstring FolderNav::prev() {
    if (!hasPrev()) return current();
    --index_;
    return files_[index_];
}

std::wstring FolderNav::next() {
    if (!hasNext()) return current();
    ++index_;
    return files_[index_];
}

std::wstring FolderNav::current() const {
    if (index_ < 0 || index_ >= static_cast<int>(files_.size())) return L"";
    return files_[index_];
}

} // namespace mp
