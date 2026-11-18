// 當掉測試用的子行程（mp_plugin_crash_child.exe），test_plugins.cpp 啟動它。
// 用法：mp_plugin_crash_child <plugins 資料夾> <紀錄資料夾> <mode> <檔案>
//   decode：loadImage；size：readImageSize；hostcrash：另一條執行緒在 plugin 裡（檔案指令 'W'）時，主執行緒自己當掉。
// 開紀錄、裝 filter、載入 plugins 資料夾裡全部的 plugin，然後照 mode 讀檔。沒當掉就標記正常結束、回 0。
#include <windows.h>

#include <string>
#include <thread>
#include <vector>

#include "core/ImageIO.h"
#include "core/PluginRun.h"
#include "core/Plugins.h"

int wmain(int argc, wchar_t** argv) {
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX); // 當掉時不跳視窗
    if (argc != 5) return 2;
    wchar_t exe[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    mp::installPluginCrashFilter();
    if (!mp::startPluginRunLog(argv[2], exe)) return 3;
    std::vector<mp::PluginEntry> entries = mp::scanPlugins(argv[1]);
    std::vector<std::wstring> ids;
    for (const auto& e : entries) ids.push_back(e.id);
    std::vector<mp::PluginLoadResult> results;
    mp::setActivePlugins(mp::loadPlugins(entries, ids, results));
    const std::wstring mode = argv[3], path = argv[4];
    if (mode == L"decode") {
        mp::loadImage(path);
    } else if (mode == L"size") {
        int w = 0, h = 0;
        mp::readImageSize(path, w, h);
    } else if (mode == L"hostcrash") {
        std::thread inPlugin([path] { mp::loadImage(path); });
        Sleep(1000); // 讓那條執行緒進到 plugin 裡
        volatile int* p = nullptr;
        *p = 1;
        inPlugin.join();
    } else {
        return 4;
    }
    mp::markPluginRunClean();
    return 0;
}
