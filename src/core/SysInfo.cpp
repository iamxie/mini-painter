#include "core/SysInfo.h"

#include <windows.h>

#include <vector>

namespace mp {

namespace {
constexpr unsigned kFirstWindows11Build = 22000;
}

unsigned windowsBuildNumber() {
    HMODULE k = GetModuleHandleW(L"kernel32.dll");
    if (!k) return 0;
    wchar_t path[MAX_PATH];
    if (!GetModuleFileNameW(k, path, MAX_PATH)) return 0;
    DWORD dummy = 0;
    DWORD size = GetFileVersionInfoSizeW(path, &dummy);
    if (!size) return 0;
    std::vector<BYTE> data(size);
    if (!GetFileVersionInfoW(path, 0, size, data.data())) return 0;
    VS_FIXEDFILEINFO* ffi = nullptr;
    UINT len = 0;
    if (!VerQueryValueW(data.data(), L"\\", reinterpret_cast<void**>(&ffi), &len) || !ffi) return 0;
    // 檔案版本是 主.次.build.修訂：build 在低 32 位的高 16 位。主版號不看：沒有相容性 manifest 的行程
    // 讀到的是 6.2（系統替舊程式改過的值），build 那一格不受影響。
    return HIWORD(ffi->dwFileVersionLS);
}

bool isWindows11() { return windowsBuildNumber() >= kFirstWindows11Build; }

} // namespace mp
