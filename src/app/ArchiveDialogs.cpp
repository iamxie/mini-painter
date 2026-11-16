#include "app/ArchiveDialogs.h"

#include <cwchar>

#include "app/DialogUtil.h"
#include "app/ProgressDialog.h"
#include "app/UiText.h"
#include "app/resource.h"
#include "core/Lang.h"
#include "core/PathUtil.h"

namespace mp {

namespace {

struct PasswordState {
    std::wstring name;
    bool retry = false;
    std::wstring out;
};

INT_PTR CALLBACK passwordProc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp) {
    auto* st = reinterpret_cast<PasswordState*>(GetWindowLongPtrW(dlg, DWLP_USER));
    switch (msg) {
    case WM_INITDIALOG: {
        st = reinterpret_cast<PasswordState*>(lp);
        SetWindowLongPtrW(dlg, DWLP_USER, lp);
        translateDialog(dlg);
        std::wstring prompt(wcslen(tr(L"“%s” is encrypted. Enter the password:")) + st->name.size() + 8, L'\0');
        int n = swprintf(prompt.data(), prompt.size(), tr(L"“%s” is encrypted. Enter the password:"), st->name.c_str());
        prompt.resize(n > 0 ? static_cast<size_t>(n) : 0);
        SetDlgItemTextW(dlg, IDC_PW_PROMPT, prompt.c_str());
        SetDlgItemTextW(dlg, IDC_PW_NOTE, st->retry ? tr(L"Wrong password. Try again.") : L"");
        SendDlgItemMessageW(dlg, IDC_PW_EDIT, EM_SETLIMITTEXT, 255, 0);
        SetForegroundWindow(dlg);
        SetFocus(GetDlgItem(dlg, IDC_PW_EDIT));
        return FALSE; // 焦點已自己設
    }
    case WM_COMMAND:
        if (LOWORD(wp) == IDOK) {
            wchar_t buf[256];
            GetDlgItemTextW(dlg, IDC_PW_EDIT, buf, 256);
            st->out = buf;
            EndDialog(dlg, IDOK);
            return TRUE;
        }
        if (LOWORD(wp) == IDCANCEL) { EndDialog(dlg, IDCANCEL); return TRUE; }
        break;
    }
    return FALSE;
}

// 「正在解壓縮「檔名」…」
std::wstring unpackingText(const std::wstring& archivePath) { return fmt1(L"Unpacking “%s”…", fileNameOf(archivePath)); }

} // namespace

bool showPasswordDialog(HWND owner, const std::wstring& archiveName, bool retry, std::wstring& password) {
    PasswordState st;
    st.name = archiveName;
    st.retry = retry;
    HINSTANCE inst = reinterpret_cast<HINSTANCE>(GetWindowLongPtrW(owner, GWLP_HINSTANCE));
    INT_PTR r = DialogBoxParamW(inst, MAKEINTRESOURCEW(IDD_PASSWORD), owner, passwordProc, reinterpret_cast<LPARAM>(&st));
    if (r != IDOK) return false;
    password = st.out;
    return true;
}

PasswordExtract extractWithProgress(HWND owner, const std::wstring& archivePath, const std::wstring& destDir,
                                    const ExtractOptions& opt, const std::vector<std::wstring>& passwords) {
    ProgressState st;
    st.caption = tr(L"Unpacking");
    st.text = unpackingText(archivePath);
    // 背景執行緒不查翻譯表：要用的字先在這裡備好。
    const std::wstring unpackCaption = st.caption, unpackText = st.text, passwordCaption = tr(L"Password");
    const std::wstring tryingFormat = tr(L"Trying saved passwords (%d/%d)…");
    PasswordExtract result;
    st.work = [&](ProgressState& s) {
        bool trying = false; // 進度框現在顯示的是「正在試密碼」
        ExtractOptions o = opt;
        o.progress = [&](uint64_t done, uint64_t total) {
            if (s.cancel.load()) return false;
            if (trying) { // 密碼試出來了，回到解壓
                trying = false;
                s.setCaption(unpackCaption);
                s.setText(unpackText);
            }
            s.setPos(done, total);
            return true;
        };
        PasswordHooks hooks;
        hooks.cancelled = [&s] { return s.cancel.load(); };
        hooks.onTry = [&](int index, int total) {
            if (!trying) {
                trying = true;
                s.setCaption(passwordCaption);
            }
            wchar_t buf[128];
            swprintf(buf, 128, tryingFormat.c_str(), index, total);
            s.setText(buf);
            s.setPos(static_cast<uint64_t>(index - 1), static_cast<uint64_t>(total));
        };
        result = extractWithPasswords(archivePath, destDir, o, passwords, hooks);
    };
    // 建不出對話框就同步解，沒有進度也沒有取消。
    if (!runWithProgress(owner, st)) return extractWithPasswords(archivePath, destDir, opt, passwords);
    return result;
}

void runUnpackWithProgress(HWND owner, const std::wstring& archivePath,
                           const std::function<void(const std::function<bool(uint64_t, uint64_t)>& progress)>& work) {
    ProgressState st;
    st.caption = tr(L"Unpacking");
    st.text = unpackingText(archivePath);
    st.work = [&](ProgressState& s) {
        work([&s](uint64_t done, uint64_t total) {
            if (s.cancel.load()) return false;
            s.setPos(done, total);
            return true;
        });
    };
    if (!runWithProgress(owner, st)) work(nullptr);
}

} // namespace mp
