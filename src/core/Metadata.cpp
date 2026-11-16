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
        // 位移與長度都來自檔案，可能是任何值：用 size_t 算，而且只做減法，32 位元相加會繞回去。
        size_t off = be32(t + 4), len = be32(t + 8);
        if (len < 12 || off > size || len > size - off) return L"";
        const unsigned char* d = data + off;
        if (std::memcmp(d, "desc", 4) == 0) {
            // v2：textDescriptionType，ASCII 長度在 +8，字串在 +12（含結尾 0）。
            size_t n = be32(d + 8);
            if (n == 0 || n > len - 12) return L"";
            std::wstring s;
            for (size_t k = 0; k < n && d[12 + k]; ++k) s += static_cast<wchar_t>(d[12 + k]);
            return s;
        }
        if (std::memcmp(d, "mluc", 4) == 0) {
            // v4：multiLocalizedUnicodeType，取第一筆紀錄，UTF-16BE。
            // +8 筆數、+12 每筆的長度（規格固定 12）、+16 起是第一筆：語言與國碼 4 bytes、字串長度、字串位移。
            if (len < 28) return L"";
            size_t records = be32(d + 8), recLen = be32(d + 12);
            size_t strLen = be32(d + 20), strOff = be32(d + 24);
            if (records == 0 || recLen < 12 || strOff > len || strLen > len - strOff) return L"";
            std::wstring s;
            for (size_t k = 0; k + 1 < strLen; k += 2) {
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

const wchar_t* colorModelName(ColorModel model) {
    switch (model) {
    case ColorModel::Rgb: return L"RGB";
    case ColorModel::Grayscale: return trKey(L"Grayscale");
    case ColorModel::Indexed: return trKey(L"Indexed");
    case ColorModel::Cmyk: return L"CMYK";
    }
    return L"";
}

const wchar_t* metaSectionName(MetaSectionId id) {
    switch (id) {
    case MetaSectionId::Tiff: return L"TIFF";
    case MetaSectionId::TiffSubIfd: return trKey(L"TIFF (sub-IFD)");
    case MetaSectionId::Exif: return L"Exif";
    case MetaSectionId::Gps: return L"GPS";
    case MetaSectionId::Interop: return trKey(L"Interoperability");
    case MetaSectionId::Xmp: return L"XMP";
    case MetaSectionId::Iptc: return L"IPTC";
    case MetaSectionId::Photoshop: return L"Photoshop";
    case MetaSectionId::Jfif: return L"JFIF";
    case MetaSectionId::Png: return L"PNG";
    case MetaSectionId::Other: return trKey(L"Other");
    }
    return L"";
}

const wchar_t* exifTagName(unsigned tag) {
    switch (tag) {
    case 0x0100: return trKey(L"Image width");
    case 0x0101: return trKey(L"Image height");
    case 0x0102: return trKey(L"Bits per sample");
    case 0x0103: return trKey(L"Compression");
    case 0x0106: return trKey(L"Photometric interpretation");
    case 0x010E: return trKey(L"Image description");
    case 0x010F: return trKey(L"Make");
    case 0x0110: return trKey(L"Model");
    case 0x0111: return trKey(L"Strip offsets");
    case 0x0112: return trKey(L"Orientation");
    case 0x0115: return trKey(L"Samples per pixel");
    case 0x0116: return trKey(L"Rows per strip");
    case 0x0117: return trKey(L"Strip byte counts");
    case 0x011A: return trKey(L"X resolution");
    case 0x011B: return trKey(L"Y resolution");
    case 0x011C: return trKey(L"Planar configuration");
    case 0x0128: return trKey(L"Resolution unit");
    case 0x0131: return trKey(L"Software");
    case 0x0132: return trKey(L"Date modified");
    case 0x013B: return trKey(L"Artist");
    case 0x013E: return trKey(L"White point");
    case 0x013F: return trKey(L"Primary chromaticities");
    case 0x0201: return trKey(L"JPEG thumbnail offset");
    case 0x0202: return trKey(L"JPEG thumbnail length");
    case 0x0211: return trKey(L"YCbCr coefficients");
    case 0x0212: return trKey(L"YCbCr subsampling");
    case 0x0213: return trKey(L"YCbCr positioning");
    case 0x0214: return trKey(L"Reference black/white");
    case 0x8298: return trKey(L"Copyright");
    case 0x8769: return L"Exif IFD";
    case 0x8825: return L"GPS IFD";
    case 0x829A: return trKey(L"Exposure time");
    case 0x829D: return trKey(L"F-number");
    case 0x8822: return trKey(L"Exposure program");
    case 0x8827: return trKey(L"ISO speed");
    case 0x9000: return trKey(L"Exif version");
    case 0x9003: return trKey(L"Date taken");
    case 0x9004: return trKey(L"Date digitized");
    case 0x9010: return trKey(L"Time zone");
    case 0x9011: return trKey(L"Time zone (taken)");
    case 0x9101: return trKey(L"Components configuration");
    case 0x9102: return trKey(L"Compressed bits per pixel");
    case 0x9201: return trKey(L"Shutter speed");
    case 0x9202: return trKey(L"Aperture");
    case 0x9203: return trKey(L"Brightness");
    case 0x9204: return trKey(L"Exposure bias");
    case 0x9205: return trKey(L"Max aperture");
    case 0x9206: return trKey(L"Subject distance");
    case 0x9207: return trKey(L"Metering mode");
    case 0x9208: return trKey(L"Light source");
    case 0x9209: return trKey(L"Flash");
    case 0x920A: return trKey(L"Focal length");
    case 0x927C: return trKey(L"Maker note");
    case 0x9286: return trKey(L"User comment");
    case 0x9290: return trKey(L"Subsecond time");
    case 0x9291: return trKey(L"Subsecond time (taken)");
    case 0x9292: return trKey(L"Subsecond time (digitized)");
    case 0xA000: return trKey(L"FlashPix version");
    case 0xA001: return trKey(L"Color space");
    case 0xA002: return trKey(L"Pixel X dimension");
    case 0xA003: return trKey(L"Pixel Y dimension");
    case 0xA005: return trKey(L"Interoperability IFD");
    case 0xA20E: return trKey(L"Focal plane X resolution");
    case 0xA20F: return trKey(L"Focal plane Y resolution");
    case 0xA210: return trKey(L"Focal plane resolution unit");
    case 0xA217: return trKey(L"Sensing method");
    case 0xA300: return trKey(L"File source");
    case 0xA301: return trKey(L"Scene type");
    case 0xA401: return trKey(L"Custom rendered");
    case 0xA402: return trKey(L"Exposure mode");
    case 0xA403: return trKey(L"White balance");
    case 0xA404: return trKey(L"Digital zoom ratio");
    case 0xA405: return trKey(L"Focal length (35mm)");
    case 0xA406: return trKey(L"Scene capture type");
    case 0xA408: return trKey(L"Contrast");
    case 0xA409: return trKey(L"Saturation");
    case 0xA40A: return trKey(L"Sharpness");
    case 0xA420: return trKey(L"Image unique ID");
    case 0xA430: return trKey(L"Camera owner");
    case 0xA431: return trKey(L"Body serial number");
    case 0xA432: return trKey(L"Lens specification");
    case 0xA433: return trKey(L"Lens make");
    case 0xA434: return trKey(L"Lens model");
    case 0xA435: return trKey(L"Lens serial number");
    default: return nullptr;
    }
}

int exifOrientation(const unsigned char* t, size_t size) {
    if (!t || size < 8) return 1;
    const bool le = t[0] == 'I' && t[1] == 'I';
    if (!le && !(t[0] == 'M' && t[1] == 'M')) return 1;
    auto rd16 = [&](size_t o) -> unsigned { return le ? (t[o] | (t[o + 1] << 8)) : ((t[o] << 8) | t[o + 1]); };
    auto rd32 = [&](size_t o) -> size_t {
        return le ? (t[o] | (t[o + 1] << 8) | (t[o + 2] << 16) | (static_cast<size_t>(t[o + 3]) << 24))
                  : ((static_cast<size_t>(t[o]) << 24) | (t[o + 1] << 16) | (t[o + 2] << 8) | t[o + 3]);
    };
    if (rd16(2) != 42) return 1;
    // 位移來自檔案：用 size_t 算，而且只做減法。
    size_t ifd0 = rd32(4);
    if (ifd0 < 8 || ifd0 > size || size - ifd0 < 2) return 1;
    size_t n = rd16(ifd0);
    if (n * 12 > size - ifd0 - 2) return 1;
    for (size_t i = 0; i < n; ++i) {
        size_t e = ifd0 + 2 + i * 12;
        if (rd16(e) != 0x0112) continue;
        unsigned type = rd16(e + 2);
        size_t v = type == 3 ? rd16(e + 8) : type == 4 ? rd32(e + 8) : 0;
        return v >= 1 && v <= 8 ? static_cast<int>(v) : 1;
    }
    return 1;
}

} // namespace mp
