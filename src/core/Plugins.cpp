#include "core/Plugins.h"

#include <windows.h>

#include <algorithm>
#include <cstring>
#include <mutex>

#include "core/AppInfo.h"
#include "core/FolderNav.h"
#include "core/ImageFormat.h"
#include "core/PathUtil.h"
#include "core/PluginRun.h"
#include "core/Settings.h"
#include "core/TextUtil.h"
#include "plugins/sdk/mpainter_plugin.h"

namespace mp {

static_assert(kPluginApiVersion == MP_PLUGIN_API_VERSION, "Plugins.h and the SDK header must agree on the API version");

namespace {

bool sameId(const std::wstring& a, const std::wstring& b) { return _wcsicmp(a.c_str(), b.c_str()) == 0; }

bool containsId(const std::vector<std::wstring>& ids, const std::wstring& id) {
    for (const auto& x : ids)
        if (sameId(x, id)) return true;
    return false;
}

bool isBuiltInExtension(const std::wstring& ext) {
    return imageFormatOfExtension(ext) != ImageFormat::Unknown || isArchiveExtension(ext);
}

// 生效中的那一組。背景的預先解碼也會讀，用 mutex 保護 shared_ptr 本身。
std::shared_ptr<const PluginSet> g_active;
std::mutex g_activeMutex;

// 整個檔的唯讀映射：plugin 只碰得到需要的頁面（readSize 不會把整個檔讀進來）。
struct MappedFile {
    HANDLE file = INVALID_HANDLE_VALUE;
    HANDLE mapping = nullptr;
    const uint8_t* data = nullptr;
    size_t size = 0;
    DWORD error = 0;

    explicit MappedFile(const std::wstring& path) {
        file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE) { error = GetLastError(); return; }
        LARGE_INTEGER len{};
        if (!GetFileSizeEx(file, &len)) { error = GetLastError(); return; }
        if (len.QuadPart == 0) return; // 空檔不能映射：當成讀不到（error 0）
        mapping = CreateFileMappingW(file, nullptr, PAGE_READONLY, 0, 0, nullptr);
        if (!mapping) { error = GetLastError(); return; }
        data = static_cast<const uint8_t*>(MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, 0));
        if (!data) { error = GetLastError(); return; }
        size = static_cast<size_t>(len.QuadPart);
    }
    ~MappedFile() {
        if (data) UnmapViewOfFile(data);
        if (mapping) CloseHandle(mapping);
        if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
    }
    MappedFile(const MappedFile&) = delete;
    MappedFile& operator=(const MappedFile&) = delete;
};

// 映射期間檔案被別人截短時，讀到那一頁會丟 EXCEPTION_IN_PAGE_ERROR：當成讀檔失敗。
// 其他例外（plugin 的 bug）照常往外丟，要讓程式當掉、下次啟動認出是哪個 plugin。
// 這兩個函式裡不能有要解構的 C++ 物件（__try 的限制）。
int inPageOnly(unsigned long code) { return code == EXCEPTION_IN_PAGE_ERROR ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH; }

bool guardedProbe(const MpPluginFuncs* f, const uint8_t* data, size_t size, int32_t* format) {
    __try {
        *format = f->probe(data, size);
        return true;
    } __except (inPageOnly(GetExceptionCode())) {
        return false;
    }
}

bool guardedReadSize(const MpPluginFuncs* f, int32_t format, const uint8_t* data, size_t size, int32_t* w, int32_t* h, int32_t* rc) {
    __try {
        *rc = f->readSize(format, data, size, w, h);
        return true;
    } __except (inPageOnly(GetExceptionCode())) {
        return false;
    }
}

bool guardedDecode(const MpPluginFuncs* f, int32_t format, const uint8_t* data, size_t size, const MpHost* host, MpDecodeInfo* info,
                   int32_t* rc) {
    __try {
        *rc = f->decode(format, data, size, host, info);
        return true;
    } __except (inPageOnly(GetExceptionCode())) {
        return false;
    }
}

