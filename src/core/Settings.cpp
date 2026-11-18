#include "core/Settings.h"

#include <windows.h>

#include <algorithm>
#include <functional>
#include <vector>

#include "core/TempDir.h"
#include "core/TextFile.h"
#include "core/TextUtil.h"

namespace mp {

namespace {

bool parseBool(const std::wstring& v, bool def) {
    if (v == L"1" || v == L"true" || v == L"yes") return true;
    if (v == L"0" || v == L"false" || v == L"no") return false;
    return def;
}

// saveSettings 取代原檔暫時失敗時：重試幾次、每次隔多久。
constexpr int kSaveRetries = 50;
constexpr DWORD kSaveRetryMs = 5;
// saveSettingsMerged 等別的 mpainter 存完，最多等多久。等不到就照樣存（寧可晚一步蓋掉，也不要不存）。
constexpr DWORD kMergeLockWaitMs = 2000;

// 檢視設定的 ini key -> 欄位。parseSettings 的頂層與 [profile] 區段共用。不是檢視設定的 key 回 false。
bool setPrefKey(ViewPrefs& p, const std::wstring& key, const std::wstring& val) {
    if (key == L"viewMode") {
        p.viewMode = viewModeFromKey(val); // 不認得的退回 fitwindow
        return true;
    }
    for (const ViewFlag& f : kViewFlags) {
        if (key != f.key) continue;
        p.*f.member = parseBool(val, p.*f.member);
        return true;
    }
    return false;
}

std::wstring serializePrefs(const ViewPrefs& p) {
    std::wstring out = L"viewMode=" + std::wstring(viewModeKey(p.viewMode)) + L"\n";
    for (const ViewFlag& f : kViewFlags) out += std::wstring(f.key) + L"=" + (p.*f.member ? L"1" : L"0") + L"\n";
    return out;
}

bool sameProfiles(const std::vector<Profile>& a, const std::vector<Profile>& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (a[i].name != b[i].name || a[i].prefs != b[i].prefs) return false;
    return true;
}

bool contains(const std::vector<std::wstring>& v, const std::wstring& s) { return std::find(v.begin(), v.end(), s) != v.end(); }

// plugin 狀態 <-> ini 的字。
constexpr struct {
    PluginState state;
    const wchar_t* key;
} kPluginStateKeys[] = {
    {PluginState::Enabled, L"on"},
    {PluginState::Disabled, L"off"},
    {PluginState::Crashed, L"crashed"},
    {PluginState::Conflict, L"conflict"},
};

const wchar_t* pluginStateKey(PluginState state) {
    for (const auto& k : kPluginStateKeys)
        if (k.state == state) return k.key;
    return L"off";
}

// 兩份 plugin 紀錄一樣（不看順序；id 不分大小寫）。
bool samePluginSettings(const std::vector<PluginSetting>& a, const std::vector<PluginSetting>& b) {
    if (a.size() != b.size()) return false;
    Settings sb;
    sb.plugins = b;
    for (const auto& x : a) {
        const PluginSetting* y = findPluginSetting(sb, x.id);
        if (!y || y->state != x.state) return false;
    }
    return true;
}

} // namespace

const PluginSetting* findPluginSetting(const Settings& s, const std::wstring& id) {
    for (const auto& p : s.plugins)
        if (_wcsicmp(p.id.c_str(), id.c_str()) == 0) return &p;
    return nullptr;
}

void setPluginState(Settings& s, const std::wstring& id, PluginState state) {
    for (auto& p : s.plugins) {
        if (_wcsicmp(p.id.c_str(), id.c_str()) == 0) {
            p.state = state;
            return;
        }
    }
    s.plugins.push_back(PluginSetting{id, state});
}

bool ViewPrefs::operator==(const ViewPrefs& o) const {
    if (viewMode != o.viewMode) return false;
    for (const ViewFlag& f : kViewFlags)
        if (this->*f.member != o.*f.member) return false;
    return true;
}

Profile profileFromCurrent(const Settings& s) { return Profile{s.profileName, s.view}; }

void applyProfile(Settings& s, const Profile& p) {
    s.profileName = p.name;
    s.view = p.prefs;
}

const Profile* findProfile(const Settings& s, const std::wstring& name) {
    for (const auto& p : s.profiles)
        if (_wcsicmp(p.name.c_str(), name.c_str()) == 0) return &p;
    return nullptr;
}

Profile* findProfile(Settings& s, const std::wstring& name) {
    return const_cast<Profile*>(findProfile(static_cast<const Settings&>(s), name));
}

bool profileModified(const Settings& s) {
    const Profile* p = findProfile(s, s.profileName);
    return !p || p->prefs != s.view;
}

int prefetchMBFromText(const std::wstring& text, int fallback) {
    return clampedIntFromText(text, fallback, kPrefetchMinMB, kPrefetchMaxMB);
}

int intSettingFromText(const IntSetting& field, const std::wstring& text, int fallback) {
    return clampedIntFromText(text, fallback, field.min, field.max);
}

std::vector<Profile> defaultProfiles(const std::wstring& generalName, const std::wstring& comicName) {
    Profile general;
    general.name = generalName;
    general.prefs.viewMode = ViewMode::Actual;
    Profile comic;
    comic.name = comicName;
    comic.prefs.viewMode = ViewMode::TwoPage;
    comic.prefs.highQuality = true;
    comic.prefs.autoNextBook = true;
    comic.prefs.warnMetadataLoss = false;
    return {general, comic};
}

bool ensureProfiles(Settings& s, const std::wstring& generalName, const std::wstring& comicName) {
    bool changed = false;
    if (s.profiles.empty()) {
        // 第一次執行（或舊版 ini）：建內建的兩個，目前的值也套成第一個，讓一開始就是乾淨的「一般模式」而不是「已調整」。
        s.profiles = defaultProfiles(generalName, comicName);
        applyProfile(s, s.profiles.front());
        changed = true;
    }
    if (!findProfile(s, s.profileName)) { s.profileName = s.profiles.front().name; changed = true; }
    return changed;
}

Settings parseSettings(const std::wstring& text) {
    Settings s;
    bool inProfile = false, skipSection = false;
    size_t pos = 0;
    while (pos <= text.size()) {
        size_t nl = text.find(L'\n', pos);
        std::wstring raw = text.substr(pos, nl == std::wstring::npos ? std::wstring::npos : nl - pos);
        pos = nl == std::wstring::npos ? text.size() + 1 : nl + 1;
        while (!raw.empty() && (raw.back() == L'\r' || raw.back() == L'\n')) raw.pop_back();
        std::wstring line = trim(raw);
        if (line.empty() || line[0] == L';' || line[0] == L'#') continue;
        if (line[0] == L'[') {
            // 區段：[mpainter] 是頂層，[profile] 開一個新模式，其他的整段略過。
            std::wstring sec = trim(line.substr(1, line.find(L']') == std::wstring::npos ? std::wstring::npos : line.find(L']') - 1));
            inProfile = _wcsicmp(sec.c_str(), L"profile") == 0;
            skipSection = !inProfile && _wcsicmp(sec.c_str(), L"mpainter") != 0;
            if (inProfile) s.profiles.push_back(Profile{});
            continue;
        }
        if (skipSection) continue;
        size_t eq = line.find(L'=');
        if (eq == std::wstring::npos) continue;
        std::wstring key = trim(line.substr(0, eq)), val = trim(line.substr(eq + 1));
        if (inProfile) {
            Profile& p = s.profiles.back();
            if (key == L"name") p.name = val;
            else setPrefKey(p.prefs, key, val);
            continue;
        }
        // 密碼前後的空白是密碼的一部分：從沒 trim 過的那行取值（第一個 = 的位置相同，前面只可能少了空白）。
        if (key == L"password") val = raw.substr(raw.find(L'=') + 1);
        if (setPrefKey(s.view, key, val)) continue;
        bool isInt = false;
        for (const IntSetting& f : kIntSettings) {
            if (key != f.key) continue;
            s.*f.member = intSettingFromText(f, val, s.*f.member);
            isInt = true;
        }
        if (isInt) continue;
        if (key == L"askedDefault") s.askedDefault = parseBool(val, s.askedDefault);
        else if (key == L"tipShown") { try { s.tipShown = std::stoi(val); } catch (...) {} }
        else if (key == L"contextMenu") s.contextMenu = parseBool(val, s.contextMenu);
        else if (key == L"language") s.language = val;
        else if (key == L"profile") s.profileName = val;
        else if (key == L"unzipDir") s.unzipDir = val;
        else if (key == L"autoCheckUpdate") s.autoCheckUpdate = parseBool(val, s.autoCheckUpdate);
        else if (key == L"skipVersion") s.skipVersion = val;
        else if (key == L"updateUrl") s.updateUrl = val;
        else if (key == L"pinnedNames") {
            s.pinnedNames.clear();
            size_t p = 0;
            while (p <= val.size()) {
                size_t bar = val.find(L'|', p);
                std::wstring item = trim(val.substr(p, bar == std::wstring::npos ? std::wstring::npos : bar - p));
                if (!item.empty()) s.pinnedNames.push_back(item);
                if (bar == std::wstring::npos) break;
                p = bar + 1;
            }
        }
        else if (key == L"password") { if (!val.empty()) s.passwords.push_back(val); }
        else if (key == L"plugin") {
            // <id>|<狀態>。id 是資料夾名，不會有 |。狀態不認得、id 空的、重複的（留第一個）略過。
            const size_t bar = val.rfind(L'|');
            if (bar == std::wstring::npos) continue;
            const std::wstring id = trim(val.substr(0, bar)), st = trim(val.substr(bar + 1));
            if (id.empty() || findPluginSetting(s, id)) continue;
            for (const auto& k : kPluginStateKeys)
                if (st == k.key) s.plugins.push_back(PluginSetting{id, k.state});
        }
        else if (key == L"window") {
            int x = 0, y = 0, w = 0, h = 0, m = 0;
            if (swscanf_s(val.c_str(), L"%d,%d,%d,%d,%d", &x, &y, &w, &h, &m) == 5 && w > 0 && h > 0) {
                s.winX = x; s.winY = y; s.winW = w; s.winH = h; s.winMax = m != 0;
            }
        }
    }
    // 模式：沒名字的丟掉、名字重複的留第一個。
    std::vector<Profile> kept;
    for (auto& p : s.profiles) {
        p.name = trim(p.name);
        if (p.name.empty()) continue;
        bool dup = false;
        for (const auto& k : kept) if (_wcsicmp(k.name.c_str(), p.name.c_str()) == 0) dup = true;
        if (dup) continue;
        kept.push_back(p);
    }
    s.profiles = std::move(kept);
    if (s.tipShown < 0) s.tipShown = 0;
    return s;
}

std::wstring serializeSettings(const Settings& s) {
    std::wstring out = L"[mpainter]\n";
    out += L"askedDefault=" + std::wstring(s.askedDefault ? L"1" : L"0") + L"\n";
    out += L"tipShown=" + std::to_wstring(s.tipShown) + L"\n";
    out += L"contextMenu=" + std::wstring(s.contextMenu ? L"1" : L"0") + L"\n";
    out += L"language=" + s.language + L"\n";
    out += L"profile=" + s.profileName + L"\n";
    out += serializePrefs(s.view);
    out += L"unzipDir=" + s.unzipDir + L"\n";
    out += L"autoCheckUpdate=" + std::wstring(s.autoCheckUpdate ? L"1" : L"0") + L"\n";
    out += L"skipVersion=" + s.skipVersion + L"\n";
    for (const IntSetting& f : kIntSettings) out += std::wstring(f.key) + L"=" + std::to_wstring(s.*f.member) + L"\n";
    if (!s.updateUrl.empty()) out += L"updateUrl=" + s.updateUrl + L"\n";
    std::wstring pinned;
    for (const auto& n : s.pinnedNames) pinned += (pinned.empty() ? L"" : L"|") + n;
    out += L"pinnedNames=" + pinned + L"\n";
    for (const auto& pw : s.passwords) out += L"password=" + pw + L"\n";
    for (const auto& p : s.plugins) out += L"plugin=" + p.id + L"|" + pluginStateKey(p.state) + L"\n";
    if (s.winW > 0 && s.winH > 0)
        out += L"window=" + std::to_wstring(s.winX) + L"," + std::to_wstring(s.winY) + L"," + std::to_wstring(s.winW) + L"," +
               std::to_wstring(s.winH) + L"," + (s.winMax ? L"1" : L"0") + L"\n";
    for (const auto& p : s.profiles) {
        out += L"\n[profile]\n";
        out += L"name=" + p.name + L"\n";
        out += serializePrefs(p.prefs);
    }
    return out;
}

DialogChanges mergeDialogSettings(Settings& current, const Settings& fromDialog, const std::wstring& exeDir) {
    DialogChanges c;
    c.contextMenu = fromDialog.contextMenu != current.contextMenu;
    c.pinnedNames = fromDialog.pinnedNames != current.pinnedNames;
    c.unzipRoot = _wcsicmp(resolveUnzipRoot(fromDialog.unzipDir, exeDir).c_str(), resolveUnzipRoot(current.unzipDir, exeDir).c_str()) != 0;
    c.undoLimits = fromDialog.undoLimit != current.undoLimit || fromDialog.undoMemoryMB != current.undoMemoryMB;
    c.prefetch = fromDialog.prefetchMB != current.prefetchMB;
    c.profiles = !sameProfiles(fromDialog.profiles, current.profiles) || fromDialog.profileName != current.profileName;
    c.language = _wcsicmp(fromDialog.language.c_str(), current.language.c_str()) != 0;
    c.plugins = !samePluginSettings(fromDialog.plugins, current.plugins);
    // 換了模式、或正在用的那個內容被改了，就要重新套用（目前的值 = 模式本體）。
    const Profile* np = findProfile(fromDialog, fromDialog.profileName);
    const Profile* op = findProfile(current, current.profileName);
    c.reapplyProfile = np && (!op || _wcsicmp(np->name.c_str(), op->name.c_str()) != 0 || np->prefs != op->prefs);

    // 整份照抄，再把面板不管的兩樣放回去。
    const std::wstring language = current.language;
    const ViewPrefs view = current.view;
    current = fromDialog;
    current.language = language;
    current.view = view;
    return c;
}

Settings mergeSettings(const Settings& base, const Settings& mine, const Settings& disk) {
    Settings out = disk;
    // 一般欄位：這個視窗改過（mine 和 base 不同）就用 mine。
    auto pick = [&](auto Settings::*member) {
        if (mine.*member != base.*member) out.*member = mine.*member;
    };
    pick(&Settings::askedDefault);
    pick(&Settings::tipShown);
    pick(&Settings::contextMenu);
    pick(&Settings::language);
    pick(&Settings::autoCheckUpdate);
    pick(&Settings::skipVersion);
    pick(&Settings::updateUrl);
    pick(&Settings::unzipDir);
    pick(&Settings::pinnedNames);
    pick(&Settings::profileName);
    for (const IntSetting& f : kIntSettings) pick(f.member);
    // 目前的檢視值：模式與每個開關各自算。
    if (mine.view.viewMode != base.view.viewMode) out.view.viewMode = mine.view.viewMode;
    for (const ViewFlag& f : kViewFlags)
        if (mine.view.*f.member != base.view.*f.member) out.view.*f.member = mine.view.*f.member;
    // 模式清單：整份。
    if (!sameProfiles(mine.profiles, base.profiles)) out.profiles = mine.profiles;
    // 視窗位置：五個數字一組。
    if (mine.winX != base.winX || mine.winY != base.winY || mine.winW != base.winW || mine.winH != base.winH ||
        mine.winMax != base.winMax) {
        out.winX = mine.winX; out.winY = mine.winY; out.winW = mine.winW; out.winH = mine.winH; out.winMax = mine.winMax;
    }
    // plugin：逐個。這個視窗改過（狀態不同、加了、刪了紀錄）的照 mine，其他照 disk。
    for (const auto* list : {&base.plugins, &mine.plugins}) {
        for (const auto& entry : *list) {
            const PluginSetting* m = findPluginSetting(mine, entry.id);
            const PluginSetting* b = findPluginSetting(base, entry.id);
            if ((m == nullptr) == (b == nullptr) && (!m || m->state == b->state)) continue; // 沒改
            if (m) {
                setPluginState(out, m->id, m->state);
            } else {
                out.plugins.erase(std::remove_if(out.plugins.begin(), out.plugins.end(),
                                                 [&](const PluginSetting& x) { return _wcsicmp(x.id.c_str(), entry.id.c_str()) == 0; }),
                                  out.plugins.end());
            }
        }
    }
    // 密碼：逐筆。disk 的每一筆都留，除了這個視窗刪掉的；這個視窗加的接在後面。
    out.passwords.clear();
    for (const auto& pw : disk.passwords) {
        const bool removedByMe = contains(base.passwords, pw) && !contains(mine.passwords, pw);
        if (!removedByMe && !contains(out.passwords, pw)) out.passwords.push_back(pw);
    }
    for (const auto& pw : mine.passwords) {
        const bool addedByMe = !contains(base.passwords, pw);
        if (addedByMe && !contains(out.passwords, pw)) out.passwords.push_back(pw);
    }
    return out;
}

SettingsLoad loadSettings(const std::wstring& path, Settings& out) {
    out = Settings{};
    std::wstring text;
    switch (readUtf8File(path, text)) {
    case FileRead::Missing: return SettingsLoad::Missing;
    case FileRead::Unreadable: return SettingsLoad::Unreadable; // 含讀到一半失敗：半份不能當成設定用
    case FileRead::Ok: break;
    }
    out = parseSettings(text);
    return SettingsLoad::Loaded;
}

bool saveSettings(const std::wstring& path, const Settings& s, unsigned long* systemError) {
    DWORD why = 0; // 失敗的那一步的 GetLastError
    if (systemError) *systemError = 0;
    auto fail = [&] {
        if (systemError) *systemError = why;
        return false;
    };
    std::string bytes = wideToUtf8(serializeSettings(s));
    DWORD attr = GetFileAttributesW(path.c_str());
    if (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_READONLY)) {
        why = ERROR_ACCESS_DENIED;
        return fail();
    }
    std::wstring tmp = path + L"." + std::to_wstring(GetCurrentProcessId()) + L".tmp";
    HANDLE h = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        why = GetLastError();
        return fail();
    }
    DWORD written = 0;
    bool ok = WriteFile(h, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr) && written == bytes.size() &&
              FlushFileBuffers(h);
    if (!ok) why = GetLastError();
    CloseHandle(h);
    // 取代原檔。原檔剛好被別人開著（另一個 mpainter 正在讀、防毒掃描）時會暫時失敗，等一下再試，最多約 0.25 秒。
    for (int attempt = 0; ok; ++attempt) {
        if (MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) return true;
        why = GetLastError();
        if ((why != ERROR_SHARING_VIOLATION && why != ERROR_ACCESS_DENIED) || attempt >= kSaveRetries) break;
        Sleep(kSaveRetryMs);
    }
    DeleteFileW(tmp.c_str());
    return fail();
}

