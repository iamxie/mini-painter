#include "core/Metadata.h"

#include "core/Lang.h"

#include <cstdio>
#include <cstring>

namespace mp {

static unsigned be32(const unsigned char* p) {
    return (static_cast<unsigned>(p[0]) << 24) | (p[1] << 16) | (p[2] << 8) | p[3];
}
static unsigned be16(const unsigned char* p) { return (static_cast<unsigned>(p[0]) << 8) | p[1]; }

std::wstring iccProfileDescription(const unsigned char* data, size_t size) {
    // ICC 檔頭 128 bytes，接著 tag 數量，每個 tag 12 bytes：sig、offset、size。
    if (!data || size < 132) return L"";
    unsigned count = be32(data + 128);
    if (count > 4096 || 132 + static_cast<size_t>(count) * 12 > size) return L"";
    for (unsigned i = 0; i < count; ++i) {
        const unsigned char* t = data + 132 + i * 12;
        if (std::memcmp(t, "desc", 4) != 0) continue;
        unsigned off = be32(t + 4), len = be32(t + 8);
        if (off + 12 > size || off + len > size || len < 12) return L"";
        const unsigned char* d = data + off;
        if (std::memcmp(d, "desc", 4) == 0) {
            // v2：textDescriptionType，ASCII 長度在 +8，字串在 +12（含結尾 0）。
            unsigned n = be32(d + 8);
            if (n == 0 || 12 + n > len) return L"";
            std::wstring s;
            for (unsigned k = 0; k < n && d[12 + k]; ++k) s += static_cast<wchar_t>(d[12 + k]);
            return s;
        }
        if (std::memcmp(d, "mluc", 4) == 0) {
            // v4：multiLocalizedUnicodeType，取第一筆紀錄，UTF-16BE。
            if (len < 28) return L"";
            unsigned recLen = be32(d + 16);
            unsigned strLen = be32(d + 20), strOff = be32(d + 24);
            if (recLen < 12 || strOff + strLen > len) return L"";
            std::wstring s;
            for (unsigned k = 0; k + 1 < strLen; k += 2) {
                wchar_t ch = static_cast<wchar_t>(be16(d + strOff + k));
                if (ch == 0) break;
                s += ch;
            }
            return s;
        }
        return L"";
    }
    return L"";
}

std::wstring exifTagName(unsigned tag) {
    switch (tag) {
    case 0x0100: return tr(L"Image width");
    case 0x0101: return tr(L"Image height");
    case 0x0102: return tr(L"Bits per sample");
    case 0x0103: return tr(L"Compression");
    case 0x0106: return tr(L"Photometric interpretation");
    case 0x010E: return tr(L"Image description");
    case 0x010F: return tr(L"Make");
    case 0x0110: return tr(L"Model");
    case 0x0111: return tr(L"Strip offsets");
    case 0x0112: return tr(L"Orientation");
    case 0x0115: return tr(L"Samples per pixel");
    case 0x0116: return tr(L"Rows per strip");
    case 0x0117: return tr(L"Strip byte counts");
    case 0x011A: return tr(L"X resolution");
    case 0x011B: return tr(L"Y resolution");
    case 0x011C: return tr(L"Planar configuration");
    case 0x0128: return tr(L"Resolution unit");
    case 0x0131: return tr(L"Software");
    case 0x0132: return tr(L"Date modified");
    case 0x013B: return tr(L"Artist");
    case 0x013E: return tr(L"White point");
    case 0x013F: return tr(L"Primary chromaticities");
    case 0x0201: return tr(L"JPEG thumbnail offset");
    case 0x0202: return tr(L"JPEG thumbnail length");
    case 0x0211: return tr(L"YCbCr coefficients");
    case 0x0212: return tr(L"YCbCr subsampling");
    case 0x0213: return tr(L"YCbCr positioning");
    case 0x0214: return tr(L"Reference black/white");
    case 0x8298: return tr(L"Copyright");
    case 0x8769: return L"Exif IFD";
    case 0x8825: return L"GPS IFD";
    case 0x829A: return tr(L"Exposure time");
    case 0x829D: return tr(L"F-number");
    case 0x8822: return tr(L"Exposure program");
    case 0x8827: return tr(L"ISO speed");
    case 0x9000: return tr(L"Exif version");
    case 0x9003: return tr(L"Date taken");
    case 0x9004: return tr(L"Date digitized");
    case 0x9010: return tr(L"Time zone");
    case 0x9011: return tr(L"Time zone (taken)");
    case 0x9101: return tr(L"Components configuration");
    case 0x9102: return tr(L"Compressed bits per pixel");
    case 0x9201: return tr(L"Shutter speed");
    case 0x9202: return tr(L"Aperture");
    case 0x9203: return tr(L"Brightness");
    case 0x9204: return tr(L"Exposure bias");
    case 0x9205: return tr(L"Max aperture");
    case 0x9206: return tr(L"Subject distance");
    case 0x9207: return tr(L"Metering mode");
    case 0x9208: return tr(L"Light source");
    case 0x9209: return tr(L"Flash");
    case 0x920A: return tr(L"Focal length");
    case 0x927C: return tr(L"Maker note");
    case 0x9286: return tr(L"User comment");
    case 0x9290: return tr(L"Subsecond time");
    case 0x9291: return tr(L"Subsecond time (taken)");
    case 0x9292: return tr(L"Subsecond time (digitized)");
    case 0xA000: return tr(L"FlashPix version");
    case 0xA001: return tr(L"Color space");
    case 0xA002: return tr(L"Pixel X dimension");
    case 0xA003: return tr(L"Pixel Y dimension");
    case 0xA005: return tr(L"Interoperability IFD");
    case 0xA20E: return tr(L"Focal plane X resolution");
    case 0xA20F: return tr(L"Focal plane Y resolution");
    case 0xA210: return tr(L"Focal plane resolution unit");
    case 0xA217: return tr(L"Sensing method");
    case 0xA300: return tr(L"File source");
    case 0xA301: return tr(L"Scene type");
    case 0xA401: return tr(L"Custom rendered");
    case 0xA402: return tr(L"Exposure mode");
    case 0xA403: return tr(L"White balance");
    case 0xA404: return tr(L"Digital zoom ratio");
    case 0xA405: return tr(L"Focal length (35mm)");
    case 0xA406: return tr(L"Scene capture type");
    case 0xA408: return tr(L"Contrast");
    case 0xA409: return tr(L"Saturation");
    case 0xA40A: return tr(L"Sharpness");
    case 0xA420: return tr(L"Image unique ID");
    case 0xA430: return tr(L"Camera owner");
    case 0xA431: return tr(L"Body serial number");
    case 0xA432: return tr(L"Lens specification");
    case 0xA433: return tr(L"Lens make");
    case 0xA434: return tr(L"Lens model");
    case 0xA435: return tr(L"Lens serial number");
    default: return L"";
    }
}

std::wstring formatFileSize(unsigned long long bytes) {
    wchar_t buf[64];
    if (bytes < 1024) {
        swprintf(buf, 64, tr(L"%llu bytes"), bytes);
    } else if (bytes < 1024ull * 1024) {
        swprintf(buf, 64, L"%.1f KB", bytes / 1024.0);
    } else if (bytes < 1024ull * 1024 * 1024) {
        swprintf(buf, 64, L"%.1f MB", bytes / (1024.0 * 1024));
    } else {
        swprintf(buf, 64, L"%.2f GB", bytes / (1024.0 * 1024 * 1024));
    }
    return buf;
}

} // namespace mp