struct AllocContext {
    StraightImage* image = nullptr;
    bool called = false;
    bool outOfMemory = false;
};

uint8_t* allocImage(void* context, int32_t width, int32_t height) {
    auto* ctx = static_cast<AllocContext*>(context);
    if (ctx->called || width <= 0 || height <= 0) return nullptr; // 一次 decode 只配一次
    ctx->called = true;
    try {
        *ctx->image = StraightImage(width, height);
    } catch (...) { // std::bad_alloc；例外不能穿過 C 介面
        ctx->outOfMemory = true;
        return nullptr;
    }
    return ctx->image->pixels.data();
}

IoError pluginFailure(const PluginMatch& m, const char* func, int32_t rc, const char* message) {
    IoError e;
    e.code = IoErrorCode::PluginFailed;
    wchar_t head[96];
    swprintf(head, 96, L"%ls, %hs returned %d", m.plugin->id.c_str(), func, rc);
    e.detail = head;
    if (message && *message) e.detail += L": " + utf8ToWide(message);
    return e;
}

} // namespace

bool pluginApiSupported(unsigned api) { return api >= kPluginApiOldest && api <= kPluginApiVersion; }

bool askPluginsFirst(ImageFormat format) { return format == ImageFormat::Tiff || format == ImageFormat::Unknown; }

std::wstring pluginsDirForExe(const std::wstring& exeDir) { return joinPath(exeDir, L"plugins"); }

bool isBundledPlugin(const std::wstring& id) {
    for (const wchar_t* b : kBundledPlugins)
        if (sameId(b, id)) return true;
    return false;
}

std::vector<PluginEntry> scanPlugins(const std::wstring& pluginsDir) {
    std::vector<PluginEntry> out;
    forEachDirEntry(pluginsDir, [&](const DirEntry& d) {
        if (!d.directory || d.reparsePoint) return true;
        PluginEntry e;
        e.id = d.name;
        e.dllPath = joinPath(joinPath(pluginsDir, d.name), d.name + L".dll");
        if (GetFileAttributesW(e.dllPath.c_str()) == INVALID_FILE_ATTRIBUTES) return true;
        e.manifest = readPluginManifest(e.dllPath);
        if (e.manifest.ok())
            for (const auto& f : e.manifest.manifest.formats)
                for (const auto& ext : f.extensions)
                    if (isBuiltInExtension(ext) && !containsId(e.builtInOverlap, ext)) e.builtInOverlap.push_back(ext);
        out.push_back(std::move(e));
        return true;
    });
    std::sort(out.begin(), out.end(), [](const PluginEntry& a, const PluginEntry& b) { return _wcsicmp(a.id.c_str(), b.id.c_str()) < 0; });
    return out;
}

std::vector<std::wstring> pluginEffectiveExtensions(const PluginEntry& e) {
    std::vector<std::wstring> out;
    if (!e.manifest.ok()) return out;
    for (const auto& f : e.manifest.manifest.formats)
        for (const auto& ext : f.extensions)
            if (!isBuiltInExtension(ext) && !containsId(out, ext)) out.push_back(ext);
    return out;
}

PluginLoadError pluginStaticCheck(const PluginEntry& e) {
    if (!e.manifest.ok()) return PluginLoadError::BadManifest;
    if (!pluginApiSupported(e.manifest.manifest.api)) return PluginLoadError::ApiMismatch;
    return PluginLoadError::None;
}

std::vector<PluginConflict> findPluginConflicts(const std::vector<PluginEntry>& entries, const std::vector<std::wstring>& enabledIds) {
    std::vector<PluginConflict> out;
    for (const auto& e : entries) {
        if (!containsId(enabledIds, e.id) || pluginStaticCheck(e) != PluginLoadError::None) continue;
        for (const auto& ext : pluginEffectiveExtensions(e)) {
            auto it = std::find_if(out.begin(), out.end(), [&](const PluginConflict& c) { return c.extension == ext; });
            if (it == out.end()) out.push_back({ext, {e.id}});
            else it->ids.push_back(e.id);
        }
    }
    out.erase(std::remove_if(out.begin(), out.end(), [](const PluginConflict& c) { return c.ids.size() < 2; }), out.end());
    std::sort(out.begin(), out.end(), [](const PluginConflict& a, const PluginConflict& b) { return a.extension < b.extension; });
    return out;
}

