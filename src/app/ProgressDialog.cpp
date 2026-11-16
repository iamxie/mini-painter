#include "app/ProgressDialog.h"

#include <commctrl.h>

#include "app/DialogUtil.h"
#include "app/resource.h"
#include "core/Lang.h"

namespace mp {

namespace {

constexpr UINT WM_PRG_POS = WM_APP + 10;     // wParam = 千分比
constexpr UINT WM_PRG_TEXT = WM_APP + 11;    // lParam = new 出來的 std::wstring*，收到的人 delete
constexpr UINT WM_PRG_DONE = WM_APP + 12;
constexpr UINT WM_PRG_CAPTION = WM_APP + 13; // lParam = new 出來的 std::wstring*，收到的人 delete
constexpr int kBarRange = 1000;
// 這麼久還沒做完才顯示對話框。
constexpr UINT kShowDelayMs = 300;
constexpr UINT_PTR kShowTimer = 1;

// 使用者要取消：記下來、通知 work 那一側、畫面上說「取消中」。對話框等 work 回來才關。
void requestCancel(HWND dlg, ProgressState& st) {
    if (st.cancel.exchange(true)) return;
    EnableWindow(GetDlgItem(dlg, IDCANCEL), FALSE);
    SetDlgItemTextW(dlg, IDC_PRG_TEXT, tr(L"Cancelling…"));
    if (st.onCancel) st.onCancel();
}

INT_PTR CALLBACK progressProc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp) {
    auto* st = reinterpret_cast<ProgressState*>(GetWindowLongPtrW(dlg, DWLP_USER));
    switch (msg) {
    case WM_INITDIALOG: {
        st = reinterpret_cast<ProgressState*>(lp);
        st->dlg = dlg;
        SetWindowLongPtrW(dlg, DWLP_USER, lp);
        translateDialog(dlg);
        SetWindowTextW(dlg, st->caption.c_str());
        SetDlgItemTextW(dlg, IDC_PRG_TEXT, st->text.c_str());
        SendDlgItemMessageW(dlg, IDC_PRG_BAR, PBM_SETRANGE32, 0, kBarRange);
        st->worker = std::thread([st] {
            st->work(*st);
            PostMessageW(st->dlg, WM_PRG_DONE, 0, 0);
        });
        SetTimer(dlg, kShowTimer, kShowDelayMs, nullptr);
        return TRUE;
    }
    case WM_TIMER:
        KillTimer(dlg, kShowTimer);
        if (!st->done && !st->shown) {
            st->shown = true;
            ShowWindow(dlg, SW_SHOW);
            SetForegroundWindow(dlg);
        }
        return TRUE;
    case WM_PRG_POS:
        SendDlgItemMessageW(dlg, IDC_PRG_BAR, PBM_SETPOS, wp, 0);
        return TRUE;
    case WM_PRG_TEXT: {
        auto* t = reinterpret_cast<std::wstring*>(lp);
        if (!st->cancel) SetDlgItemTextW(dlg, IDC_PRG_TEXT, t->c_str());
        delete t;
        return TRUE;
    }
    case WM_PRG_CAPTION: {
        auto* t = reinterpret_cast<std::wstring*>(lp);
        SetWindowTextW(dlg, t->c_str());
        delete t;
        return TRUE;
    }
    case WM_PRG_DONE:
        st->done = true;
        return TRUE;
    case WM_COMMAND:
        if (LOWORD(wp) == IDCANCEL) {
            requestCancel(dlg, *st);
            return TRUE;
        }
        break;
    case WM_CLOSE:
        requestCancel(dlg, *st);
        return TRUE;
    }
    return FALSE;
}

} // namespace

void ProgressState::setPos(uint64_t doneBytes, uint64_t total) {
    if (dlg) PostMessageW(dlg, WM_PRG_POS, total ? static_cast<WPARAM>(doneBytes * kBarRange / total) : 0, 0);
}
void ProgressState::setText(const std::wstring& t) {
    if (dlg) PostMessageW(dlg, WM_PRG_TEXT, 0, reinterpret_cast<LPARAM>(new std::wstring(t)));
}
void ProgressState::setCaption(const std::wstring& t) {
    if (dlg) PostMessageW(dlg, WM_PRG_CAPTION, 0, reinterpret_cast<LPARAM>(new std::wstring(t)));
}

bool runWithProgress(HWND owner, ProgressState& st) {
    // modeless 對話框 + 自己的訊息迴圈：這樣才能決定要不要顯示（DialogBox 一定會顯示）。
    HINSTANCE inst = reinterpret_cast<HINSTANCE>(GetWindowLongPtrW(owner, GWLP_HINSTANCE));
    HWND dlg = CreateDialogParamW(inst, MAKEINTRESOURCEW(IDD_PROGRESS), owner, progressProc, reinterpret_cast<LPARAM>(&st));
    if (!dlg) return false;
    EnableWindow(owner, FALSE);
    MSG msg{};
    BOOL got = 1;
    while (!st.done && (got = GetMessageW(&msg, nullptr, 0, 0)) > 0) {
        if (!IsDialogMessageW(dlg, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    // 還沒做完就離開迴圈：收到 WM_QUIT（程式要結束了）。叫背景停手再等它。
    if (!st.done) {
        st.cancel = true;
        if (st.onCancel) st.onCancel();
    }
    if (st.worker.joinable()) st.worker.join();
    EnableWindow(owner, TRUE);
    DestroyWindow(dlg);
    st.dlg = nullptr;
    SetActiveWindow(owner);
    // WM_QUIT 被這個迴圈吃掉了，還給外層的訊息迴圈，不然程式不會結束。
    if (got == 0) PostQuitMessage(static_cast<int>(msg.wParam));
    return true;
}

} // namespace mp
