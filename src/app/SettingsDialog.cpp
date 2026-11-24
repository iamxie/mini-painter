#include "app/SettingsDialog.h"

#include <commctrl.h>
#include <shobjidl.h>
#include <windowsx.h>

#include <algorithm>
#include <iterator>
#include <string>

#include "app/DialogUtil.h"
#include "app/UiText.h"
#include "app/resource.h"
#include "core/Lang.h"
#include "core/PathUtil.h"
#include "core/TempDir.h"
#include "core/TextUtil.h"
#include "core/ThumbCache.h"

namespace mp {

namespace {

// ---------------------------------------------------------------------------
// 設定：PropertySheet，五個 tab（一般 / 模式 / 壓縮檔 / Windows 整合 / Plugin），底部 取消 / 確定 / 套用。
// 每個 tab 一張英文對話框範本，WM_INITDIALOG 時翻譯。按「套用」或「確定」時每個已建立的頁面收到 PSN_APPLY，
// 各自把控制項讀回共用的 Settings 副本；最後一頁收完才呼叫 info.onApply（沒打開過的頁面沒建立，值維持原樣）。

// 選資料夾（IFileOpenDialog 的挑資料夾模式）。取消回傳空字串。
std::wstring browseFolder(HWND owner, const std::wstring& initial) {
    std::wstring out;
    IFileOpenDialog* fd = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&fd)))) return out;
    DWORD opts = 0;
    fd->GetOptions(&opts);
    fd->SetOptions(opts | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
    if (!initial.empty()) {
        IShellItem* si = nullptr;
        if (SUCCEEDED(SHCreateItemFromParsingName(initial.c_str(), nullptr, IID_PPV_ARGS(&si)))) {
            fd->SetFolder(si);
            si->Release();
        }
    }
    if (SUCCEEDED(fd->Show(owner))) {
        IShellItem* item = nullptr;
        if (SUCCEEDED(fd->GetResult(&item))) {
            PWSTR p = nullptr;
            if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &p))) {
                out = p;
                CoTaskMemFree(p);
            }
            item->Release();
        }
    }
    fd->Release();
    return out;
}

// 多行編輯框 <-> 清單：一行一項，空行略過。trimSpaces 為 true 時前後空白去掉（密碼框不去：空白是密碼的一部分）。
std::wstring joinLines(const std::vector<std::wstring>& items) {
    std::wstring out;
    for (const auto& it : items) out += (out.empty() ? L"" : L"\r\n") + it;
    return out;
}

std::vector<std::wstring> readLines(HWND dlg, int id, bool trimSpaces) {
    HWND h = GetDlgItem(dlg, id);
    int len = GetWindowTextLengthW(h);
    std::wstring text(static_cast<size_t>(len) + 1, L'\0');
    GetWindowTextW(h, text.data(), len + 1);
    text.resize(static_cast<size_t>(len));
    std::vector<std::wstring> out;
    size_t pos = 0;
    while (pos <= text.size()) {
        size_t nl = text.find(L'\n', pos);
        std::wstring line = text.substr(pos, nl == std::wstring::npos ? std::wstring::npos : nl - pos);
        pos = nl == std::wstring::npos ? text.size() + 1 : nl + 1;
        const wchar_t* ws = trimSpaces ? L" \t\r" : L"\r";
        size_t a = line.find_first_not_of(ws), b = line.find_last_not_of(ws);
        if (a == std::wstring::npos) continue;
        out.push_back(line.substr(a, b - a + 1));
    }
    return out;
}

std::wstring dlgText(HWND dlg, int id) {
    HWND h = GetDlgItem(dlg, id);
    int len = GetWindowTextLengthW(h);
    std::wstring text(static_cast<size_t>(len) + 1, L'\0');
    GetWindowTextW(h, text.data(), len + 1);
    text.resize(static_cast<size_t>(len));
    return text;
}

struct SheetState {
    Settings s;                       // 工作副本：套用時整份交給 onApply
    const SettingsDialogInfo* info = nullptr;
    int created = 0;                  // 已建立的頁面數（PSN_APPLY 會送給每一頁，收齊才 onApply）
    int applied = 0;
    int selProfile = -1;              // 模式頁：清單目前選的那個（s.profiles 的索引）
    bool loadingProfile = false;      // 模式頁：正在把模式填進控制項（忽略這期間的變更通知）
    bool generalReady = false;        // 一般頁：控制項填好了（之前的 EN_CHANGE 不算使用者改的）
    bool loadingPlugins = false;      // Plugin 頁：程式自己在改勾選框（不檢查、不算使用者改的）
};

void markChanged(HWND page) { PropSheet_Changed(GetParent(page), page); }

// 每一頁的 PSN_APPLY 走這裡：收齊所有已建立的頁面後才真的套用。
// 一輪套用從目前這一頁的 PSN_KILLACTIVE 開始（見各頁的 WM_NOTIFY），那時計數歸零：
// 上一輪如果有頁面拒絕、沒收齊，數到一半的計數不會留到這一輪。
void pageApplied(SheetState& st) {
    if (++st.applied < st.created) return;
    st.applied = 0;
    if (st.info->onApply) st.info->onApply(st.s);
}

// ----- 一般 -----

