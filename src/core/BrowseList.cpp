#include "core/BrowseList.h"

#include "core/FolderNav.h"
#include "core/PathUtil.h"

namespace mp {

namespace {

// 結尾的斜線拿掉，但磁碟根目錄留一個（"D:" 是「D 槽的目前目錄」，不是根目錄）。
std::wstring trimSlash(std::wstring dir) {
    while (dir.size() > 1 && (dir.back() == L'\\' || dir.back() == L'/')) dir.pop_back();
    if (dir.size() == 2 && dir[1] == L':') dir += L'\\';
    return dir;
}

bool isDriveRoot(const std::wstring& dir) { return dir.size() == 3 && dir[1] == L':' && (dir[2] == L'\\' || dir[2] == L'/'); }

// "\\server\share" 本身（沒有更下層）。
bool isShareRoot(const std::wstring& dir) {
    if (dir.size() < 2 || dir[0] != L'\\' || dir[1] != L'\\') return false;
    const size_t server = dir.find_first_of(L"\\/", 2);
    if (server == std::wstring::npos) return true; // "\\server"：也不能再往上
    return dir.find_first_of(L"\\/", server + 1) == std::wstring::npos;
}

} // namespace

std::vector<BrowseEntry> listBrowseEntries(const std::wstring& dir, const std::vector<std::wstring>& pinned) {
    std::vector<std::wstring> folders, archives;
    forEachDirEntry(dir, [&](const DirEntry& e) {
        if (e.hidden) return true;
        if (e.directory) folders.push_back(joinPath(dir, e.name));
        else if (isArchiveExtension(extensionOf(e.name))) archives.push_back(joinPath(dir, e.name));
        return true;
    });
    sortLogical(folders);
    sortLogical(archives);
    std::vector<std::wstring> images = listImageFiles(dir);
    movePinnedFirst(images, pinned);
    std::vector<BrowseEntry> out;
    out.reserve(folders.size() + archives.size() + images.size());
    auto add = [&](const std::vector<std::wstring>& paths, BrowseKind kind) {
        for (const auto& p : paths) out.push_back({p, fileNameOf(p), kind});
    };
    add(folders, BrowseKind::Folder);
    add(archives, BrowseKind::Archive);
    add(images, BrowseKind::Image);
    return out;
}

int indexOfBrowsePath(const std::vector<BrowseEntry>& entries, const std::wstring& path) {
    for (size_t i = 0; i < entries.size(); ++i)
        if (_wcsicmp(entries[i].path.c_str(), path.c_str()) == 0) return static_cast<int>(i);
    return -1;
}

std::wstring browseFolderOf(const std::wstring& path) {
    if (path.empty()) return L"";
    if (isDirectoryPath(path)) return trimSlash(path);
    const size_t slash = path.find_last_of(L"\\/");
    if (slash == std::wstring::npos) return L"";
    return trimSlash(path.substr(0, slash + 1));
}

std::wstring parentFolder(const std::wstring& dir) {
    const std::wstring d = trimSlash(dir);
    if (d.empty() || isDriveRoot(d) || isShareRoot(d)) return L"";
    const size_t slash = d.find_last_of(L"\\/");
    if (slash == std::wstring::npos) return L"";
    return trimSlash(d.substr(0, slash + 1));
}

std::wstring nearestExistingFolder(const std::wstring& dir) {
    for (std::wstring d = trimSlash(dir); !d.empty(); d = parentFolder(d))
        if (isDirectoryPath(d)) return d;
    return L"";
}

std::wstring folderDisplayName(const std::wstring& dir) {
    const std::wstring d = trimSlash(dir);
    if (isDriveRoot(d) || isShareRoot(d)) return d;
    const size_t slash = d.find_last_of(L"\\/");
    return slash == std::wstring::npos ? d : d.substr(slash + 1);
}

std::wstring composeBrowseTitle(const std::wstring& name, const std::wstring& label, const std::wstring& appName) {
    return (name.empty() ? std::wstring() : name + L" - ") + label + L" - " + appName;
}

} // namespace mp
