#include "core/Settings.h"

#include <windows.h>

#include <vector>

namespace mp {

namespace {

std::wstring trim(const std::wstring& s) {
    size_t a = s.find_first_not_of(L" \t\r\n");
    if (a == std::wstring::npos) return L"";
    size_t b = s.find_last_not_of(L" \t\r\n");
    return s.substr(a, b - a + 1);
}

bool parseBool(const std::wstring& v, bool def) {
    if (v == L"1" || v == L"true" || v == L"yes") return true;
    if (v == L"0" || v == L"false" || v == L"no") return false;
    return def;
}

std::wstring utf8ToWide(const std::string& s) {
    if (s.empty()) return L"";
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring out(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), n);
    return out;
}

std::string wideToUtf8(const std::wstring& s) {
    if (s.empty()) return "";
    int n = WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0, nullptr, nullptr);
    std::string out(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), n, nullptr, nullptr);
    return out;
}

const wchar_t* const kViewModes[] = {L"fitwindow", L"fitwidth", L"fitheight", L"actual", L"twopage"};

std::wstring normalizeViewMode(const std::wstring& v) {
    for (auto* m : kViewModes) if (v == m) return v;
    return L"fitwindow";
}

// 模式欄位：ini key -> 欄位。parseSettings 的頂層與 [profile] 區段共用。
bool setPrefKey(Profile& p, const std::wstring& key, const std::wstring& val) {
    if (key == L"viewMode") p.viewMode = val;
    else if (key == L"upscaleSmall") p.upscaleSmall = parseBool(val, p.upscaleSmall);
    else if (key == L"highQuality") p.highQuality = parseBool(val, p.highQuality);
    else if (key == L"scrollFlip") p.scrollFlip = parseBool(val, p.scrollFlip);
    else if (key == L"autoNextBook") p.autoNextBook = parseBool(val, p.autoNextBook);
    else if (key == L"rightToLeft") p.rightToLeft = parseBool(val, p.rightToLeft);
    else if (key == L"firstPageAlone") p.firstPageAlone = parseBool(val, p.firstPageAlone);
    else if (key == L"warnMetadataLoss") p.warnMetadataLoss = parseBool(val, p.warnMetadataLoss);
    else return false;
    return true;
}

std::wstring serializePrefs(const Profile& p) {
    auto b = [](bool v) { return std::wstring(v ? L"1" : L"0"); };
    std::wstring out;
    out += L"viewMode=" + p.viewMode + L"\n";
    out += L"upscaleSmall=" + b(p.upscaleSmall) + L"\n";
    out += L"highQuality=" + b(p.highQuality) + L"\n";
    out += L"scrollFlip=" + b(p.scrollFlip) + L"\n";
    out += L"autoNextBook=" + b(p.autoNextBook) + L"\n";
    out += L"rightToLeft=" + b(p.rightToLeft) + L"\n";
    out += L"firstPageAlone=" + b(p.firstPageAlone) + L"\n";
    out += L"warnMetadataLoss=" + b(p.warnMetadataLoss) + L"\n";
    return out;
}

} // namespace

bool samePrefs(const Profile& a, const Profile& b) {
    return a.viewMode == b.viewMode && a.upscaleSmall == b.upscaleSmall && a.highQuality == b.highQuality &&
           a.scrollFlip == b.scrollFlip && a.autoNextBook == b.autoNextBook && a.rightToLeft == b.rightToLeft &&
           a.firstPageAlone == b.firstPageAlone && a.warnMetadataLoss == b.warnMetadataLoss;
}

Profile profileFromCurrent(const Settings& s) {
    Profile p;
    p.name = s.profileName;
    p.viewMode = s.viewMode;
    p.upscaleSmall = s.upscaleSmall;
    p.highQuality = s.highQuality;
    p.scrollFlip = s.scrollFlip;
    p.autoNextBook = s.autoNextBook;
    p.rightToLeft = s.rightToLeft;
    p.firstPageAlone = s.firstPageAlone;
    p.warnMetadataLoss = s.warnMetadataLoss;
    return p;
}

void applyProfile(Settings& s, const Profile& p) {
    s.profileName = p.name;
    s.viewMode = normalizeViewMode(p.viewMode);
    s.upscaleSmall = p.upscaleSmall;
    s.highQuality = p.highQuality;
    s.scrollFlip = p.scrollFlip;
    s.autoNextBook = p.autoNextBook;
    s.rightToLeft = p.rightToLeft;
    s.firstPageAlone = p.firstPageAlone;
    s.warnMetadataLoss = p.warnMetadataLoss;
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
    return !p || !samePrefs(*p, profileFromCurrent(s));
}

