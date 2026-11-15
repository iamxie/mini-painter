#pragma once

namespace mp {

// Windows 的 build 號。讀 kernel32.dll 的檔案版本：GetVersionEx 會被 manifest 影響。
// 拿到的是那一代的基底（19045 的系統是 19041、22631 的系統是 22621），足夠分辨 Windows 10 與 11。讀不到回 0。
unsigned windowsBuildNumber();
// Windows 11 以後（build 22000 以上）。
bool isWindows11();

} // namespace mp