// 數字欄位：控制項 id、core 的欄位定義（ini key、成員、範圍）、最多打幾位數。加一個欄位：core 的 kIntSettings 加一列、
// 這裡（放在哪一頁就加在那一頁的表）加一列、app.rc 加一個編輯框。
struct IntField {
    int id;
    int Settings::*member;
    int digits;
};
const IntField kIntFields[] = {
    {IDC_UNDOLIMIT, &Settings::undoLimit, 3},
    {IDC_UNDOMEM, &Settings::undoMemoryMB, 5},
    {IDC_PREFETCHMEM, &Settings::prefetchMB, 5},
};
// 「壓縮檔」頁的數字欄位。
const IntField kArchiveIntFields[] = {
    {IDC_THUMBCACHE, &Settings::thumbCacheMB, 5},
};
static_assert(std::size(kIntFields) + std::size(kArchiveIntFields) == std::size(kIntSettings),
              "every numeric setting needs an edit box on the General or Archives page");

// 這個欄位在 core 的定義（範圍）。
const IntSetting& settingOf(const IntField& f) {
    for (const IntSetting& s : kIntSettings)
        if (s.member == f.member) return s;
    return kIntSettings[0]; // 走不到：上面的 static_assert 加上每一列都對得到
}

void showIntFields(HWND dlg, const Settings& s) {
    for (const IntField& f : kIntFields) SetDlgItemInt(dlg, f.id, static_cast<UINT>(s.*f.member), FALSE);
}

void updateTipState(HWND dlg, const SheetState& st) {
    wchar_t buf[256];
    if (st.s.tipShown >= kTipMaxShown) swprintf(buf, 256, tr(L"Shown %d times; it will not appear again."), kTipMaxShown);
    else swprintf(buf, 256, tr(L"Hint shown when the mouse rests on an annotation. Will be shown %d more times."), kTipMaxShown - st.s.tipShown);
    SetDlgItemTextW(dlg, IDC_TIPSTATE, buf);
}

INT_PTR CALLBACK generalPageProc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp) {
    auto* st = reinterpret_cast<SheetState*>(GetWindowLongPtrW(dlg, DWLP_USER));
    switch (msg) {
    case WM_INITDIALOG: {
        st = reinterpret_cast<SheetState*>(reinterpret_cast<PROPSHEETPAGEW*>(lp)->lParam);
        SetWindowLongPtrW(dlg, DWLP_USER, reinterpret_cast<LONG_PTR>(st));
        ++st->created;
        translateDialog(dlg);
        HWND combo = GetDlgItem(dlg, IDC_LANGUAGE);
        SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(tr(L"Follow System")));
        int sel = 0;
        for (size_t i = 0; i < st->info->languages.size(); ++i) {
            SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(st->info->languages[i].second.c_str()));
            if (!st->s.language.empty() && _wcsicmp(st->info->languages[i].first.c_str(), st->s.language.c_str()) == 0)
                sel = static_cast<int>(i) + 1;
        }
        SendMessageW(combo, CB_SETCURSEL, sel, 0);
        SetDlgItemTextW(dlg, IDC_INIPATH, fmt1(L"Settings file: %s", st->info->iniPath).c_str());
        SetDlgItemTextW(dlg, IDC_INIWARN, st->info->iniWritable ? L"" : tr(L"⚠ The settings file cannot be written. Changes here last only for this run; the next start returns to defaults."));
        updateTipState(dlg, *st);
        CheckDlgButton(dlg, IDC_AUTOUPDATE, st->s.autoCheckUpdate ? BST_CHECKED : BST_UNCHECKED);
        for (const IntField& f : kIntFields) SendDlgItemMessageW(dlg, f.id, EM_SETLIMITTEXT, static_cast<WPARAM>(f.digits), 0);
        showIntFields(dlg, st->s); // 會送 EN_CHANGE，此時頁面還沒顯示，不算變更
        st->generalReady = true;
        return TRUE;
    }
    case WM_COMMAND:
        if (LOWORD(wp) == IDC_AUTOUPDATE && HIWORD(wp) == BN_CLICKED) { markChanged(dlg); return TRUE; }
        if (HIWORD(wp) == EN_CHANGE && st && st->generalReady &&
            std::any_of(std::begin(kIntFields), std::end(kIntFields), [&](const IntField& f) { return f.id == LOWORD(wp); })) {
            markChanged(dlg);
            return TRUE;
        }
        if (LOWORD(wp) == IDC_RESETTIP) {
            st->s.tipShown = 0;
            updateTipState(dlg, *st);
            markChanged(dlg);
            return TRUE;
        }
        if (LOWORD(wp) == IDC_LANGUAGE && HIWORD(wp) == CBN_SELCHANGE) { markChanged(dlg); return TRUE; }
        break;
    case WM_NOTIFY:
        if (reinterpret_cast<NMHDR*>(lp)->code == PSN_KILLACTIVE) st->applied = 0;
        if (reinterpret_cast<NMHDR*>(lp)->code == PSN_APPLY) {
            int sel = static_cast<int>(SendDlgItemMessageW(dlg, IDC_LANGUAGE, CB_GETCURSEL, 0, 0));
            st->s.language = sel > 0 && static_cast<size_t>(sel - 1) < st->info->languages.size() ? st->info->languages[static_cast<size_t>(sel - 1)].first : L"";
            st->s.autoCheckUpdate = IsDlgButtonChecked(dlg, IDC_AUTOUPDATE) == BST_CHECKED;
            // 空白或超出範圍：夾回允許的範圍，並把夾過的值寫回欄位
            for (const IntField& f : kIntFields) {
                int& value = st->s.*f.member;
                value = intSettingFromText(settingOf(f), dlgText(dlg, f.id), value);
            }
            st->generalReady = false;
            showIntFields(dlg, st->s);
            st->generalReady = true;
            pageApplied(*st);
            SetWindowLongPtrW(dlg, DWLP_MSGRESULT, PSNRET_NOERROR);
            return TRUE;
        }
        break;
    }
    return FALSE;
}

