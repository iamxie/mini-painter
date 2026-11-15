#include "core/TextFile.h"

#include <windows.h>

#include "core/TextUtil.h"

namespace mp {

FileRead readFileBytes(const std::wstring& path, std::string& out) {
    out.clear();
    // 分享 DELETE：別的 mpainter 這時存檔（暫存檔取代原檔）不會被我們擋住。
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        DWORD e = GetLastError();
        return e == ERROR_FILE_NOT_FOUND || e == ERROR_PATH_NOT_FOUND ? FileRead::Missing : FileRead::Unreadable;
    }
    char buf[8192];
    DWORD got = 0;
    bool ok = true;
    for (;;) {
        if (!ReadFile(h, buf, sizeof(buf), &got, nullptr)) { ok = false; break; }
        if (got == 0) break;
        out.append(buf, got);
    }
    CloseHandle(h);
    if (!ok) { // 讀到一半失敗：半份不能用
        out.clear();
        return FileRead::Unreadable;
    }
    return FileRead::Ok;
}

FileRead readUtf8File(const std::wstring& path, std::wstring& out) {
    out.clear();
    std::string bytes;
    FileRead r = readFileBytes(path, bytes);
    if (r != FileRead::Ok) return r;
    if (bytes.size() >= 3 && static_cast<unsigned char>(bytes[0]) == 0xEF && static_cast<unsigned char>(bytes[1]) == 0xBB &&
        static_cast<unsigned char>(bytes[2]) == 0xBF)
        bytes.erase(0, 3);
    out = utf8ToWide(bytes);
    return FileRead::Ok;
}

} // namespace mp
