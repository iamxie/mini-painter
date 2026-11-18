// app/UpdateDialogs：自動更新的進度框。查詢與下載那一步換成假的，驗證「按過取消就一律算取消」。
// 真的建進度框（app/ProgressDialog，範本在 test_dialogs.rc），在這條執行緒跑它的訊息迴圈。
#include <catch2/catch_test_macros.hpp>

#include <windows.h>
#include <commctrl.h>

#include <string>

#include "app/UpdateDialogs.h"
#include "core/Http.h"

// app/DialogUtil.cpp 用到 TaskDialogIndirect，只有 Common Controls 6 有：和主程式（app.manifest）一樣要第 6 版，
// 不然測試程式啟動時就找不到這個函式。
#pragma comment(linker, "/manifestdependency:\"type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' " \
                        "processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

using namespace mp;

namespace {

// 進度框的 owner：隱藏的視窗，hInstance 是測試程式（進度框的範本從 owner 的 hInstance 找）。
HWND makeOwner() {
    static const ATOM cls = [] {
        INITCOMMONCONTROLSEX icc{sizeof(icc), ICC_PROGRESS_CLASS};
        InitCommonControlsEx(&icc);
        WNDCLASSW wc{};
        wc.lpfnWndProc = DefWindowProcW;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = L"mp_test_progress_owner";
        return RegisterClassW(&wc);
    }();
    if (!cls) return nullptr;
    return CreateWindowExW(0, L"mp_test_progress_owner", L"", WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, GetModuleHandleW(nullptr),
                           nullptr);
}

struct FindOwned {
    HWND owner;
    HWND found;
};

BOOL CALLBACK findOwnedProc(HWND h, LPARAM lp) {
    auto* f = reinterpret_cast<FindOwned*>(lp);
    if (GetWindow(h, GW_OWNER) != f->owner) return TRUE;
    f->found = h;
    return FALSE;
}

// 假的那一步（在進度框的背景執行緒跑）：pressCancel 時按下進度框的「取消」，等 onCancel 把連線關掉
// （HttpCancel 被取消），然後照樣回報成功，模擬「按取消的同時剛好做完了」。結果記下來，回到測試的執行緒再檢查。
struct FakeStep {
    DWORD uiThread = GetCurrentThreadId();
    HWND owner = nullptr;
    bool pressCancel = false;
    bool foundDialog = false;
    bool sawCancel = false;

    void run(HttpCancel* cancel) {
        if (!pressCancel) return;
        FindOwned f{owner, nullptr};
        EnumThreadWindows(uiThread, findOwnedProc, reinterpret_cast<LPARAM>(&f)); // 0.3 秒內還沒顯示，但已經建好了
        foundDialog = f.found != nullptr;
        if (!f.found || !cancel) return;
        PostMessageW(f.found, WM_COMMAND, MAKEWPARAM(IDCANCEL, BN_CLICKED), 0);
        for (int i = 0; i < 1000 && !sawCancel; ++i) { // 最多 10 秒
            sawCancel = cancel->cancelled();
            if (!sawCancel) Sleep(10);
        }
    }
};

} // namespace

TEST_CASE("checkUpdateWithProgress：按過取消就回 Cancelled，即使查詢那一步剛好成功了；沒按就照實回報") {
    HWND owner = makeOwner();
    REQUIRE(owner);
    for (bool press : {true, false}) {
        CAPTURE(press);
        FakeStep fake;
        fake.owner = owner;
        fake.pressCancel = press;
        ReleaseInfo out;
        const UpdateError err = checkUpdateWithProgress(owner, L"http://127.0.0.1/unused", out,
                                                        [&](const std::wstring&, ReleaseInfo& r, HttpCancel* cancel) {
                                                            fake.run(cancel);
                                                            r.version = L"9.9.9";
                                                            return UpdateError{};
                                                        });
        CHECK(out.version == L"9.9.9");
        if (press) {
            CHECK(fake.foundDialog);
            CHECK(fake.sawCancel); // 按取消會呼叫 onCancel：關掉連線
            CHECK(err.code == UpdateErrorCode::Cancelled);
        } else {
            CHECK(err.ok());
        }
    }
    DestroyWindow(owner);
}

TEST_CASE("downloadUpdateWithProgress：按過取消就回 Cancelled，即使下載與解壓剛好做完了；沒按就照實回報") {
    HWND owner = makeOwner();
    REQUIRE(owner);
    for (bool press : {true, false}) {
        CAPTURE(press);
        FakeStep fake;
        fake.owner = owner;
        fake.pressCancel = press;
        ReleaseInfo release;
        release.version = L"9.9.9";
        std::wstring staging;
        const UpdateError err = downloadUpdateWithProgress(
            owner, release, L"C:\\unused", staging,
            [&](const ReleaseInfo&, const std::wstring& workDir, std::wstring& stagingDir, const DownloadHooks& hooks) {
                fake.run(hooks.cancel);
                stagingDir = workDir + L"\\pkg";
                return UpdateError{};
            });
        CHECK(staging == L"C:\\unused\\pkg");
        if (press) {
            CHECK(fake.foundDialog);
            CHECK(fake.sawCancel);
            CHECK(err.code == UpdateErrorCode::Cancelled);
        } else {
            CHECK(err.ok());
        }
    }
    DestroyWindow(owner);
}
