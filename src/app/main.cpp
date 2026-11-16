#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>

#include <string>
#include <vector>

#include "app/DialogUtil.h"
#include "app/MainWindow.h"
#include "core/Lang.h"
#include "core/Launch.h"

#pragma comment(lib, "comctl32")

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, PWSTR, int nCmdShow) {
    // 秒開優先：先做最少的初始化。
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    INITCOMMONCONTROLSEX icc{sizeof(icc), ICC_STANDARD_CLASSES | ICC_BAR_CLASSES | ICC_UPDOWN_CLASS | ICC_PROGRESS_CLASS};
    InitCommonControlsEx(&icc);

    // 命令列：mpainter.exe [路徑]
    // 自動更新重啟時：mpainter.exe --after-update <舊行程 pid> [路徑]。先等舊行程結束（它在存設定、刪暫存），
    // 不然兩邊同時讀寫 ini，換下來的 .old 也刪不掉。
    mp::LaunchArgs launch;
    int argc = 0;
    if (LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc)) {
        launch = mp::parseLaunchArgs(std::vector<std::wstring>(argv, argv + argc));
        LocalFree(argv);
    }
    if (launch.afterUpdate) {
        if (HANDLE old = OpenProcess(SYNCHRONIZE, FALSE, launch.waitPid)) {
            WaitForSingleObject(old, 15000);
            CloseHandle(old);
        }
    }

    mp::MainWindow win;
    win.setJustUpdated(launch.afterUpdate);
    if (!win.create(hInstance, launch.path, nCmdShow)) {
        mp::showMessage(nullptr, mp::tr(L"Could not create the window."), TD_ERROR_ICON);
        return 1;
    }

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (win.preTranslate(msg)) continue;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    CoUninitialize();
    return static_cast<int>(msg.wParam);
}
