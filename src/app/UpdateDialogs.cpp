#include "app/UpdateDialogs.h"

#include <atomic>
#include <cwchar>

#include "app/DialogUtil.h"
#include "app/ProgressDialog.h"
#include "app/UiText.h"
#include "app/resource.h"
#include "core/Lang.h"

namespace mp {

namespace {

struct UpdateDlgState {
    const ReleaseInfo* release = nullptr;
    std::wstring current;
    HFONT bold = nullptr;
    HICON icon = nullptr;
};

INT_PTR CALLBACK updateProc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp) {
    auto* st = reinterpret_cast<UpdateDlgState*>(GetWindowLongPtrW(dlg, DWLP_USER));
    switch (msg) {
    case WM_INITDIALOG: {
        st = reinterpret_cast<UpdateDlgState*>(lp);
        SetWindowLongPtrW(dlg, DWLP_USER, lp);
        translateDialog(dlg);
        HINSTANCE inst = reinterpret_cast<HINSTANCE>(GetWindowLongPtrW(dlg, GWLP_HINSTANCE));
        int size = MulDiv(48, static_cast<int>(GetDpiForWindow(dlg)), 96);
        st->icon = static_cast<HICON>(LoadImageW(inst, MAKEINTRESOURCEW(IDI_APP), IMAGE_ICON, size, size, LR_DEFAULTCOLOR));
        if (st->icon) SendDlgItemMessageW(dlg, IDC_UPD_ICON, STM_SETICON, reinterpret_cast<WPARAM>(st->icon), 0);
        HFONT base = reinterpret_cast<HFONT>(SendMessageW(dlg, WM_GETFONT, 0, 0));
        LOGFONTW lf{};
        GetObjectW(base, sizeof(lf), &lf);
        lf.lfWeight = FW_BOLD;
        st->bold = CreateFontIndirectW(&lf);
        SendDlgItemMessageW(dlg, IDC_UPD_TITLE, WM_SETFONT, reinterpret_cast<WPARAM>(st->bold), TRUE);
        const wchar_t* f = tr(L"mpainter %s is available (you have %s). Would you like to install it now?");
        std::wstring text(wcslen(f) + st->release->version.size() + st->current.size() + 8, L'\0');
        int n = swprintf(text.data(), text.size(), f, st->release->version.c_str(), st->current.c_str());
        text.resize(n > 0 ? static_cast<size_t>(n) : 0);
        SetDlgItemTextW(dlg, IDC_UPD_TEXT, text.c_str());
        // EDIT 控制項要 \r\n 才換行；GitHub 的說明可能是 \n 或 \r\n。
        std::wstring notes;
        for (wchar_t c : st->release->notes) {
            if (c == L'\r') continue;
            if (c == L'\n') notes += L'\r';
            notes += c;
        }
        SetDlgItemTextW(dlg, IDC_UPD_NOTES, notes.empty() ? tr(L"(No release notes.)") : notes.c_str());
        SetForegroundWindow(dlg);
        SetFocus(GetDlgItem(dlg, IDC_UPD_INSTALL));
        return FALSE; // 焦點已自己設
    }
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDC_UPD_SKIP: EndDialog(dlg, static_cast<INT_PTR>(UpdateChoice::Skip)); return TRUE;
        case IDC_UPD_INSTALL: case IDOK: EndDialog(dlg, static_cast<INT_PTR>(UpdateChoice::Install)); return TRUE;
        case IDC_UPD_LATER: case IDCANCEL: EndDialog(dlg, static_cast<INT_PTR>(UpdateChoice::Later)); return TRUE;
        default: break;
        }
        break;
    case WM_DESTROY:
        if (st && st->bold) DeleteObject(st->bold);
        if (st && st->icon) DestroyIcon(st->icon);
        return TRUE;
    }
    return FALSE;
}

// 等其他視窗回覆「要不要關」最多等多久（對方可能在問使用者要不要存檔）。
constexpr DWORD kCloseReplyTimeoutMs = 10 * 60 * 1000;
// 對方答應關了之後，等行程真的結束最多等多久（存設定、刪暫存資料夾）。
constexpr DWORD kExitWaitMs = 30000;
constexpr DWORD kPollMs = 100;

} // namespace

UpdateChoice showUpdateDialog(HWND owner, const ReleaseInfo& release, const std::wstring& currentVersion) {
    UpdateDlgState st;
    st.release = &release;
    st.current = currentVersion;
    HINSTANCE inst = reinterpret_cast<HINSTANCE>(GetWindowLongPtrW(owner, GWLP_HINSTANCE));
    INT_PTR r = DialogBoxParamW(inst, MAKEINTRESOURCEW(IDD_UPDATE), owner, updateProc, reinterpret_cast<LPARAM>(&st));
    if (r == static_cast<INT_PTR>(UpdateChoice::Skip)) return UpdateChoice::Skip;
    if (r == static_cast<INT_PTR>(UpdateChoice::Install)) return UpdateChoice::Install;
    return UpdateChoice::Later;
}