// ----- 模式 -----

// 模式頁的勾選框：控制項 id 與它對應的檢視開關。加一個開關：core 的 MP_VIEW_FLAGS 加一行、這裡加一列、app.rc 加一個勾選框。
struct FlagCheck {
    int id;
    bool ViewPrefs::*member;
};
const FlagCheck kFlagChecks[] = {
    {IDC_PROF_UPSCALE, &ViewPrefs::upscaleSmall},
    {IDC_PROF_HQ, &ViewPrefs::highQuality},
    {IDC_PROF_SCROLLFLIP, &ViewPrefs::scrollFlip},
    {IDC_PROF_AUTONEXT, &ViewPrefs::autoNextBook},
    {IDC_PROF_RTL, &ViewPrefs::rightToLeft},
    {IDC_PROF_FIRSTALONE, &ViewPrefs::firstPageAlone},
    {IDC_PROF_WARNMETA, &ViewPrefs::warnMetadataLoss},
};
static_assert(std::size(kFlagChecks) == std::size(kViewFlags), "every view flag needs a check box on the Modes page");

// 名字對話框（新增 / 複製 / 重新命名）。回傳 false 表示取消。
struct NameState { std::wstring caption, value; };

INT_PTR CALLBACK nameProc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp) {
    auto* st = reinterpret_cast<NameState*>(GetWindowLongPtrW(dlg, DWLP_USER));
    switch (msg) {
    case WM_INITDIALOG:
        st = reinterpret_cast<NameState*>(lp);
        SetWindowLongPtrW(dlg, DWLP_USER, lp);
        translateDialog(dlg);
        SetWindowTextW(dlg, st->caption.c_str());
        SendDlgItemMessageW(dlg, IDC_NAME_EDIT, EM_SETLIMITTEXT, 64, 0);
        SetDlgItemTextW(dlg, IDC_NAME_EDIT, st->value.c_str());
        SendDlgItemMessageW(dlg, IDC_NAME_EDIT, EM_SETSEL, 0, -1);
        SetFocus(GetDlgItem(dlg, IDC_NAME_EDIT));
        return FALSE;
    case WM_COMMAND:
        if (LOWORD(wp) == IDOK) {
            st->value = trim(dlgText(dlg, IDC_NAME_EDIT));
            if (st->value.empty()) return TRUE; // 空名字不收
            EndDialog(dlg, IDOK);
            return TRUE;
        }
        if (LOWORD(wp) == IDCANCEL) { EndDialog(dlg, IDCANCEL); return TRUE; }
        break;
    }
    return FALSE;
}

bool askName(HWND owner, const wchar_t* caption, std::wstring& value) {
    NameState st;
    st.caption = caption;
    st.value = value;
    HINSTANCE inst = reinterpret_cast<HINSTANCE>(GetWindowLongPtrW(owner, GWLP_HINSTANCE));
    if (DialogBoxParamW(inst, MAKEINTRESOURCEW(IDD_NAME), owner, nameProc, reinterpret_cast<LPARAM>(&st)) != IDOK) return false;
    value = st.value;
    return true;
}

// 問一個不重複的名字。exceptIndex 是重新命名時自己的索引（和自己同名不算重複）。
bool askUniqueName(HWND page, SheetState& st, const wchar_t* caption, std::wstring& name, int exceptIndex) {
    for (;;) {
        if (!askName(page, caption, name)) return false;
        bool dup = false;
        for (size_t i = 0; i < st.s.profiles.size(); ++i)
            if (static_cast<int>(i) != exceptIndex && _wcsicmp(st.s.profiles[i].name.c_str(), name.c_str()) == 0) dup = true;
        if (!dup) return true;
        showMessage(page, tr(L"A mode with this name already exists."), TD_INFORMATION_ICON);
    }
}

void fillProfileList(HWND dlg, SheetState& st) {
    HWND list = GetDlgItem(dlg, IDC_PROFILES);
    SendMessageW(list, LB_RESETCONTENT, 0, 0);
    for (const auto& p : st.s.profiles) SendMessageW(list, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(p.name.c_str()));
    if (st.selProfile < 0 || st.selProfile >= static_cast<int>(st.s.profiles.size())) st.selProfile = 0;
    SendMessageW(list, LB_SETCURSEL, st.selProfile, 0);
    SetDlgItemTextW(dlg, IDC_PROF_INUSE, fmt1(L"In use: %s", st.s.profileName).c_str());
}