std::vector<PluginConflict> pluginConflictsIfEnabled(const std::vector<PluginEntry>& entries, const std::vector<std::wstring>& enabledIds,
                                                     const std::wstring& id) {
    std::vector<std::wstring> ids = enabledIds;
    if (!containsId(ids, id)) ids.push_back(id);
    std::vector<PluginConflict> out = findPluginConflicts(entries, ids);
    out.erase(std::remove_if(out.begin(), out.end(), [&](const PluginConflict& c) { return !containsId(c.ids, id); }), out.end());
    return out;
}

std::vector<std::wstring> conflictingIds(const std::vector<PluginConflict>& conflicts) {
    std::vector<std::wstring> out;
    for (const auto& c : conflicts)
        for (const auto& id : c.ids)
            if (!containsId(out, id)) out.push_back(id);
    return out;
}

PluginSet::~PluginSet() {
    for (auto& p : plugins_)
        if (p.module) FreeLibrary(static_cast<HMODULE>(p.module));
}

const PluginSet::Plugin* PluginSet::pluginForExtension(const std::wstring& ext) const {
    const std::wstring lower = lowerCopy(ext);
    for (const auto& p : plugins_)
        for (const auto& e : p.extensions)
            if (e == lower) return &p;
    return nullptr;
}

std::shared_ptr<PluginSet> loadPlugins(const std::vector<PluginEntry>& entries, const std::vector<std::wstring>& enabledIds,
                                       std::vector<PluginLoadResult>& results) {
    auto set = std::make_shared<PluginSet>();
    results.clear();
    std::vector<std::wstring> taken;
    for (const auto& e : entries) {
        if (!containsId(enabledIds, e.id)) continue;
        PluginLoadResult r;
        r.id = e.id;
        auto done = [&](PluginLoadError err, HMODULE module) {
            r.error = err;
            if (module) FreeLibrary(module);
            results.push_back(r);
        };
        if (const PluginLoadError err = pluginStaticCheck(e); err != PluginLoadError::None) { done(err, nullptr); continue; }
        HMODULE module = nullptr;
        {
            PluginCallScope scope(e.id, "load", e.dllPath);
            // plugin 自己帶的 DLL 放在同一個資料夾：從 DLL 所在的資料夾找相依的 DLL，不看目前目錄。
            module = LoadLibraryExW(e.dllPath.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
            if (!module) r.systemError = GetLastError();
        }
        if (!module) { done(PluginLoadError::LoadFailed, nullptr); continue; }
        auto entry = reinterpret_cast<MpPluginGetFuncs>(GetProcAddress(module, MP_PLUGIN_ENTRY_NAME));
        if (!entry) { done(PluginLoadError::NoEntry, module); continue; }
        // 照資訊宣告的版本講話：v1 的 plugin 只認得 1。
        const unsigned api = e.manifest.manifest.api;
        const MpPluginFuncs* funcs = nullptr;
        {
            PluginCallScope scope(e.id, "getFuncs", e.dllPath);
            funcs = entry(api);
        }
        if (!funcs || funcs->apiVersion != api || !funcs->probe || !funcs->readSize || !funcs->decode) {
            done(PluginLoadError::Incompatible, module);
            continue;
        }
        PluginSet::Plugin p;
        p.id = e.id;
        p.manifest = e.manifest.manifest;
        for (const auto& ext : pluginEffectiveExtensions(e))
            if (!containsId(taken, ext)) {
                p.extensions.push_back(ext);
                taken.push_back(ext);
            }
        p.module = module;
        p.funcs = funcs;
        set->plugins_.push_back(std::move(p));
        results.push_back(r);
    }
    return set;
}

void setActivePlugins(std::shared_ptr<const PluginSet> set) {
    std::lock_guard<std::mutex> lock(g_activeMutex);
    g_active = std::move(set);
}

std::shared_ptr<const PluginSet> activePlugins() {
    std::lock_guard<std::mutex> lock(g_activeMutex);
    return g_active;
}

bool pluginEnabled(const Settings& s, const std::wstring& id) {
    if (const PluginSetting* p = findPluginSetting(s, id)) return p->state == PluginState::Enabled;
    return isBundledPlugin(id);
}

std::vector<std::wstring> enabledPluginIds(const std::vector<PluginEntry>& entries, const Settings& s) {
    std::vector<std::wstring> out;
    for (const auto& e : entries)
        if (pluginEnabled(s, e.id)) out.push_back(e.id);
    return out;
}

std::vector<PluginConflictGroup> groupPluginConflicts(const std::vector<PluginConflict>& conflicts) {
    std::vector<PluginConflictGroup> out;
    auto sameIds = [](const std::vector<std::wstring>& a, const std::vector<std::wstring>& b) {
        if (a.size() != b.size()) return false;
        for (const auto& id : a)
            if (!containsId(b, id)) return false;
        return true;
    };
    for (const auto& c : conflicts) { // 已照副檔名排序，組裡的副檔名也就照順序
        auto it = std::find_if(out.begin(), out.end(), [&](const PluginConflictGroup& g) { return sameIds(g.ids, c.ids); });
        if (it == out.end()) out.push_back({c.ids, {c.extension}});
        else it->extensions.push_back(c.extension);
    }
    return out;
}

PluginStartup startPlugins(const std::wstring& pluginsDir, const std::vector<std::wstring>& runDirs, const std::wstring& exePath,
                           Settings& s) {
    PluginStartup out;
    for (const auto& dir : runDirs) {
        for (auto& c : collectPluginCrashes(dir, exePath, pluginsDir)) {
            const bool seen = std::any_of(out.crashes.begin(), out.crashes.end(),
                                          [&](const PluginCrash& x) { return sameId(x.pluginId, c.pluginId); });
            if (seen) continue;
            setPluginState(s, c.pluginId, PluginState::Crashed);
            out.crashes.push_back(std::move(c));
            out.settingsChanged = true;
        }
    }
    out.entries = scanPlugins(pluginsDir);
    std::vector<std::wstring> enabled = enabledPluginIds(out.entries, s);
    const auto conflicts = findPluginConflicts(out.entries, enabled);
    out.conflicts = groupPluginConflicts(conflicts);
    for (const auto& id : conflictingIds(conflicts)) {
        setPluginState(s, id, PluginState::Conflict);
        enabled.erase(std::remove_if(enabled.begin(), enabled.end(), [&](const std::wstring& x) { return sameId(x, id); }),
                      enabled.end());
        out.settingsChanged = true;
    }
    if (!enabled.empty()) {
        installPluginCrashFilter();
        for (const auto& dir : runDirs)
            if (startPluginRunLog(dir, exePath)) break;
    }
    setActivePlugins(loadPlugins(out.entries, enabled, out.results));
    return out;
}

bool pluginHandlesExtension(const std::wstring& ext) {
    auto set = activePlugins();
    return set && set->pluginForExtension(ext) != nullptr;
}

std::vector<std::wstring> pluginExtensions() {
    std::vector<std::wstring> out;
    if (auto set = activePlugins())
        for (const auto& p : set->plugins())
            for (const auto& e : p.extensions) out.push_back(e);
    return out;
}

std::wstring PluginMatch::formatName() const {
    if (!plugin || format < 0 || static_cast<size_t>(format) >= plugin->manifest.formats.size()) return L"";
    return plugin->manifest.formats[static_cast<size_t>(format)].name;
}

std::wstring PluginMatch::extension() const {
    if (!plugin) return L"";
    if (format >= 0 && static_cast<size_t>(format) < plugin->manifest.formats.size())
        for (const auto& e : plugin->manifest.formats[static_cast<size_t>(format)].extensions)
            if (containsId(plugin->extensions, e)) return e;
    return plugin->extensions.empty() ? L"" : plugin->extensions.front();
}

PluginMatch matchPluginFile(const std::wstring& path) {
    PluginMatch m;
    auto set = activePlugins();
    if (!set || set->plugins().empty()) return m;
    MappedFile file(path);
    if (!file.data) return m;
    for (const auto& p : set->plugins()) {
        if (p.extensions.empty()) continue; // 全部被內建或別的 plugin 拿走：不接手任何檔
        const size_t size = p.manifest.api == 1 ? std::min<size_t>(file.size, MP_PLUGIN_PROBE_BYTES) : file.size;
        int32_t format = -1;
        bool readable = false;
        {
            PluginCallScope scope(p.id, "probe", path);
            readable = guardedProbe(p.funcs, file.data, size, &format);
        }
        if (!readable) return PluginMatch{}; // 檔案被截短：後面的 plugin 也讀不到
        if (format < 0 || static_cast<size_t>(format) >= p.manifest.formats.size()) continue;
        m.set = set;
        m.plugin = &p;
        m.format = format;
        return m;
    }
    return m;
}

bool pluginReadSize(const PluginMatch& m, const std::wstring& path, int& width, int& height) {
    width = height = 0;
    if (!m) return false;
    MappedFile file(path);
    if (!file.data) return false;
    int32_t w = 0, h = 0, rc = MP_ERR_INTERNAL;
    bool readable = false;
    {
        PluginCallScope scope(m.plugin->id, "readSize", path);
        readable = guardedReadSize(m.plugin->funcs, m.format, file.data, file.size, &w, &h, &rc);
    }
    if (!readable || rc != MP_OK || w <= 0 || h <= 0) return false;
    width = w;
    height = h;
    return true;
}

PluginDecoded pluginDecode(const PluginMatch& m, const std::wstring& path) {
    PluginDecoded out;
    if (!m) { out.error.code = IoErrorCode::DecodeFailed; return out; }
    MappedFile file(path);
    if (!file.data) {
        out.error.code = IoErrorCode::FileUnreadable;
        out.error.systemError = file.error;
        return out;
    }
    AllocContext ctx;
    ctx.image = &out.image;
    MpHost host{&ctx, &allocImage};
    MpDecodeInfo info;
    std::memset(&info, 0, sizeof(info));
    int32_t rc = MP_ERR_INTERNAL;
    bool readable = false;
    {
        PluginCallScope scope(m.plugin->id, "decode", path);
        readable = guardedDecode(m.plugin->funcs, m.format, file.data, file.size, &host, &info, &rc);
    }
    info.message[sizeof(info.message) - 1] = '\0';
    if (!readable) {
        out.image = StraightImage{};
        out.error.code = IoErrorCode::FileUnreadable; // 解到一半檔案被截短
        return out;
    }
    if (ctx.outOfMemory || rc == MP_ERR_OUT_OF_MEMORY) {
        out.image = StraightImage{};
        out.error.code = IoErrorCode::OutOfMemory;
        return out;
    }
    if (rc != MP_OK) {
        out.image = StraightImage{};
        out.error = pluginFailure(m, "decode", rc, info.message);
        return out;
    }
    if (out.image.empty()) {
        out.error = pluginFailure(m, "decode", rc, "returned success without an image");
        return out;
    }
    // 解析度：plugin 沒給、或不合理（同 WIC 的路徑）就是 96。
    if (info.dpiX >= 1 && info.dpiX <= 100000 && info.dpiY >= 1 && info.dpiY <= 100000) out.dpi = Dpi{info.dpiX, info.dpiY};
    return out;
}

} // namespace mp