bool saveSettingsMerged(const std::wstring& path, const Settings& base, const Settings& mine, Settings& written,
                        unsigned long* systemError) {
    if (systemError) *systemError = 0;
    // 「讀 → 合併 → 寫」要一口氣做完：兩個視窗同時做的話，後寫的會蓋掉先寫的。用路徑當名字的 mutex 排隊。
    wchar_t full[MAX_PATH * 2];
    DWORD n = GetFullPathNameW(path.c_str(), MAX_PATH * 2, full, nullptr);
    const std::wstring key = lowerCopy(n > 0 && n < MAX_PATH * 2 ? std::wstring(full, n) : path);
    const std::wstring name = L"Local\\mpainter-ini-" + std::to_wstring(std::hash<std::wstring>{}(key));
    HANDLE mutex = CreateMutexW(nullptr, FALSE, name.c_str());
    const DWORD waited = mutex ? WaitForSingleObject(mutex, kMergeLockWaitMs) : WAIT_FAILED;
    const bool locked = waited == WAIT_OBJECT_0 || waited == WAIT_ABANDONED;

    bool ok = false;
    Settings disk;
    SettingsLoad loaded = loadSettings(path, disk);
    if (loaded != SettingsLoad::Unreadable) { // 讀不到時不寫：不知道裡面有什麼，寫了可能蓋掉別人的
        written = loaded == SettingsLoad::Loaded ? mergeSettings(base, mine, disk) : mine;
        ok = saveSettings(path, written, systemError);
    }
    if (locked) ReleaseMutex(mutex);
    if (mutex) CloseHandle(mutex);
    return ok;
}

bool isPathWritable(const std::wstring& path) {
    DWORD attr = GetFileAttributesW(path.c_str());
    if (attr != INVALID_FILE_ATTRIBUTES) {
        if (attr & FILE_ATTRIBUTE_READONLY) return false;
        HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                               FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE) return false;
        CloseHandle(h);
        return true;
    }
    // 檔案不存在：試著在同資料夾建一個暫存檔。
    std::wstring probe = path + L".probe";
    HANDLE h = CreateFileW(probe.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                           FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    CloseHandle(h);
    return true;
}

std::wstring settingsPathForExe(const std::wstring& exePath) {
    size_t s = exePath.find_last_of(L"\\/");
    std::wstring dir = s == std::wstring::npos ? L"" : exePath.substr(0, s + 1);
    return dir + L"mpainter.ini";
}

} // namespace mp