void loadProfileControls(HWND dlg, SheetState& st) {
    if (st.selProfile < 0 || st.selProfile >= static_cast<int>(st.s.profiles.size())) return;
    const Profile& p = st.s.profiles[static_cast<size_t>(st.selProfile)];
    st.loadingProfile = true;
    SetDlgItemTextW(dlg, IDC_PROF_GRP, p.name.c_str());
    SendDlgItemMessageW(dlg, IDC_PROF_VIEWMODE, CB_SETCURSEL, static_cast<WPARAM>(p.prefs.viewMode), 0); // 下拉的順序就是 enum 的順序
    for (const FlagCheck& c : kFlagChecks) CheckDlgButton(dlg, c.id, p.prefs.*c.member ? BST_CHECKED : BST_UNCHECKED);
    bool inUse = _wcsicmp(p.name.c_str(), st.s.profileName.c_str()) == 0;
    EnableWindow(GetDlgItem(dlg, IDC_PROF_USE), !inUse);
    EnableWindow(GetDlgItem(dlg, IDC_PROF_DELETE), !inUse && st.s.profiles.size() > 1);
    st.loadingProfile = false;
}

void storeProfileControls(HWND dlg, SheetState& st) {
    if (st.selProfile < 0 || st.selProfile >= static_cast<int>(st.s.profiles.size())) return;
    Profile& p = st.s.profiles[static_cast<size_t>(st.selProfile)];
    int vm = static_cast<int>(SendDlgItemMessageW(dlg, IDC_PROF_VIEWMODE, CB_GETCURSEL, 0, 0));
    if (vm >= 0 && vm < kViewModeCount) p.prefs.viewMode = static_cast<ViewMode>(vm);
    for (const FlagCheck& c : kFlagChecks) p.prefs.*c.member = IsDlgButtonChecked(dlg, c.id) == BST_CHECKED;
}

INT_PTR CALLBACK modesPageProc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp) {
    auto* st = reinterpret_cast<SheetState*>(GetWindowLongPtrW(dlg, DWLP_USER));
    switch (msg) {
    case WM_INITDIALOG: {
        st = reinterpret_cast<SheetState*>(reinterpret_cast<PROPSHEETPAGEW*>(lp)->lParam);
        SetWindowLongPtrW(dlg, DWLP_USER, reinterpret_cast<LONG_PTR>(st));
        ++st->created;
        translateDialog(dlg);
        for (int i = 0; i < kViewModeCount; ++i)
            SendDlgItemMessageW(dlg, IDC_PROF_VIEWMODE, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(tr(viewModeName(static_cast<ViewMode>(i)))));
        // 一開始選到正在使用的那個
        for (size_t i = 0; i < st->s.profiles.size(); ++i)
            if (_wcsicmp(st->s.profiles[i].name.c_str(), st->s.profileName.c_str()) == 0) st->selProfile = static_cast<int>(i);
        fillProfileList(dlg, *st);
        loadProfileControls(dlg, *st);
        return TRUE;
    }
    case WM_COMMAND: {
        int id = LOWORD(wp), code = HIWORD(wp);
        if (id == IDC_PROFILES && code == LBN_SELCHANGE) {
            storeProfileControls(dlg, *st);
            st->selProfile = static_cast<int>(SendDlgItemMessageW(dlg, IDC_PROFILES, LB_GETCURSEL, 0, 0));
            loadProfileControls(dlg, *st);
            return TRUE;
        }
        if (id == IDC_PROF_ADD || id == IDC_PROF_DUP) {
            storeProfileControls(dlg, *st);
            std::wstring name;
            if (!askUniqueName(dlg, *st, id == IDC_PROF_ADD ? tr(L"New Mode") : tr(L"Copy Mode"), name, -1)) return TRUE;
            Profile p = id == IDC_PROF_DUP && st->selProfile >= 0 ? st->s.profiles[static_cast<size_t>(st->selProfile)] : Profile{};
            p.name = name;
            st->s.profiles.push_back(p);
            st->selProfile = static_cast<int>(st->s.profiles.size()) - 1;
            fillProfileList(dlg, *st);
            loadProfileControls(dlg, *st);
            markChanged(dlg);
            return TRUE;
        }
        if (id == IDC_PROF_RENAME && st->selProfile >= 0) {
            storeProfileControls(dlg, *st);
            Profile& p = st->s.profiles[static_cast<size_t>(st->selProfile)];
            std::wstring name = p.name;
            if (!askUniqueName(dlg, *st, tr(L"Rename Mode"), name, st->selProfile)) return TRUE;
            if (_wcsicmp(p.name.c_str(), st->s.profileName.c_str()) == 0) st->s.profileName = name;
            p.name = name;
            fillProfileList(dlg, *st);
            loadProfileControls(dlg, *st);
            markChanged(dlg);
            return TRUE;
        }
        if (id == IDC_PROF_DELETE && st->selProfile >= 0) {
            const Profile& p = st->s.profiles[static_cast<size_t>(st->selProfile)];
            if (_wcsicmp(p.name.c_str(), st->s.profileName.c_str()) == 0) {
                showMessage(dlg, tr(L"This mode is in use. Switch to another mode first."), TD_INFORMATION_ICON);
                return TRUE;
            }
            if (st->s.profiles.size() <= 1) {
                showMessage(dlg, tr(L"At least one mode must remain."), TD_INFORMATION_ICON);
                return TRUE;
            }
            st->s.profiles.erase(st->s.profiles.begin() + st->selProfile);
            if (st->selProfile >= static_cast<int>(st->s.profiles.size())) st->selProfile = static_cast<int>(st->s.profiles.size()) - 1;
            fillProfileList(dlg, *st);
            loadProfileControls(dlg, *st);
            markChanged(dlg);
            return TRUE;
        }
        if (id == IDC_PROF_USE && st->selProfile >= 0) {
            storeProfileControls(dlg, *st);
            st->s.profileName = st->s.profiles[static_cast<size_t>(st->selProfile)].name;
            fillProfileList(dlg, *st);
            loadProfileControls(dlg, *st);
            markChanged(dlg);
            return TRUE;
        }
        bool pref = std::any_of(std::begin(kFlagChecks), std::end(kFlagChecks), [&](const FlagCheck& c) { return c.id == id; });
        if ((pref && code == BN_CLICKED) || (id == IDC_PROF_VIEWMODE && code == CBN_SELCHANGE)) {
            if (!st->loadingProfile) markChanged(dlg);
            return TRUE;
        }
        break;
    }
    case WM_NOTIFY:
        if (reinterpret_cast<NMHDR*>(lp)->code == PSN_KILLACTIVE) st->applied = 0;
        if (reinterpret_cast<NMHDR*>(lp)->code == PSN_APPLY) {
            storeProfileControls(dlg, *st);
            pageApplied(*st);
            SetWindowLongPtrW(dlg, DWLP_MSGRESULT, PSNRET_NOERROR);
            return TRUE;
        }
        break;
    }
    return FALSE;
}

