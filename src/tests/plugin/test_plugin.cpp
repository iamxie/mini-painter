// 測試用的讀圖 plugin（mp_test_plugin.dll）。測試把它複製成 plugins\<id>\<id>.dll，需要時改寫資訊資源。
// 檔案格式（小端序，24 bytes 檔頭）：
//   0..7   "MPTEST1\0"（格式 0，.mpt）或 "MPTEST2\0"（格式 1，.mpt2 / .mpu）
//   8..11  寬   12..15 高   16 指令   17..20 B G R A   21..23 不用
// 指令（decode）：'O' 照寬高填顏色；'D' 同 'O' 再給 300 × 150 dpi；'E' 回 MP_ERR_CORRUPT 加說明；
//   'N' 回成功但沒配置；'A' 寫入 NULL（存取違規）；'F' __fastfail；'W' 睡 30 秒再成功（給「主程式別的執行緒當掉」的測試）。
// 指令（readSize）：'a' 存取違規；'z' 回 MP_ERR_CORRUPT。指令（probe）：'P' 存取違規。
// DLL 檔名含 "badapi" 時回報的介面版本是 999；含 "nullentry" 時 mpPluginGetFuncs 回 NULL。
#include <windows.h>

#include <cstring>
#include <cwchar>

#include "plugins/sdk/mpainter_plugin.h"

#pragma warning(disable : 4702) // 故意當掉之後的 return 走不到

extern "C" IMAGE_DOS_HEADER __ImageBase;

namespace {

constexpr size_t kHeader = 24;

int32_t readI32(const uint8_t* p) {
    return static_cast<int32_t>(static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
                                (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24));
}

void crash() {
    volatile int* p = nullptr;
    *p = 1;
}

int32_t probe(const uint8_t* head, size_t size) {
    if (size >= 17 && head[16] == 'P' && std::memcmp(head, "MPTEST", 6) == 0) crash();
    if (size < 8) return -1;
    if (std::memcmp(head, "MPTEST1\0", 8) == 0) return 0;
    if (std::memcmp(head, "MPTEST2\0", 8) == 0) return 1;
    return -1;
}

int32_t readSize(int32_t, const uint8_t* data, size_t size, int32_t* width, int32_t* height) {
    if (size < kHeader) return MP_ERR_CORRUPT;
    if (data[16] == 'a') crash();
    if (data[16] == 'z') return MP_ERR_CORRUPT;
    *width = readI32(data + 8);
    *height = readI32(data + 12);
    return MP_OK;
}

int32_t decode(int32_t, const uint8_t* data, size_t size, const MpHost* host, MpDecodeInfo* info) {
    if (size < kHeader) {
        strcpy_s(info->message, sizeof(info->message), "file too short");
        return MP_ERR_CORRUPT;
    }
    const uint8_t cmd = data[16];
    switch (cmd) {
    case 'E': strcpy_s(info->message, sizeof(info->message), "test failure"); return MP_ERR_CORRUPT;
    case 'N': return MP_OK;
    case 'A': crash(); return MP_ERR_INTERNAL;
    case 'F': __fastfail(7); // FAST_FAIL_FATAL_APP_EXIT
    case 'W': Sleep(30000); break;
    default: break;
    }
    const int32_t w = readI32(data + 8), h = readI32(data + 12);
    uint8_t* px = host->allocImage(host->context, w, h);
    if (!px) return MP_ERR_OUT_OF_MEMORY;
    for (int64_t i = 0; i < static_cast<int64_t>(w) * h; ++i) std::memcpy(px + i * 4, data + 17, 4);
    if (cmd == 'D') {
        info->dpiX = 300;
        info->dpiY = 150;
    }
    return MP_OK;
}

const MpPluginFuncs kFuncs = {MP_PLUGIN_API_VERSION, probe, readSize, decode};
const MpPluginFuncs kBadApiFuncs = {999, probe, readSize, decode};

bool nameContains(const wchar_t* part) {
    wchar_t path[MAX_PATH] = {};
    GetModuleFileNameW(reinterpret_cast<HMODULE>(&__ImageBase), path, MAX_PATH);
    const wchar_t* name = wcsrchr(path, L'\\');
    return wcsstr(name ? name : path, part) != nullptr;
}

} // namespace

MP_PLUGIN_EXPORT const MpPluginFuncs* mpPluginGetFuncs(uint32_t hostApiVersion) {
    if (nameContains(L"nullentry") || hostApiVersion != MP_PLUGIN_API_VERSION) return nullptr;
    return nameContains(L"badapi") ? &kBadApiFuncs : &kFuncs;
}
