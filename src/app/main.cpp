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

    std::wstring path;
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argv) {
        if (argc >= 2) path = argv[1];
        LocalFree(argv);
    }

    mp::MainWindow win;
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
