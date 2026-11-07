#include "core/Lang.h"

#include <windows.h>

#include <algorithm>

namespace mp {

namespace {

std::wstring trim(const std::wstring& s) {
    size_t a = s.find_first_not_of(L" \t\r\n");
    if (a == std::wstring::npos) return L"";
    size_t b = s.find_last_not_of(L" \t\r\n");
    return s.substr(a, b - a + 1);
}

std::wstring unescape(const std::wstring& s) {
    std::wstring out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] != L'\\' || i + 1 >= s.size()) { out += s[i]; continue; }
        wchar_t c = s[++i];
        switch (c) {
        case L'n': out += L'\n'; break;
        case L't': out += L'\t'; break;
        case L'\\': out += L'\\'; break;
        case L'=': out += L'='; break;
        default: out += L'\\'; out += c; break;
        }
    }
    return out;
}

// 找第一個沒被反斜線跳脫的 '='。
size_t findSeparator(const std::wstring& line) {
    for (size_t i = 0; i < line.size(); ++i) {
        if (line[i] == L'\\') { ++i; continue; }
        if (line[i] == L'=') return i;
    }
    return std::wstring::npos;
}

std::wstring utf8ToWide(const std::string& s) {
    if (s.empty()) return L"";
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring out(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), n);
    return out;
}

std::wstring lower(std::wstring s) {
    for (auto& c : s) c = static_cast<wchar_t>(towlower(c));
    return s;
}

std::wstring langPart(const std::wstring& code) {
    size_t d = code.find_first_of(L"-_");
    return lower(d == std::wstring::npos ? code : code.substr(0, d));
}

LangTable g_active;

} // namespace

LangTable parseLangText(const std::wstring& text) {
    LangTable t;
    size_t pos = 0;
    while (pos <= text.size()) {
        size_t nl = text.find(L'\n', pos);
        std::wstring line = trim(text.substr(pos, nl == std::wstring::npos ? std::wstring::npos : nl - pos));
        pos = nl == std::wstring::npos ? text.size() + 1 : nl + 1;
        if (line.empty() || line[0] == L'#' || line[0] == L';') continue;
        size_t eq = findSeparator(line);
        if (eq == std::wstring::npos) continue;
        std::wstring key = unescape(trim(line.substr(0, eq)));
        std::wstring val = unescape(trim(line.substr(eq + 1)));
        if (key.empty()) continue;
        if (key == L"_code") t.code = val;
        else if (key == L"_name") t.name = val;
        else t.map[key] = val;
    }
    return t;
}

bool loadLangFile(const std::wstring& path, LangTable& out) {
    out = LangTable{};
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    std::string bytes;
    char buf[8192];
    DWORD got = 0;
    while (ReadFile(h, buf, sizeof(buf), &got, nullptr) && got > 0) bytes.append(buf, got);
    CloseHandle(h);
    if (bytes.size() >= 3 && static_cast<unsigned char>(bytes[0]) == 0xEF && static_cast<unsigned char>(bytes[1]) == 0xBB &&
        static_cast<unsigned char>(bytes[2]) == 0xBF)
        bytes.erase(0, 3);
    out = parseLangText(utf8ToWide(bytes));
    return true;
}

std::vector<LangTable> loadLangDir(const std::wstring& dir) {
    std::vector<std::wstring> names;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"\\*.txt").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) names.push_back(fd.cFileName);
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    std::sort(names.begin(), names.end(), [](const std::wstring& a, const std::wstring& b) { return _wcsicmp(a.c_str(), b.c_str()) < 0; });
    std::vector<LangTable> out;
    for (const auto& n : names) {
        LangTable t;
        if (loadLangFile(dir + L"\\" + n, t) && !t.code.empty()) out.push_back(std::move(t));
    }
    return out;
}

std::wstring pickLanguage(const std::wstring& setting, const std::wstring& systemLocale, const std::vector<std::wstring>& available) {
    auto has = [&](const std::wstring& code) {
        return std::any_of(available.begin(), available.end(), [&](const std::wstring& a) { return _wcsicmp(a.c_str(), code.c_str()) == 0; });
    };
    auto canonical = [&](const std::wstring& code) -> std::wstring {
        for (const auto& a : available)
            if (_wcsicmp(a.c_str(), code.c_str()) == 0) return a;
        return code;
    };
    if (!setting.empty() && has(setting)) return canonical(setting);
    if (!systemLocale.empty()) {
        if (has(systemLocale)) return canonical(systemLocale);
        std::wstring lang = langPart(systemLocale);
        if (lang == L"zh") {
            std::wstring low = lower(systemLocale);
            bool hant = low.find(L"hant") != std::wstring::npos || low.find(L"-tw") != std::wstring::npos ||
                        low.find(L"-hk") != std::wstring::npos || low.find(L"-mo") != std::wstring::npos;
            if (hant && has(L"zh-TW")) return canonical(L"zh-TW");
        } else {
            for (const auto& a : available)
                if (langPart(a) == lang) return a;
        }
    }
    return L"en";
}

void setActiveLang(LangTable table) { g_active = std::move(table); }

const LangTable& activeLang() { return g_active; }

const wchar_t* tr(const wchar_t* key) {
    if (g_active.map.empty()) return key;
    auto it = g_active.map.find(key);
    return it == g_active.map.end() ? key : it->second.c_str();
}

} // namespace mp
