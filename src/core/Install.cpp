#include "core/Install.h"

#include <windows.h>
#include <tlhelp32.h>

#include <cwchar>
#include <cwctype>
#include <set>

#include "core/AppInfo.h"
#include "core/PathUtil.h"
#include "core/TempDir.h"
#include "core/TextUtil.h"

namespace mp {

namespace {

// 換下來的舊檔改名成 X.mpainter.tempold（上次留下的還被占著就 X.mpainter.tempold2…，最多到 9）。
// 刻意不用 .old / .bak 這類大家自己備份也會用的名字：啟動時只刪這個名字，才不會刪到使用者的檔。
constexpr wchar_t kOldSuffix[] = L".mpainter.tempold";
constexpr int kMaxOldSuffix = 9;

bool fileExists(const std::wstring& p) {
    DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

// X.mpainter.tempold 或 X.mpainter.tempoldN（N 是數字），不分大小寫。
bool isOldFileName(const std::wstring& name) {
    const std::wstring lower = lowerCopy(name);
    const size_t pos = lower.rfind(kOldSuffix);
    if (pos == std::wstring::npos || pos == 0) return false;
    for (size_t i = pos + wcslen(kOldSuffix); i < lower.size(); ++i)
        if (!iswdigit(lower[i])) return false;
    return true;
}

std::wstring parentOf(const std::wstring& path) {
    size_t slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? L"" : path.substr(0, slash);
}

void listFiles(const std::wstring& root, const std::wstring& rel, std::vector<std::wstring>& out) {
    forEachDirEntry(joinPath(root, rel), [&](const DirEntry& e) {
        std::wstring r = rel.empty() ? e.name : rel + L"\\" + e.name;
        if (e.directory) listFiles(root, r, out);
        else out.push_back(r);
        return true;
    });
}

void rollback(std::vector<InstallStep>& steps) {
    for (auto it = steps.rbegin(); it != steps.rend(); ++it) {
        if (it->placed) DeleteFileW(it->dst.c_str());
        if (!it->old.empty()) MoveFileExW(it->old.c_str(), it->dst.c_str(), MOVEFILE_REPLACE_EXISTING);
        it->placed = false;
        it->old.clear();
    }
}

} // namespace

bool isDirWritable(const std::wstring& dir) {
    wchar_t tmp[MAX_PATH];
    if (!GetTempFileNameW(dir.c_str(), L"mpu", 0, tmp)) return false;
    std::wstring renamed = std::wstring(tmp) + L".r";
    bool ok = MoveFileExW(tmp, renamed.c_str(), 0) != 0;
    DeleteFileW(ok ? renamed.c_str() : tmp);
    return ok;
}

std::vector<unsigned long> otherProcessesOfImage(const std::wstring& exePath, unsigned long selfPid) {
    std::vector<unsigned long> out;
    std::wstring exeName = exePath.substr(exePath.find_last_of(L"\\/") + 1);
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return out;
    PROCESSENTRY32W pe{sizeof(pe)};
    for (BOOL more = Process32FirstW(snap, &pe); more; more = Process32NextW(snap, &pe)) {
        if (pe.th32ProcessID == selfPid || _wcsicmp(pe.szExeFile, exeName.c_str()) != 0) continue;
        HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pe.th32ProcessID);
        if (!h) continue;
        wchar_t path[MAX_PATH * 2];
        DWORD len = MAX_PATH * 2;
        if (QueryFullProcessImageNameW(h, 0, path, &len) && _wcsicmp(path, exePath.c_str()) == 0)
            out.push_back(pe.th32ProcessID);
        CloseHandle(h);
    }
    CloseHandle(snap);
    return out;
}

std::vector<InstallStep> planInstall(const std::wstring& stagingDir, const std::wstring& appDir, const std::wstring& exeName) {
    std::vector<std::wstring> rels;
    listFiles(stagingDir, L"", rels);
    std::vector<InstallStep> steps;
    for (const auto& r : rels) {
        InstallStep s;
        s.src = joinPath(stagingDir, r);
        s.dst = joinPath(appDir, _wcsicmp(r.c_str(), kPackageExeName) == 0 ? exeName : r);
        steps.push_back(s);
    }
    return steps;
}

UpdateError installTargetsWritable(const std::vector<InstallStep>& steps) {
    std::set<std::wstring> checked;
    for (const auto& s : steps) {
        // 還不存在的資料夾會由 applyInstall 建：能不能建，看它最近的、存在的上層能不能寫。
        std::wstring dir = parentOf(s.dst);
        while (!dir.empty() && !isDirectoryPath(dir)) dir = parentOf(dir);
        if (dir.empty() || !checked.insert(lowerCopy(dir)).second) continue;
        if (!isDirWritable(dir)) {
            UpdateError e;
            e.code = UpdateErrorCode::FolderNotWritable;
            e.detail = dir;
            return e;
        }
    }
    return {};
}

UpdateError applyInstall(std::vector<InstallStep>& steps) {
    auto fail = [&](UpdateErrorCode code, const std::wstring& path, unsigned long systemError) {
        rollback(steps);
        UpdateError e;
        e.code = code;
        e.detail = path;
        e.systemError = systemError;
        return e;
    };
    for (auto& s : steps) {
        createDirectories(parentOf(s.dst));
        DWORD attr = GetFileAttributesW(s.dst.c_str());
        if (attr != INVALID_FILE_ATTRIBUTES) {
            if (attr & FILE_ATTRIBUTE_DIRECTORY) return fail(UpdateErrorCode::FolderInTheWay, s.dst, 0);
            // 上次更新留下、還刪不掉的舊檔就換下一個名字。
            std::wstring old;
            for (int n = 1; n <= kMaxOldSuffix && old.empty(); ++n) {
                std::wstring cand = s.dst + kOldSuffix + (n == 1 ? L"" : std::to_wstring(n));
                if (fileExists(cand)) DeleteFileW(cand.c_str());
                if (!fileExists(cand)) old = cand;
            }
            if (old.empty() || !MoveFileExW(s.dst.c_str(), old.c_str(), 0)) return fail(UpdateErrorCode::RenameFailed, s.dst, GetLastError());
            s.old = old;
        }
        if (!CopyFileW(s.src.c_str(), s.dst.c_str(), FALSE)) return fail(UpdateErrorCode::CopyFailed, s.dst, GetLastError());
        s.placed = true;
    }
    return {};
}

int removeOldFiles(const std::wstring& dir) {
    int failed = 0;
    std::vector<std::wstring> dirs{dir};
    for (const wchar_t* sub : kPackageSubdirs) dirs.push_back(joinPath(dir, sub));
    for (const std::wstring& d : dirs) {
        forEachDirEntry(d, [&](const DirEntry& e) {
            if (e.directory || !isOldFileName(e.name)) return true;
            if (!DeleteFileW(joinPath(d, e.name).c_str())) ++failed;
            return true;
        }, L"*.mpainter.tempold*");
    }
    return failed;
}

} // namespace mp
