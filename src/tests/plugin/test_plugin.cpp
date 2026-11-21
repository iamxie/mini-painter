// 測試用的讀圖 plugin（mp_test_plugin.dll）。測試把它複製成 plugins\<id>\<id>.dll，需要時改寫資訊資源。
// 介面 v1、v2 都講：主程式照資訊的 api= 傳版本進來，回那一版的函式表（同一組函式）。
// 檔案格式（小端序，24 bytes 檔頭）：
//   0..7   "MPTEST1\0"（格式 0，.mpt）或 "MPTEST2\0"（格式 1，.mpt2 / .mpu）
//          "MPTESTL\0"：第 100 byte 是 'L' 才認（格式 0）—— 要看到整個檔（v2）才認得出來
//          "II*\0"（TIFF 檔頭）：第 100 起是 "MPTIFF" 才認（格式 1），像 DNG 一樣藏在 TIFF 裡
//          "....ftyp"（HEIF 檔頭）：第 100 起是一份完整的 24 bytes 檔頭、開頭 "MPHEIF\0\0" 才認（格式 1），
//          像自帶解碼器的 HEIC plugin；寬高、指令、顏色從那一份讀
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

constexpr size_t kHeifRecord = 100;

bool isHeifFake(const uint8_t* data, size_t size) {
    return size >= kHeifRecord + kHeader && std::memcmp(data + 4, "ftyp", 4) == 0 &&
           std::memcmp(data + kHeifRecord, "MPHEIF\0\0", 8) == 0;
}

// 寬高、指令、顏色從哪裡讀：HEIF 檔頭的假檔在第 100 byte，其他在開頭。
const uint8_t* recordOf(const uint8_t* data, size_t size) { return isHeifFake(data, size) ? data + kHeifRecord : data; }

int32_t probe(const uint8_t* data, size_t size) {
    if (size >= 17 && data[16] == 'P' && std::memcmp(data, "MPTEST", 6) == 0) crash();
    if (size < 8) return -1;
    if (std::memcmp(data, "MPTEST1\0", 8) == 0) return 0;
    if (std::memcmp(data, "MPTEST2\0", 8) == 0) return 1;
    if (std::memcmp(data, "MPTESTL\0", 8) == 0) return size > 100 && data[100] == 'L' ? 0 : -1;
    if (std::memcmp(data, "II*\0", 4) == 0) return size >= 106 && std::memcmp(data + 100, "MPTIFF", 6) == 0 ? 1 : -1;
    if (std::memcmp(data + 4, "ftyp", 4) == 0) return isHeifFake(data, size) ? 1 : -1;
    return -1;
}

int32_t readSize(int32_t, const uint8_t* data, size_t size, int32_t* width, int32_t* height) {
    if (size < kHeader) return MP_ERR_CORRUPT;
    data = recordOf(data, size);
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
    data = recordOf(data, size);
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
const MpPluginFuncs kFuncsV1 = {1, probe, readSize, decode};
const MpPluginFuncs kBadApiFuncs = {999, probe, readSize, decode};

bool nameContains(const wchar_t* part) {
    wchar_t path[MAX_PATH] = {};
    GetModuleFileNameW(reinterpret_cast<HMODULE>(&__ImageBase), path, MAX_PATH);
    const wchar_t* name = wcsrchr(path, L'\\');
    return wcsstr(name ? name : path, part) != nullptr;
}

} // namespace

MP_PLUGIN_EXPORT const MpPluginFuncs* mpPluginGetFuncs(uint32_t hostApiVersion) {
    if (nameContains(L"nullentry")) return nullptr;
    if (nameContains(L"badapi")) return &kBadApiFuncs;
    if (hostApiVersion == 1) return &kFuncsV1;
    return hostApiVersion == MP_PLUGIN_API_VERSION ? &kFuncs : nullptr;
}
