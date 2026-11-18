#include "core/PluginRun.h"

#include <windows.h>

#include <atomic>
#include <cstring>
#include <cwchar>
#include <memory>
#include <random>

#include "core/PathUtil.h"
#include "core/TempDir.h"
#include "core/TextUtil.h"

namespace mp {

namespace {

// 紀錄檔的內容（固定大小，整個檔就是這個結構）。行程死掉時映射的頁面照樣寫回檔案，所以下次啟動讀得到。
constexpr char kMagic[8] = {'M', 'P', 'R', 'U', 'N', '0', '0', '1'};
constexpr int kSlots = 16;
constexpr size_t kPathChars = 1024;

enum SlotState : LONG { SlotFree = 0, SlotWriting = 1, SlotInside = 2 };

struct RunSlot {
    volatile LONG state;
    DWORD threadId;
    wchar_t pluginId[64];
    char func[16];
    wchar_t path[kPathChars];
};

struct RunFile {
    char magic[8];
    volatile LONG clean;
    volatile LONG crashed;
    DWORD crashThreadId;
    DWORD exceptionCode;
    wchar_t crashModule[kPathChars];
    wchar_t exePath[kPathChars];
    RunSlot slots[kSlots];
};

// 這個行程的紀錄檔。開著的期間 view 一直有效（正常執行時永遠不關，避免背景執行緒還在 plugin 裡時被拆掉）。
std::atomic<RunFile*> g_view{nullptr};
HANDLE g_file = INVALID_HANDLE_VALUE;
HANDLE g_mapping = nullptr;
std::wstring g_path;
LPTOP_LEVEL_EXCEPTION_FILTER g_prevFilter = nullptr;

template <size_t N>
void copyText(wchar_t (&dst)[N], const std::wstring& src) {
    wcsncpy_s(dst, N, src.c_str(), _TRUNCATE);
}

template <size_t N>
std::wstring readText(const wchar_t (&src)[N]) {
    return std::wstring(src, wcsnlen(src, N));
}

LONG WINAPI crashFilter(EXCEPTION_POINTERS* ep) {
    // 當掉當下：只寫記憶體，不碰檔案 API。
    if (RunFile* f = g_view.load()) {
        if (InterlockedCompareExchange(&f->crashed, 1, 0) == 0) {
            f->crashThreadId = GetCurrentThreadId();
            f->exceptionCode = ep && ep->ExceptionRecord ? ep->ExceptionRecord->ExceptionCode : 0;
            HMODULE m = nullptr;
            if (ep && ep->ExceptionRecord &&
                GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                   static_cast<LPCWSTR>(ep->ExceptionRecord->ExceptionAddress), &m))
                GetModuleFileNameW(m, f->crashModule, static_cast<DWORD>(kPathChars));
        }
    }
    return g_prevFilter ? g_prevFilter(ep) : EXCEPTION_CONTINUE_SEARCH;
}

bool isRunFileName(const std::wstring& name) {
    const size_t n = name.size();
    return n > 4 && _wcsicmp(name.c_str() + n - 4, L".run") == 0 && isInstanceTempDirName(name.substr(0, n - 4));
}

// 「path 在 dir 底下」：dir 之後緊接著分隔符號。回傳 dir 之後的第一段（plugin id）；不在底下回空字串。
std::wstring childOf(const std::wstring& path, const std::wstring& dir) {
    if (dir.empty() || path.size() <= dir.size() + 1) return L"";
    if (_wcsnicmp(path.c_str(), dir.c_str(), dir.size()) != 0) return L"";
    const wchar_t sep = path[dir.size()];
    if (sep != L'\\' && sep != L'/') return L"";
    const std::wstring rest = path.substr(dir.size() + 1);
    const size_t end = rest.find_first_of(L"\\/");
    return end == std::wstring::npos ? L"" : rest.substr(0, end); // 要是資料夾裡的檔，不是 dir 底下直接的檔
}

PluginRunRecord recordFromFile(const RunFile& f) {
    PluginRunRecord r;
    r.cleanExit = f.clean != 0;
    r.crashed = f.crashed != 0;
    r.crashThreadId = f.crashThreadId;
    r.exceptionCode = f.exceptionCode;
    r.crashModule = readText(f.crashModule);
    for (const RunSlot& s : f.slots) {
        if (s.state != SlotInside) continue;
        PluginRunRecord::Busy b;
        b.pluginId = readText(s.pluginId);
        b.func = utf8ToWide(std::string(s.func, strnlen(s.func, sizeof(s.func))));
        b.path = readText(s.path);
        b.threadId = s.threadId;
        if (!b.pluginId.empty()) r.busy.push_back(std::move(b));
    }
    return r;
}

} // namespace

bool startPluginRunLog(const std::wstring& dir, const std::wstring& exePath) {
    if (g_view.load()) return true;
    if (!createDirectories(dir)) return false;
    std::mt19937 rng(static_cast<unsigned>(GetTickCount64() ^ (GetCurrentProcessId() << 8)));
    for (int attempt = 0; attempt < 8; ++attempt) {
        wchar_t name[64];
        swprintf(name, 64, L"%lu-%08x.run", GetCurrentProcessId(), rng());
        const std::wstring path = joinPath(dir, name);
        // 共用模式只給讀：別的行程要「讀完刪掉」得獨佔開啟，這個行程活著的時候一定開不了。
        HANDLE h = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_NEW,
                               FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE) {
            if (GetLastError() == ERROR_FILE_EXISTS) continue;
            return false;
        }
        HANDLE m = CreateFileMappingW(h, nullptr, PAGE_READWRITE, 0, static_cast<DWORD>(sizeof(RunFile)), nullptr);
        auto* view = m ? static_cast<RunFile*>(MapViewOfFile(m, FILE_MAP_WRITE, 0, 0, sizeof(RunFile))) : nullptr;
        if (!view) {
            if (m) CloseHandle(m);
            CloseHandle(h);
            DeleteFileW(path.c_str());
            return false;
        }
        // 新延伸的檔案內容是 0：其他欄位不必清。
        std::memcpy(view->magic, kMagic, sizeof(kMagic));
        copyText(view->exePath, exePath);
        g_file = h;
        g_mapping = m;
        g_path = path;
        g_view.store(view);
        return true;
    }
    return false;
}