// ----- 壓縮檔 -----

// 暫存位置的用量：解出來的壓縮檔（不含縮圖快取）與縮圖快取分開寫。
void updateUnzipState(HWND dlg, const std::wstring& root, const std::wstring& thumbPath) {
    const unsigned long long thumbs = thumbCacheFileBytes(thumbPath);
    const unsigned long long all = directorySize(root);
    const bool thumbsInRoot = _wcsicmp(thumbPath.c_str(), joinPath(root, kThumbCacheFileName).c_str()) == 0;
    SetDlgItemTextW(dlg, IDC_UNZIPSTATE, fmt2(L"In use: %s (%s)", root, fileSizeText(thumbsInRoot && all >= thumbs ? all - thumbs : all)).c_str());
    SetDlgItemTextW(dlg, IDC_THUMBSTATE, fmt1(L"Thumbnails stored now: %s", fileSizeText(thumbs)).c_str());
}

INT_PTR CALLBACK archivePageProc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp) {
    auto* st = reinterpret_cast<SheetState*>(GetWindowLongPtrW(dlg, DWLP_USER));
    switch (msg) {
    case WM_INITDIALOG: {
        st = reinterpret_cast<SheetState*>(reinterpret_cast<PROPSHEETPAGEW*>(lp)->lParam);
        SetWindowLongPtrW(dlg, DWLP_USER, reinterpret_cast<LONG_PTR>(st));
        ++st->created;
        translateDialog(dlg);
        SetDlgItemTextW(dlg, IDC_UNZIPDIR, st->s.unzipDir.c_str());
        SetDlgItemTextW(dlg, IDC_PINNED, joinLines(st->s.pinnedNames).c_str());
        SetDlgItemTextW(dlg, IDC_PASSWORDS, joinLines(st->s.passwords).c_str());
        for (const IntField& f : kArchiveIntFields) {
            SendDlgItemMessageW(dlg, f.id, EM_SETLIMITTEXT, static_cast<WPARAM>(f.digits), 0);
            SetDlgItemInt(dlg, f.id, static_cast<UINT>(st->s.*f.member), FALSE);
        }
        updateUnzipState(dlg, st->info->unzipRootInUse, st->info->thumbCachePath);
        return TRUE;
    }
    case WM_COMMAND: {
        int id = LOWORD(wp), code = HIWORD(wp);
        if (id == IDC_UNZIPBROWSE) {
            std::wstring picked = browseFolder(dlg, dlgText(dlg, IDC_UNZIPDIR));
            if (!picked.empty()) SetDlgItemTextW(dlg, IDC_UNZIPDIR, picked.c_str()); // EN_CHANGE 會標記變更
            return TRUE;
        }
        if ((id == IDC_UNZIPDIR || id == IDC_PINNED || id == IDC_PASSWORDS || id == IDC_THUMBCACHE) && code == EN_CHANGE) { markChanged(dlg); return TRUE; }
        if (id == IDC_THUMBCLEAR) {
            if (st->info->onClearThumbs) st->info->onClearThumbs();
            updateUnzipState(dlg, st->info->unzipRootInUse, st->info->thumbCachePath);
            return TRUE;
        }
        break;
    }
    case WM_NOTIFY:
        if (reinterpret_cast<NMHDR*>(lp)->code == PSN_KILLACTIVE) st->applied = 0;
        if (reinterpret_cast<NMHDR*>(lp)->code == PSN_APPLY) {
            st->s.unzipDir = trim(dlgText(dlg, IDC_UNZIPDIR));
            st->s.pinnedNames = readLines(dlg, IDC_PINNED, true);
            st->s.passwords = readLines(dlg, IDC_PASSWORDS, false);
            for (const IntField& f : kArchiveIntFields) {
                int& value = st->s.*f.member;
                value = intSettingFromText(settingOf(f), dlgText(dlg, f.id), value);
                SetDlgItemInt(dlg, f.id, static_cast<UINT>(value), FALSE);
            }
            pageApplied(*st);
            const std::wstring root = resolveUnzipRoot(st->s.unzipDir, st->info->exeDir);
            updateUnzipState(dlg, root, joinPath(root, kThumbCacheFileName));
            SetWindowLongPtrW(dlg, DWLP_MSGRESULT, PSNRET_NOERROR);
            return TRUE;
        }
        break;
    }
    return FALSE;
}

