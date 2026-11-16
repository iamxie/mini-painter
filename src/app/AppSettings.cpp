#include "app/AppSettings.h"

#include "app/resource.h"

namespace mp {

namespace {

// 啟動時設定檔讀不到：重試幾次、每次隔多久（最多多等 1 秒）。
constexpr int kReadRetries = 10;
constexpr DWORD kReadRetryMs = 100;

// 內建在 exe 裡的繁中語系（RCDATA，UTF-8）。沒有、或內容不對回空的表。
LangTable builtinChinese(HMODULE module) {
    HRSRC res = FindResourceW(module, MAKEINTRESOURCEW(IDR_LANG_ZHTW), RT_RCDATA);
    HGLOBAL g = res ? LoadResource(module, res) : nullptr;
    const char* bytes = g ? static_cast<const char*>(LockResource(g)) : nullptr;
    const DWORD n = res ? SizeofResource(module, res) : 0;
    if (!bytes || !n) return {};
    const int wn = MultiByteToWideChar(CP_UTF8, 0, bytes, static_cast<int>(n), nullptr, 0);
    std::wstring text(wn, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, bytes, static_cast<int>(n), text.data(), wn);
    if (!text.empty() && text[0] == 0xFEFF) text.erase(0, 1);
    return parseLangText(text);
}

} // namespace

std::wstring AppSettings::exeDir() const {
    const size_t slash = exePath_.find_last_of(L"\\/");
    return slash == std::wstring::npos ? L"" : exePath_.substr(0, slash + 1);
}

void AppSettings::load(const std::wstring& exePath, HMODULE module) {
    exePath_ = exePath;
    path_ = settingsPathForExe(exePath_);
    SettingsLoad loaded = loadSettings(path_, values);
    // 設定檔在但讀不到（別的程式正獨佔它）：多半很快就放開，等一下再試。
    for (int i = 0; i < kReadRetries && loaded == SettingsLoad::Unreadable; ++i) {
        Sleep(kReadRetryMs);
        loaded = loadSettings(path_, values);
    }
    unreadable_ = loaded == SettingsLoad::Unreadable;
    base_ = values;
    refreshWritable();
    initLanguage(module);
}

void AppSettings::refreshWritable() { writable_ = isPathWritable(path_); }

bool AppSettings::save() {
    saveError_ = 0;
    if (unreadable_) {
        writable_ = false;
        return false;
    }
    // 寫不進去（唯讀位置）時本次執行內的狀態還在；要不要告訴使用者由呼叫端決定（saveError 是原因）。
    Settings written;
    writable_ = saveSettingsMerged(path_, base_, values, written, &saveError_);
    if (!writable_) return false;
    // 別的視窗記下的東西裡，這幾樣不影響這個視窗正在顯示的畫面，直接收下：新學到的密碼馬上能用，
    // 問過的事不再問一次。其他欄位（檢視值、模式、置頂、暫存位置…）這個視窗繼續用自己的。
    values.passwords = written.passwords;
    values.askedDefault = written.askedDefault;
    values.tipShown = written.tipShown;
    values.skipVersion = written.skipVersion;
    base_ = values;
    return true;
}

void AppSettings::initLanguage(HMODULE module) {
    // 內建：英文（key 本身）與繁中（RCDATA）。lang\ 底下同代碼的檔覆蓋內建，其他的加進清單。
    langs_.clear();
    LangTable en;
    en.code = L"en";
    en.name = L"English";
    langs_.push_back(en);
    if (LangTable zh = builtinChinese(module); !zh.code.empty()) langs_.push_back(std::move(zh));
    for (auto& t : loadLangDir(exeDir() + L"lang")) {
        bool replaced = false;
        for (auto& b : langs_) {
            if (_wcsicmp(b.code.c_str(), t.code.c_str()) == 0) { b = t; replaced = true; break; }
        }
        if (!replaced) langs_.push_back(std::move(t));
    }
    std::vector<std::wstring> codes;
    for (const auto& t : langs_) codes.push_back(t.code);
    wchar_t locale[LOCALE_NAME_MAX_LENGTH] = L"";
    LCIDToLocaleName(MAKELCID(GetUserDefaultUILanguage(), SORT_DEFAULT), locale, LOCALE_NAME_MAX_LENGTH, 0);
    const std::wstring code = pickLanguage(values.language, locale, codes);
    for (const auto& t : langs_) {
        if (_wcsicmp(t.code.c_str(), code.c_str()) == 0) { setActiveLang(t); break; }
    }
}

} // namespace mp