void markPluginRunClean() {
    if (RunFile* f = g_view.load()) InterlockedExchange(&f->clean, 1);
}

void stopPluginRunLogForTest(bool clean) {
    RunFile* f = g_view.exchange(nullptr);
    if (!f) return;
    if (clean) InterlockedExchange(&f->clean, 1);
    UnmapViewOfFile(f);
    CloseHandle(g_mapping);
    CloseHandle(g_file);
    g_mapping = nullptr;
    g_file = INVALID_HANDLE_VALUE;
    g_path.clear();
}

std::wstring pluginRunLogPath() { return g_view.load() ? g_path : L""; }

void installPluginCrashFilter() {
    static bool installed = false;
    if (installed) return;
    installed = true;
    g_prevFilter = SetUnhandledExceptionFilter(crashFilter);
}

PluginCallScope::PluginCallScope(const std::wstring& pluginId, const char* func, const std::wstring& path) {
    RunFile* f = g_view.load();
    if (!f) return;
    for (int i = 0; i < kSlots; ++i) {
        RunSlot& s = f->slots[i];
        if (InterlockedCompareExchange(&s.state, SlotWriting, SlotFree) != SlotFree) continue;
        s.threadId = GetCurrentThreadId();
        copyText(s.pluginId, pluginId);
        strncpy_s(s.func, sizeof(s.func), func, _TRUNCATE);
        copyText(s.path, path);
        InterlockedExchange(&s.state, SlotInside);
        slot_ = i;
        return;
    }
}

PluginCallScope::~PluginCallScope() {
    if (slot_ < 0) return;
    if (RunFile* f = g_view.load()) InterlockedExchange(&f->slots[slot_].state, SlotFree);
}

std::vector<PluginCrash> judgePluginRun(const PluginRunRecord& rec, const std::wstring& pluginsDir) {
    std::vector<PluginCrash> out;
    if (rec.cleanExit) return out;
    auto add = [&](const std::wstring& id, const std::wstring& func, const std::wstring& path, unsigned long code) {
        for (const auto& c : out)
            if (_wcsicmp(c.pluginId.c_str(), id.c_str()) == 0) return;
        out.push_back({id, func, path, code});
    };
    if (rec.crashed) {
        for (const auto& b : rec.busy)
            if (b.threadId == rec.crashThreadId) {
                add(b.pluginId, b.func, b.path, rec.exceptionCode);
                return out;
            }
        const std::wstring id = childOf(rec.crashModule, pluginsDir);
        if (!id.empty()) {
            // 出事的是 plugin 的模組（例如它自己開的執行緒）：同一個 plugin 有執行緒在裡面的話，順便帶出當時的檔。
            std::wstring func, path;
            for (const auto& b : rec.busy)
                if (_wcsicmp(b.pluginId.c_str(), id.c_str()) == 0) { func = b.func; path = b.path; break; }
            add(id, func, path, rec.exceptionCode);
        }
        return out;
    }
    for (const auto& b : rec.busy) add(b.pluginId, b.func, b.path, 0);
    return out;
}

std::vector<PluginCrash> collectPluginCrashes(const std::wstring& dir, const std::wstring& exePath, const std::wstring& pluginsDir) {
    std::vector<std::wstring> names;
    forEachDirEntry(dir, [&](const DirEntry& e) {
        if (!e.directory && isRunFileName(e.name)) names.push_back(e.name);
        return true;
    });
    std::vector<PluginCrash> out;
    for (const auto& name : names) {
        const std::wstring path = joinPath(dir, name);
        if (path == g_path) continue; // 自己的
        // 獨佔開啟：主人還活著（握著檔案）就開不了；兩個行程同時來讀，只有一個開得到。
        HANDLE h = CreateFileW(path.c_str(), GENERIC_READ | DELETE, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE) continue;
        auto file = std::make_unique<RunFile>();
        DWORD got = 0;
        const bool whole = ReadFile(h, file.get(), sizeof(RunFile), &got, nullptr) && got == sizeof(RunFile);
        const bool valid = whole && std::memcmp(file->magic, kMagic, sizeof(kMagic)) == 0;
        bool mine = false;
        if (valid) {
            mine = _wcsicmp(readText(file->exePath).c_str(), exePath.c_str()) == 0;
            if (mine)
                for (auto& c : judgePluginRun(recordFromFile(*file), pluginsDir)) {
                    bool dup = false;
                    for (const auto& o : out) dup = dup || _wcsicmp(o.pluginId.c_str(), c.pluginId.c_str()) == 0;
                    if (!dup) out.push_back(std::move(c));
                }
        }
        // 自己的讀完刪；內容不完整的也刪（名字是我們的格式，只可能是建到一半）；別的 exe 的留給它自己。
        if (mine || !valid) {
            FILE_DISPOSITION_INFO del{TRUE};
            SetFileInformationByHandle(h, FileDispositionInfo, &del, sizeof(del));
        }
        CloseHandle(h);
    }
    return out;
}

} // namespace mp
