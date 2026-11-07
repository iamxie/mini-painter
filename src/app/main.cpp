#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>

#include <string>

#include "app/MainWindow.h"
#include "core/Lang.h"

#pragma comment(lib, "comctl32")

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, PWSTR, int nCmdShow) {
    // 秒開優先：先做最少的初始化。
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    INITCOMMONCONTROLSEX icc{sizeof(icc), ICC_STANDARD_CLASSES | ICC_BAR_CLASSES | ICC_UPDOWN_CLASS | ICC_PROGRESS_CLASS};
    InitCommonControlsEx(&icc);

    // 命令列：mpainter.exe [路徑]
    // 自動更新重啟時：mpainter.exe --after-update <舊行程 pid> [路徑]。先等舊行程結束（它在存設定、刪暫存），
    // 不然兩邊同時讀寫 ini，換下來的 .old 也刪不掉。
    std::wstring path;
    bool afterUpdate = false;
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argv) {
        if (argc >= 3 && wcscmp(argv[1], L"--after-update") == 0) {
            afterUpdate = true;
            if (HANDLE old = OpenProcess(SYNCHRONIZE, FALSE, wcstoul(argv[2], nullptr, 10))) {
                WaitForSingleObject(old, 15000);
                CloseHandle(old);
            }
            if (argc >= 4) path = argv[3];
        } else if (argc >= 2) {
            path = argv[1];
        }
        LocalFree(argv);
    }

    mp::MainWindow win;
    win.setJustUpdated(afterUpdate);
    if (!win.create(hInstance, path, nCmdShow)) {
        MessageBoxW(nullptr, mp::tr(L"Could not create the window."), L"mpainter", MB_ICONERROR);
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