UpdateError checkUpdateWithProgress(HWND owner, const std::wstring& url, ReleaseInfo& out) {
    ProgressState st;
    st.caption = tr(L"Software Update");
    st.text = tr(L"Checking for updates…");
    HttpCancel http;
    st.onCancel = [&] { http.cancel(); }; // 等伺服器回應的時候卡在 WinHTTP 裡：直接把連線關掉
    UpdateError err;
    st.work = [&](ProgressState&) { err = fetchLatestRelease(url, out, &http); };
    if (!runWithProgress(owner, st)) st.work(st);
    if (st.cancel.load()) err.code = UpdateErrorCode::Cancelled; // 按了取消就算取消，即使剛好查到了
    return err;
}

UpdateError downloadUpdateWithProgress(HWND owner, const ReleaseInfo& release, const std::wstring& workDir, std::wstring& stagingDir) {
    ProgressState st;
    st.caption = tr(L"Software Update");
    st.text = fmt1(L"Downloading mpainter %s…", release.version);
    // 背景執行緒不查翻譯表：要用的字先在這裡備好。
    const std::wstring verifying = tr(L"Verifying the download…"), unpacking = tr(L"Unpacking the update…");
    HttpCancel http;
    st.onCancel = [&] { http.cancel(); };
    UpdateError err;
    st.work = [&](ProgressState& s) {
        DownloadHooks hooks;
        hooks.cancel = &http;
        hooks.progress = [&s](uint64_t done, uint64_t total) { s.setPos(done, total); };
        hooks.onStage = [&](DownloadStage stage) {
            if (stage == DownloadStage::Verifying) s.setText(verifying);
            else if (stage == DownloadStage::Unpacking) s.setText(unpacking);
        };
        err = downloadUpdate(release, workDir, stagingDir, hooks);
    };
    if (!runWithProgress(owner, st)) st.work(st);
    if (!err.ok() && st.cancel.load()) err.code = UpdateErrorCode::Cancelled;
    return err;
}

bool closeWindowsWithProgress(HWND owner, UINT closeMsg, const std::vector<HWND>& windows, const std::vector<HANDLE>& processes) {
    ProgressState st;
    st.caption = tr(L"Software Update");
    st.text = tr(L"Closing the other mpainter windows…");
    bool allClosed = false;
    st.work = [&](ProgressState& s) {
        // 對方可能正在問使用者要不要存檔，要等到有人回答。用會卡住的 SendMessage 的話，這期間這邊按取消沒有反應；
        // 所以用 SendMessageCallback 送出去、自己邊等邊看取消。回覆在這條執行緒收訊息的時候送到。
        struct Reply {
            bool arrived = false;
            LRESULT agreed = 0;
        };
        for (HWND w : windows) {
            Reply reply;
            const BOOL sent = SendMessageCallbackW(
                w, closeMsg, GetCurrentProcessId(), reinterpret_cast<LPARAM>(s.dlg),
                [](HWND, UINT, ULONG_PTR data, LRESULT result) {
                    auto* r = reinterpret_cast<Reply*>(data);
                    r->arrived = true;
                    r->agreed = result;
                },
                reinterpret_cast<ULONG_PTR>(&reply));
            if (!sent) {
                if (IsWindow(w)) return; // 送不出去而視窗還在：當成沒關
                continue;
            }
            for (DWORD waited = 0; !reply.arrived; waited += kPollMs) {
                // 這裡回去之後這條執行緒就結束、不再收訊息，晚到的回覆不會被送到已經不在的 reply 上。
                if (s.cancel.load() || waited >= kCloseReplyTimeoutMs) return;
                if (!IsWindow(w)) break; // 視窗已經不在了：就是關了
                MsgWaitForMultipleObjects(0, nullptr, FALSE, kPollMs, QS_ALLINPUT);
                MSG m;
                while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) DispatchMessageW(&m);
            }
            if (reply.arrived && !reply.agreed && IsWindow(w)) return; // 使用者在那邊按了取消，或那邊正忙
        }
        // 等行程真的結束。
        const DWORD n = static_cast<DWORD>(processes.size());
        for (DWORD waited = 0; waited < kExitWaitMs; waited += kPollMs) {
            if (s.cancel.load()) return;
            DWORD r = n ? WaitForMultipleObjects(n, processes.data(), TRUE, kPollMs) : WAIT_OBJECT_0;
            if (r < WAIT_OBJECT_0 + n || n == 0) {
                allClosed = true;
                return;
            }
        }
    };
    if (!runWithProgress(owner, st)) st.work(st);
    return allClosed;
}

} // namespace mp