std::vector<Profile> defaultProfiles(const std::wstring& generalName, const std::wstring& comicName) {
    Profile general;
    general.name = generalName;
    general.viewMode = L"actual";
    Profile comic;
    comic.name = comicName;
    comic.viewMode = L"twopage";
    comic.highQuality = true;
    comic.autoNextBook = true;
    comic.warnMetadataLoss = false;
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
    Profile cur; // 頂層的八個欄位先收在這裡，最後套進 s（和 [profile] 區段共用解析）
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
            else setPrefKey(p, key, val);
            continue;
        }
        // 密碼前後的空白是密碼的一部分：從沒 trim 過的那行取值（第一個 = 的位置相同，前面只可能少了空白）。
        if (key == L"password") val = raw.substr(raw.find(L'=') + 1);
        if (setPrefKey(cur, key, val)) continue;
        if (key == L"askedDefault") s.askedDefault = parseBool(val, s.askedDefault);
        else if (key == L"tipShown") { try { s.tipShown = std::stoi(val); } catch (...) {} }
        else if (key == L"contextMenu") s.contextMenu = parseBool(val, s.contextMenu);
        else if (key == L"language") s.language = val;
        else if (key == L"profile") s.profileName = val;
        else if (key == L"unzipDir") s.unzipDir = val;
        else if (key == L"autoCheckUpdate") s.autoCheckUpdate = parseBool(val, s.autoCheckUpdate);
        else if (key == L"skipVersion") s.skipVersion = val;
        else if (key == L"undoLimit") s.undoLimit = undoLimitFromText(val, s.undoLimit);
        else if (key == L"undoMemoryMB") s.undoMemoryMB = undoMemoryFromText(val, s.undoMemoryMB);
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
        else if (key == L"window") {
            int x = 0, y = 0, w = 0, h = 0, m = 0;
            if (swscanf_s(val.c_str(), L"%d,%d,%d,%d,%d", &x, &y, &w, &h, &m) == 5 && w > 0 && h > 0) {
                s.winX = x; s.winY = y; s.winW = w; s.winH = h; s.winMax = m != 0;
            }
        }
    }
    cur.name = s.profileName;
    applyProfile(s, cur); // 順便把不認得的 viewMode 退回 fitwindow
    // 模式：沒名字的丟掉、名字重複的留第一個、viewMode 正規化。
    std::vector<Profile> kept;
    for (auto& p : s.profiles) {
        p.name = trim(p.name);
        if (p.name.empty()) continue;
        bool dup = false;
        for (const auto& k : kept) if (_wcsicmp(k.name.c_str(), p.name.c_str()) == 0) dup = true;
        if (dup) continue;
        p.viewMode = normalizeViewMode(p.viewMode);
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
    out += serializePrefs(profileFromCurrent(s));
    out += L"unzipDir=" + s.unzipDir + L"\n";
    out += L"autoCheckUpdate=" + std::wstring(s.autoCheckUpdate ? L"1" : L"0") + L"\n";
    out += L"skipVersion=" + s.skipVersion + L"\n";
    out += L"undoLimit=" + std::to_wstring(s.undoLimit) + L"\n";
    out += L"undoMemoryMB=" + std::to_wstring(s.undoMemoryMB) + L"\n";
    if (!s.updateUrl.empty()) out += L"updateUrl=" + s.updateUrl + L"\n";
    std::wstring pinned;
    for (const auto& n : s.pinnedNames) pinned += (pinned.empty() ? L"" : L"|") + n;
    out += L"pinnedNames=" + pinned + L"\n";
    for (const auto& pw : s.passwords) out += L"password=" + pw + L"\n";
    if (s.winW > 0 && s.winH > 0)
        out += L"window=" + std::to_wstring(s.winX) + L"," + std::to_wstring(s.winY) + L"," + std::to_wstring(s.winW) + L"," +
               std::to_wstring(s.winH) + L"," + (s.winMax ? L"1" : L"0") + L"\n";
    for (const auto& p : s.profiles) {
        out += L"\n[profile]\n";
        out += L"name=" + p.name + L"\n";
        out += serializePrefs(p);
    }
    return out;
}

bool loadSettings(const std::wstring& path, Settings& out) {
    out = Settings{};
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    std::string bytes;
    char buf[4096];
    DWORD got = 0;
    while (ReadFile(h, buf, sizeof(buf), &got, nullptr) && got > 0) bytes.append(buf, got);
    CloseHandle(h);
    if (bytes.size() >= 3 && static_cast<unsigned char>(bytes[0]) == 0xEF && static_cast<unsigned char>(bytes[1]) == 0xBB &&
        static_cast<unsigned char>(bytes[2]) == 0xBF)
        bytes.erase(0, 3);
    out = parseSettings(utf8ToWide(bytes));
    return true;
}

bool saveSettings(const std::wstring& path, const Settings& s) {
    std::string bytes = wideToUtf8(serializeSettings(s));
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    bool ok = WriteFile(h, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr) && written == bytes.size();
    CloseHandle(h);
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