// ----- Windows 整合 -----

INT_PTR CALLBACK windowsPageProc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp) {
    auto* st = reinterpret_cast<SheetState*>(GetWindowLongPtrW(dlg, DWLP_USER));
    switch (msg) {
    case WM_INITDIALOG: {
        st = reinterpret_cast<SheetState*>(reinterpret_cast<PROPSHEETPAGEW*>(lp)->lParam);
        SetWindowLongPtrW(dlg, DWLP_USER, reinterpret_cast<LONG_PTR>(st));
        ++st->created;
        translateDialog(dlg);
        CheckDlgButton(dlg, IDC_CTXMENU, st->s.contextMenu ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(dlg, IDC_ASKDEFAULT, st->s.askedDefault ? BST_UNCHECKED : BST_CHECKED);
        SetDlgItemTextW(dlg, IDC_CTXNOTE, st->info->isWin11
            ? tr(L"Windows 11: this item is under “Show more options” in the context menu (or press Shift+F10).")
            : L"");
        SetDlgItemTextW(dlg, IDC_DEFNOTE, st->info->isWin11
            ? tr(L"Windows does not let apps set defaults themselves. Opens the system Settings page; on Windows 11 pick each file type one by one.")
            : tr(L"Windows does not let apps set defaults themselves. Opens the system Settings page; choose mpainter there."));
        return TRUE;
    }
    case WM_COMMAND: {
        int id = LOWORD(wp), code = HIWORD(wp);
        if (id == IDC_SETDEFAULT) {
            if (st->info->onSetDefault) st->info->onSetDefault();
            return TRUE;
        }
        if ((id == IDC_CTXMENU || id == IDC_ASKDEFAULT) && code == BN_CLICKED) { markChanged(dlg); return TRUE; }
        break;
    }
    case WM_NOTIFY:
        if (reinterpret_cast<NMHDR*>(lp)->code == PSN_KILLACTIVE) st->applied = 0;
        if (reinterpret_cast<NMHDR*>(lp)->code == PSN_APPLY) {
            st->s.contextMenu = IsDlgButtonChecked(dlg, IDC_CTXMENU) == BST_CHECKED;
            st->s.askedDefault = IsDlgButtonChecked(dlg, IDC_ASKDEFAULT) != BST_CHECKED;
            pageApplied(*st);
            SetWindowLongPtrW(dlg, DWLP_MSGRESULT, PSNRET_NOERROR);
            return TRUE;
        }
        break;
    }
    return FALSE;
}

// ----- Plugin -----
// 清單的列與 info->plugins 同順序（不排序），第 i 列就是 plugins[i]。勾選框就是啟用與否；套用時才寫進 st.s。

const PluginLoadResult* loadResultOf(const SheetState& st, const std::wstring& id) {
    for (const auto& r : st.info->pluginResults)
        if (_wcsicmp(r.id.c_str(), id.c_str()) == 0) return &r;
    return nullptr;
}

std::wstring joinExtensions(const std::vector<std::wstring>& exts, const wchar_t* sep) {
    std::wstring out;
    for (const auto& e : exts) out += (out.empty() ? L"" : sep) + e;
    return out;
}

bool rowChecked(HWND list, size_t row) { return ListView_GetCheckState(list, static_cast<int>(row)) != FALSE; }

void updatePluginStatus(HWND list, const SheetState& st, size_t row) {
    const PluginEntry& e = st.info->plugins[row];
    std::wstring text = pluginStatusText(e, rowChecked(list, row), findPluginSetting(st.s, e.id), loadResultOf(st, e.id));
    ListView_SetItemText(list, static_cast<int>(row), 2, text.data());
}

void showPluginDetail(HWND dlg, const SheetState& st) {
    const auto& plugins = st.info->plugins;
    const int row = ListView_GetNextItem(GetDlgItem(dlg, IDC_PLUGINS), -1, LVNI_SELECTED);
    std::wstring text;
    if (plugins.empty()) {
        text = tr(L"No plugins are installed.");
    } else if (row >= 0 && static_cast<size_t>(row) < plugins.size()) {
        const PluginEntry& e = plugins[static_cast<size_t>(row)];
        if (e.manifest.ok()) text = fmt1(L"Version: %s", e.manifest.manifest.version) + L"\n";
        text += fmt1(L"File: %s", e.dllPath);
        if (!e.builtInOverlap.empty())
            text += L"\n" + fmt1(L"Opened by mpainter itself, not by this plugin: %s", joinExtensions(e.builtInOverlap, L", "));
        if (std::wstring problem = pluginProblemText(e, loadResultOf(st, e.id)); !problem.empty()) text += L"\n" + problem;
    }
    SetDlgItemTextW(dlg, IDC_PLUGDETAIL, text.c_str());
}

// 使用者要勾第 row 列。用不了的不給勾；和已勾的衝突時問要不要停用那些、改用這一個。回傳要不要讓它勾上。
bool allowEnablePlugin(HWND dlg, SheetState& st, size_t row) {
    HWND list = GetDlgItem(dlg, IDC_PLUGINS);
    const auto& plugins = st.info->plugins;
    const PluginEntry& e = plugins[row];
    if (pluginStaticCheck(e) != PluginLoadError::None) {
        showMessage(dlg, pluginProblemText(e, nullptr), TD_WARNING_ICON);
        return false;
    }
    std::vector<std::wstring> checked;
    for (size_t i = 0; i < plugins.size(); ++i)
        if (i != row && rowChecked(list, i)) checked.push_back(plugins[i].id);
    const auto conflicts = pluginConflictsIfEnabled(plugins, checked, e.id);
    if (conflicts.empty()) return true;
    std::vector<std::wstring> others, exts;
    for (const auto& c : conflicts) {
        exts.push_back(c.extension);
        for (const auto& id : c.ids)
            if (_wcsicmp(id.c_str(), e.id.c_str()) != 0 && std::find(others.begin(), others.end(), id) == others.end()) others.push_back(id);
    }
    std::vector<std::wstring> names;
    for (const auto& id : others) names.push_back(pluginDisplayName(plugins, id));
    if (!askOk(dlg,
               fmt2(L"Enable this plugin and disable %s? They want to open the same %s files and cannot be enabled together.",
                    quotedNameList(names), joinExtensions(exts, L"/")),
               TD_WARNING_ICON, true))
        return false;
    st.loadingPlugins = true; // 取消別的勾：不再問一次
    for (size_t i = 0; i < plugins.size(); ++i)
        if (std::find(others.begin(), others.end(), plugins[i].id) != others.end()) ListView_SetCheckState(list, static_cast<int>(i), FALSE);
    st.loadingPlugins = false;
    return true;
}

INT_PTR CALLBACK pluginsPageProc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp) {
    (void)wp;
    auto* st = reinterpret_cast<SheetState*>(GetWindowLongPtrW(dlg, DWLP_USER));
    switch (msg) {
    case WM_INITDIALOG: {
        st = reinterpret_cast<SheetState*>(reinterpret_cast<PROPSHEETPAGEW*>(lp)->lParam);
        SetWindowLongPtrW(dlg, DWLP_USER, reinterpret_cast<LONG_PTR>(st));
        ++st->created;
        translateDialog(dlg);
        HWND list = GetDlgItem(dlg, IDC_PLUGINS);
        ListView_SetExtendedListViewStyle(list, LVS_EX_CHECKBOXES | LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
        RECT rc{};
        GetClientRect(list, &rc);
        const int width = rc.right - GetSystemMetrics(SM_CXVSCROLL);
        struct Column {
            const wchar_t* key;
            int percent;
        };
        const Column columns[] = {{trKey(L"Name"), 38}, {trKey(L"Extensions"), 24}, {trKey(L"Status"), 38}};
        for (int i = 0; i < 3; ++i) {
            LVCOLUMNW c{};
            c.mask = LVCF_TEXT | LVCF_WIDTH;
            c.pszText = const_cast<wchar_t*>(tr(columns[i].key));
            c.cx = width * columns[i].percent / 100;
            ListView_InsertColumn(list, i, &c);
        }
        const auto& plugins = st->info->plugins;
        st->loadingPlugins = true;
        for (size_t i = 0; i < plugins.size(); ++i) {
            const PluginEntry& e = plugins[i];
            std::wstring name = pluginDisplayName(plugins, e.id), exts = joinExtensions(pluginEffectiveExtensions(e), L", ");
            LVITEMW item{};
            item.mask = LVIF_TEXT;
            item.iItem = static_cast<int>(i);
            item.pszText = name.data();
            ListView_InsertItem(list, &item);
            ListView_SetItemText(list, static_cast<int>(i), 1, exts.data());
            ListView_SetCheckState(list, static_cast<int>(i), pluginEnabled(st->s, e.id));
            updatePluginStatus(list, *st, i);
        }
        if (!plugins.empty()) ListView_SetItemState(list, 0, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
        st->loadingPlugins = false;
        SetDlgItemTextW(dlg, IDC_PLUGDIR, fmt1(L"Plugin folder: %s", st->info->pluginsDir).c_str());
        showPluginDetail(dlg, *st);
        return TRUE;
    }
    case WM_NOTIFY: {
        const auto* hdr = reinterpret_cast<NMHDR*>(lp);
        if (hdr->idFrom == IDC_PLUGINS && (hdr->code == LVN_ITEMCHANGING || hdr->code == LVN_ITEMCHANGED)) {
            const auto* nm = reinterpret_cast<NMLISTVIEW*>(lp);
            if (nm->iItem < 0 || static_cast<size_t>(nm->iItem) >= st->info->plugins.size() || !(nm->uChanged & LVIF_STATE)) break;
            const size_t row = static_cast<size_t>(nm->iItem);
            const UINT diff = nm->uNewState ^ nm->uOldState;
            if (hdr->code == LVN_ITEMCHANGING) {
                // 勾選框要變成打勾（state image 2）：先檢查。回 TRUE 是擋下來。
                const bool checking = (diff & LVIS_STATEIMAGEMASK) && (nm->uNewState & LVIS_STATEIMAGEMASK) == INDEXTOSTATEIMAGEMASK(2);
                const bool block = checking && !st->loadingPlugins && !allowEnablePlugin(dlg, *st, row);
                SetWindowLongPtrW(dlg, DWLP_MSGRESULT, block ? TRUE : FALSE);
                return TRUE;
            }
            if (diff & LVIS_STATEIMAGEMASK) {
                updatePluginStatus(GetDlgItem(dlg, IDC_PLUGINS), *st, row);
                if (!st->loadingPlugins) markChanged(dlg);
            }
            if (diff & LVIS_SELECTED) showPluginDetail(dlg, *st);
            return TRUE;
        }
        if (hdr->code == PSN_KILLACTIVE) st->applied = 0;
        if (hdr->code == PSN_APPLY) {
            HWND list = GetDlgItem(dlg, IDC_PLUGINS);
            const auto& plugins = st->info->plugins;
            // 只寫有變的：沒動過的維持原本的紀錄（含停用的原因：當掉、衝突）。
            for (size_t i = 0; i < plugins.size(); ++i) {
                const bool checked = rowChecked(list, i);
                if (checked != pluginEnabled(st->s, plugins[i].id))
                    setPluginState(st->s, plugins[i].id, checked ? PluginState::Enabled : PluginState::Disabled);
            }
            for (size_t i = 0; i < plugins.size(); ++i) updatePluginStatus(list, *st, i);
            pageApplied(*st);
            SetWindowLongPtrW(dlg, DWLP_MSGRESULT, PSNRET_NOERROR);
            return TRUE;
        }
        break;
    }
    }
    return FALSE;
}

// 底部三顆按鈕的文字跟語系走（PropertySheet 預設是系統語言的 OK / Cancel / Apply）。
int CALLBACK sheetCallback(HWND sheet, UINT msg, LPARAM) {
    if (msg == PSCB_INITIALIZED) {
        SetDlgItemTextW(sheet, IDOK, tr(L"OK"));
        SetDlgItemTextW(sheet, IDCANCEL, tr(L"Cancel"));
        SetDlgItemTextW(sheet, 0x3021 /* ID_APPLY_NOW */, tr(L"Apply"));
    }
    return 0;
}

} // namespace

void showSettingsDialog(HWND owner, const Settings& s, const SettingsDialogInfo& info) {
    SheetState st;
    st.s = s;
    st.info = &info;
    HINSTANCE inst = reinterpret_cast<HINSTANCE>(GetWindowLongPtrW(owner, GWLP_HINSTANCE));
    struct PageDef { int id; DLGPROC proc; const wchar_t* title; };
    const PageDef defs[] = {
        {IDD_SET_GENERAL, generalPageProc, trKey(L"General")},
        {IDD_SET_MODES, modesPageProc, trKey(L"Modes")},
        {IDD_SET_ARCHIVE, archivePageProc, trKey(L"Archives")},
        {IDD_SET_WINDOWS, windowsPageProc, trKey(L"Windows Integration")},
        {IDD_SET_PLUGINS, pluginsPageProc, trKey(L"Plugins")},
    };
    constexpr int kPages = static_cast<int>(std::size(defs));
    PROPSHEETPAGEW pages[kPages] = {};
    for (int i = 0; i < kPages; ++i) {
        pages[i].dwSize = sizeof(PROPSHEETPAGEW);
        pages[i].dwFlags = PSP_USETITLE;
        pages[i].hInstance = inst;
        pages[i].pszTemplate = MAKEINTRESOURCEW(defs[i].id);
        pages[i].pfnDlgProc = defs[i].proc;
        pages[i].pszTitle = tr(defs[i].title);
        pages[i].lParam = reinterpret_cast<LPARAM>(&st);
    }
    PROPSHEETHEADERW hdr = {};
    hdr.dwSize = sizeof(hdr);
    hdr.dwFlags = PSH_PROPSHEETPAGE | PSH_USECALLBACK | PSH_NOCONTEXTHELP;
    hdr.hwndParent = owner;
    hdr.hInstance = inst;
    hdr.pszCaption = tr(L"Settings");
    hdr.nPages = kPages;
    hdr.nStartPage = static_cast<UINT>(std::clamp(info.startPage, 0, kPages - 1));
    hdr.ppsp = pages;
    hdr.pfnCallback = sheetCallback;
    PropertySheetW(&hdr);
}

} // namespace mp
