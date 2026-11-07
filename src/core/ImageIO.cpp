#include "core/ImageIO.h"

#include <windows.h>
#include <wincodec.h>
#include <wincodecsdk.h>
#include <wrl/client.h>
#include <propvarutil.h>

#include <webp/decode.h>
#include <webp/demux.h>
#include <webp/encode.h>
#include <webp/mux.h>
#include <shlwapi.h>

#include <cstdio>
#include <fstream>
#include <vector>

#include "core/FolderNav.h"
#include "core/ImageOps.h"
#include "core/Lang.h"

using Microsoft::WRL::ComPtr;

namespace mp {

namespace {

// COM 初始化，離開範圍時還原。
struct ComInit {
    HRESULT hr;
    ComInit() : hr(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED)) {}
    ~ComInit() {
        if (SUCCEEDED(hr)) CoUninitialize();
    }
};

std::wstring hrText(const wchar_t* what, HRESULT hr) {
    wchar_t buf[128];
    swprintf(buf, 128, tr(L"%s (HRESULT 0x%08X)"), what, static_cast<unsigned>(hr));
    return buf;
}

ComPtr<IWICImagingFactory> makeFactory() {
    ComPtr<IWICImagingFactory> f;
    CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&f));
    return f;
}

// ---- 像素格式描述 ----

bool isGuid(const GUID& a, const GUID& b) { return IsEqualGUID(a, b) != FALSE; }

std::wstring colorModelOf(const GUID& fmt) {
    if (isGuid(fmt, GUID_WICPixelFormatBlackWhite) || isGuid(fmt, GUID_WICPixelFormat2bppGray) ||
        isGuid(fmt, GUID_WICPixelFormat4bppGray) || isGuid(fmt, GUID_WICPixelFormat8bppGray) ||
        isGuid(fmt, GUID_WICPixelFormat16bppGray) || isGuid(fmt, GUID_WICPixelFormat16bppGrayFixedPoint) ||
        isGuid(fmt, GUID_WICPixelFormat16bppGrayHalf) || isGuid(fmt, GUID_WICPixelFormat32bppGrayFloat) ||
        isGuid(fmt, GUID_WICPixelFormat32bppGrayFixedPoint))
        return tr(L"Grayscale");
    if (isGuid(fmt, GUID_WICPixelFormat1bppIndexed) || isGuid(fmt, GUID_WICPixelFormat2bppIndexed) ||
        isGuid(fmt, GUID_WICPixelFormat4bppIndexed) || isGuid(fmt, GUID_WICPixelFormat8bppIndexed))
        return tr(L"Indexed");
    if (isGuid(fmt, GUID_WICPixelFormat32bppCMYK) || isGuid(fmt, GUID_WICPixelFormat64bppCMYK) ||
        isGuid(fmt, GUID_WICPixelFormat40bppCMYKAlpha) || isGuid(fmt, GUID_WICPixelFormat80bppCMYKAlpha))
        return L"CMYK";
    return L"RGB";
}

int bitsPerPixelOf(IWICImagingFactory* factory, const GUID& fmt) {
    ComPtr<IWICComponentInfo> ci;
    if (FAILED(factory->CreateComponentInfo(fmt, &ci))) return 0;
    ComPtr<IWICPixelFormatInfo> pi;
    if (FAILED(ci.As(&pi))) return 0;
    UINT bpp = 0;
    pi->GetBitsPerPixel(&bpp);
    return static_cast<int>(bpp);
}

std::wstring formatNameOf(const GUID& container) {
    if (isGuid(container, GUID_ContainerFormatJpeg)) return tr(L"JPEG image");
    if (isGuid(container, GUID_ContainerFormatPng)) return tr(L"PNG image");
    if (isGuid(container, GUID_ContainerFormatBmp)) return tr(L"BMP image");
    if (isGuid(container, GUID_ContainerFormatTiff)) return tr(L"TIFF image");
    if (isGuid(container, GUID_ContainerFormatGif)) return tr(L"GIF image");
    return tr(L"Image");
}

// ---- metadata ----

std::wstring jfifTagName(unsigned tag) {
    switch (tag) {
    case 0: return tr(L"JFIF version");
    case 1: return tr(L"Density unit");
    case 2: return tr(L"X density");
    case 3: return tr(L"Y density");
    case 4: return tr(L"Thumbnail width");
    case 5: return tr(L"Thumbnail height");
    case 6: return tr(L"Thumbnail");
    default: return L"";
    }
}

std::wstring sectionNameOf(const GUID& fmt, bool* isContainerOnly) {
    *isContainerOnly = false;
    if (isGuid(fmt, GUID_MetadataFormatApp1) || isGuid(fmt, GUID_MetadataFormatApp13)) {
        *isContainerOnly = true;
        return L"";
    }
    if (isGuid(fmt, GUID_MetadataFormatIfd)) return L"TIFF";
    if (isGuid(fmt, GUID_MetadataFormatSubIfd)) return tr(L"TIFF (sub-IFD)");
    if (isGuid(fmt, GUID_MetadataFormatExif)) return L"Exif";
    if (isGuid(fmt, GUID_MetadataFormatGps)) return L"GPS";
    if (isGuid(fmt, GUID_MetadataFormatInterop)) return tr(L"Interoperability");
    if (isGuid(fmt, GUID_MetadataFormatXMP) || isGuid(fmt, GUID_MetadataFormatXMPStruct) ||
        isGuid(fmt, GUID_MetadataFormatXMPBag) || isGuid(fmt, GUID_MetadataFormatXMPSeq) ||
        isGuid(fmt, GUID_MetadataFormatXMPAlt))
        return L"XMP";
    if (isGuid(fmt, GUID_MetadataFormatIPTC)) return L"IPTC";
    if (isGuid(fmt, GUID_MetadataFormatIRB)) return L"Photoshop";
    if (isGuid(fmt, GUID_MetadataFormatApp0)) return L"JFIF";
    if (isGuid(fmt, GUID_MetadataFormatChunktEXt) || isGuid(fmt, GUID_MetadataFormatChunkiTXt) ||
        isGuid(fmt, GUID_MetadataFormatChunkgAMA) || isGuid(fmt, GUID_MetadataFormatChunkcHRM) ||
        isGuid(fmt, GUID_MetadataFormatChunksRGB) ||
        isGuid(fmt, GUID_MetadataFormatChunktIME) || isGuid(fmt, GUID_MetadataFormatChunkbKGD) ||
        isGuid(fmt, GUID_MetadataFormatChunkhIST) || isGuid(fmt, GUID_MetadataFormatChunkiCCP))
        return L"PNG";
    return tr(L"Other");
}

std::wstring chunkPrefixOf(const GUID& fmt) {
    if (isGuid(fmt, GUID_MetadataFormatChunkgAMA)) return L"gAMA ";
    if (isGuid(fmt, GUID_MetadataFormatChunkcHRM)) return L"cHRM ";
    if (isGuid(fmt, GUID_MetadataFormatChunksRGB)) return L"sRGB ";
    if (isGuid(fmt, GUID_MetadataFormatChunktIME)) return L"tIME ";
    if (isGuid(fmt, GUID_MetadataFormatChunkbKGD)) return L"bKGD ";
    if (isGuid(fmt, GUID_MetadataFormatChunkiCCP)) return L"iCCP ";
    return L"";
}

std::wstring toW(const char* s) {
    if (!s) return L"";
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, nullptr, 0);
    if (n <= 1) {
        n = MultiByteToWideChar(CP_ACP, 0, s, -1, nullptr, 0);
        if (n <= 1) return L"";
        std::wstring w(n - 1, L'\0');
        MultiByteToWideChar(CP_ACP, 0, s, -1, w.data(), n);
        return w;
    }
    std::wstring w(n - 1, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s, -1, w.data(), n);
    return w;
}

std::wstring scalarToString(const PROPVARIANT& v) {
    wchar_t buf[64];
    switch (v.vt) {
    case VT_UI1: swprintf(buf, 64, L"%u", v.bVal); return buf;
    case VT_I1: swprintf(buf, 64, L"%d", v.cVal); return buf;
    case VT_UI2: swprintf(buf, 64, L"%u", v.uiVal); return buf;
    case VT_I2: swprintf(buf, 64, L"%d", v.iVal); return buf;
    case VT_UI4: swprintf(buf, 64, L"%u", v.ulVal); return buf;
    case VT_I4: swprintf(buf, 64, L"%d", v.lVal); return buf;
    case VT_UI8: {
        // Exif 的 unsigned rational：低 32 位分子，高 32 位分母。
        unsigned num = v.uhVal.LowPart, den = v.uhVal.HighPart;
        if (den == 0) { swprintf(buf, 64, L"%llu", v.uhVal.QuadPart); return buf; }
        if (den == 1) { swprintf(buf, 64, L"%u", num); return buf; }
        swprintf(buf, 64, L"%u/%u (%.4g)", num, den, static_cast<double>(num) / den);
        return buf;
    }
    case VT_I8: {
        int num = v.hVal.LowPart, den = v.hVal.HighPart;
        if (den == 0) { swprintf(buf, 64, L"%lld", v.hVal.QuadPart); return buf; }
        if (den == 1) { swprintf(buf, 64, L"%d", num); return buf; }
        swprintf(buf, 64, L"%d/%d (%.4g)", num, den, static_cast<double>(num) / den);
        return buf;
    }
    case VT_R4: swprintf(buf, 64, L"%.4g", v.fltVal); return buf;
    case VT_R8: swprintf(buf, 64, L"%.6g", v.dblVal); return buf;
    case VT_BOOL: return v.boolVal ? tr(L"Yes") : tr(L"No");
    case VT_LPSTR: return toW(v.pszVal);
    case VT_LPWSTR: return v.pwszVal ? v.pwszVal : L"";
    case VT_BSTR: return v.bstrVal ? v.bstrVal : L"";
    case VT_EMPTY: case VT_NULL: return L"";
    default: swprintf(buf, 64, tr(L"(type %u)"), v.vt); return buf;
    }
}

bool isRationalTag(unsigned tag) {
    switch (tag) {
    case 0x011A: case 0x011B: case 0x829A: case 0x829D: case 0x9201: case 0x9202: case 0x9203: case 0x9204:
    case 0x9205: case 0x9206: case 0x920A: case 0xA20E: case 0xA20F: case 0xA404:
        return true;
    default: return false;
    }
}

std::wstring valueToString(const PROPVARIANT& v, unsigned tag) {
    wchar_t buf[64];
    if (v.vt == VT_BLOB) { swprintf(buf, 64, tr(L"(%lu bytes)"), v.blob.cbSize); return buf; }
    if (!(v.vt & VT_VECTOR)) return scalarToString(v);
    VARTYPE et = v.vt & VT_TYPEMASK;
    ULONG n = v.caub.cElems; // 所有 vector 的 cElems 位置相同
    // 有些寫入器把 rational 存成兩個整數。
    if (n == 2 && (et == VT_UI4 || et == VT_I4) && isRationalTag(tag)) {
        double num = et == VT_UI4 ? v.caul.pElems[0] : v.cal.pElems[0];
        double den = et == VT_UI4 ? v.caul.pElems[1] : v.cal.pElems[1];
        if (den == 1) { swprintf(buf, 64, L"%.0f", num); return buf; }
        if (den != 0) { swprintf(buf, 64, L"%.0f/%.0f (%.4g)", num, den, num / den); return buf; }
    }
    if (et == VT_UI1) {
        if (n > 32) { swprintf(buf, 64, tr(L"(%lu bytes)"), n); return buf; }
        // 短的 byte 陣列多半是 ASCII 版本字串，例如 Exif 版本 "0230"。
        bool printable = n > 0;
        for (ULONG i = 0; i < n; ++i)
            if (v.caub.pElems[i] < 32 || v.caub.pElems[i] > 126) { printable = false; break; }
        if (printable) {
            std::wstring s;
            for (ULONG i = 0; i < n; ++i) s += static_cast<wchar_t>(v.caub.pElems[i]);
            return s;
        }
    }
    std::wstring out;
    ULONG shown = std::min<ULONG>(n, 8);
    for (ULONG i = 0; i < shown; ++i) {
        PROPVARIANT e;
        PropVariantInit(&e);
        if (FAILED(PropVariantGetElem(v, i, &e))) break;
        if (i) out += L", ";
        out += scalarToString(e);
        PropVariantClear(&e);
    }
    if (n > shown) out += L", …";
    return out;
}

// 回傳索引而不是參照：走訪子區塊時 sections 會 push_back 重新配置，拿著參照或指標會變成懸空。
size_t sectionNamed(std::vector<MetaSection>& sections, const std::wstring& name) {
    for (size_t i = 0; i < sections.size(); ++i)
        if (sections[i].name == name) return i;
    sections.push_back({name, {}});
    return sections.size() - 1;
}

void walkReader(IWICMetadataReader* reader, std::vector<MetaSection>& sections, ImageInfo& info, int depth);

constexpr size_t kNoSection = static_cast<size_t>(-1);

void addItem(IWICMetadataReader* reader, const GUID& fmt, size_t sectionIdx, std::vector<MetaSection>& sections,
             ImageInfo& info, int depth, UINT i) {
    PROPVARIANT schema, id, value;
    PropVariantInit(&schema);
    PropVariantInit(&id);
    PropVariantInit(&value);
    if (SUCCEEDED(reader->GetValueByIndex(i, &schema, &id, &value))) {
        if (value.vt == VT_UNKNOWN && value.punkVal) {
            ComPtr<IWICMetadataReader> child;
            if (SUCCEEDED(value.punkVal->QueryInterface(IID_PPV_ARGS(&child)))) walkReader(child.Get(), sections, info, depth + 1);
        } else if (sectionIdx != kNoSection) {
            MetaSection* section = &sections[sectionIdx];
            std::wstring name;
            unsigned tag = 0xFFFFFFFF;
            if (id.vt == VT_UI2 || id.vt == VT_UI4 || id.vt == VT_I4) {
                tag = id.vt == VT_UI2 ? id.uiVal : static_cast<unsigned>(id.ulVal);
                const bool jfif = isGuid(fmt, GUID_MetadataFormatApp0);
                if (!jfif && tag == 0x0112 && (value.vt == VT_UI2 || value.vt == VT_UI4)) {
                    info.orientation = value.vt == VT_UI2 ? value.uiVal : static_cast<int>(value.ulVal);
                }
                name = jfif ? jfifTagName(tag) : exifTagName(tag);
                if (jfif && tag == 0) { // 版本是 ushort，例如 0x0101 -> 1.01
                    unsigned ver = value.vt == VT_UI2 ? value.uiVal : 0;
                    wchar_t vb[16];
                    swprintf(vb, 16, L"%u.%02u", ver >> 8, ver & 0xFF);
                    section->items.emplace_back(name, vb);
                    PropVariantClear(&schema); PropVariantClear(&id); PropVariantClear(&value);
                    return;
                }
                if (name.empty()) {
                    wchar_t buf[32];
                    swprintf(buf, 32, L"Tag 0x%04X", tag);
                    name = buf;
                }
                if (tag == 0x927C) { // MakerNote 不展開
                    wchar_t buf[48];
                    swprintf(buf, 48, tr(L"(%lu bytes)"), (value.vt & VT_VECTOR) ? value.caub.cElems : 0ul);
                    section->items.emplace_back(name, buf);
                    PropVariantClear(&schema); PropVariantClear(&id); PropVariantClear(&value);
                    return;
                }
            } else {
                name = chunkPrefixOf(fmt) + scalarToString(id);
            }
            std::wstring text = valueToString(value, tag);
            if (!text.empty()) section->items.emplace_back(name, text);
        }
    }
    PropVariantClear(&schema);
    PropVariantClear(&id);
    PropVariantClear(&value);
}

void walkReader(IWICMetadataReader* reader, std::vector<MetaSection>& sections, ImageInfo& info, int depth) {
    if (depth > 6) return;
    GUID fmt{};
    reader->GetMetadataFormat(&fmt);
    bool containerOnly = false;
    std::wstring name = sectionNameOf(fmt, &containerOnly);
    UINT count = 0;
    reader->GetCount(&count);
    size_t sectionIdx = containerOnly ? kNoSection : sectionNamed(sections, name);
    for (UINT i = 0; i < count; ++i) addItem(reader, fmt, sectionIdx, sections, info, depth, i);
}

void readMetadata(IWICBitmapFrameDecode* frame, ImageInfo& info) {
    ComPtr<IWICMetadataBlockReader> block;
    if (FAILED(frame->QueryInterface(IID_PPV_ARGS(&block)))) return;
    UINT count = 0;
    if (FAILED(block->GetCount(&count))) return;
    for (UINT i = 0; i < count; ++i) {
        ComPtr<IWICMetadataReader> reader;
        if (SUCCEEDED(block->GetReaderByIndex(i, &reader))) walkReader(reader.Get(), info.sections, info, 0);
    }
    // 空區塊不顯示。
    std::vector<MetaSection> kept;
    for (auto& s : info.sections)
        if (!s.items.empty()) kept.push_back(std::move(s));
    info.sections = std::move(kept);
}

void readColorProfile(IWICImagingFactory* factory, IWICBitmapFrameDecode* frame, ImageInfo& info) {
    UINT count = 0;
    if (FAILED(frame->GetColorContexts(0, nullptr, &count)) || count == 0) return;
    std::vector<ComPtr<IWICColorContext>> ctxs(count);
    std::vector<IWICColorContext*> raw(count);
    for (UINT i = 0; i < count; ++i) {
        if (FAILED(factory->CreateColorContext(&ctxs[i]))) return;
        raw[i] = ctxs[i].Get();
    }
    if (FAILED(frame->GetColorContexts(count, raw.data(), &count))) return;
    for (UINT i = 0; i < count; ++i) {
        WICColorContextType type;
        if (FAILED(ctxs[i]->GetType(&type))) continue;
        if (type == WICColorContextProfile) {
            UINT size = 0;
            ctxs[i]->GetProfileBytes(0, nullptr, &size);
            if (size == 0) continue;
            std::vector<BYTE> bytes(size);
            if (SUCCEEDED(ctxs[i]->GetProfileBytes(size, bytes.data(), &size))) {
                info.colorProfile = iccProfileDescription(bytes.data(), size);
                if (!info.colorProfile.empty()) return;
            }
        } else if (type == WICColorContextExifColorSpace) {
            UINT cs = 0;
            if (SUCCEEDED(ctxs[i]->GetExifColorSpace(&cs))) {
                if (cs == 1) info.colorProfile = L"sRGB IEC61966-2.1";
                else if (cs == 2) info.colorProfile = L"Adobe RGB (1998)";
            }
        }
    }
}

LoadResult loadWithWic(const std::wstring& path) {
    LoadResult r;
    auto factory = makeFactory();
    if (!factory) { r.error = tr(L"Could not create the WIC factory"); return r; }

    ComPtr<IWICBitmapDecoder> decoder;
    HRESULT hr = factory->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ,
                                                    WICDecodeMetadataCacheOnDemand, &decoder);
    if (FAILED(hr)) { r.error = hrText(tr(L"Could not decode this file"), hr); return r; }

    GUID container{};
    decoder->GetContainerFormat(&container);
    r.info.formatName = formatNameOf(container);

    ComPtr<IWICBitmapFrameDecode> frame;
    hr = decoder->GetFrame(0, &frame);
    if (FAILED(hr)) { r.error = hrText(tr(L"Could not read the first frame"), hr); return r; }

    GUID fmt{};
    frame->GetPixelFormat(&fmt);
    r.info.colorModel = colorModelOf(fmt);
    r.info.bitsPerPixel = bitsPerPixelOf(factory.Get(), fmt);

    UINT w = 0, h = 0;
    frame->GetSize(&w, &h);
    if (w == 0 || h == 0) { r.error = tr(L"Image size is 0"); return r; }

    auto img = std::make_shared<Image>(static_cast<int>(w), static_cast<int>(h));
    frame->GetResolution(&img->dpiX, &img->dpiY);
    if (img->dpiX <= 0) img->dpiX = 96;
    if (img->dpiY <= 0) img->dpiY = 96;

    ComPtr<IWICFormatConverter> conv;
    hr = factory->CreateFormatConverter(&conv);
    if (SUCCEEDED(hr))
        hr = conv->Initialize(frame.Get(), GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0.0,
                              WICBitmapPaletteTypeMedianCut);
    if (FAILED(hr)) { r.error = hrText(tr(L"Could not convert the pixel format"), hr); return r; }
    hr = conv->CopyPixels(nullptr, static_cast<UINT>(img->stride()), static_cast<UINT>(img->pixels.size()),
                          img->pixels.data());
    if (FAILED(hr)) { r.error = hrText(tr(L"Reading pixels failed"), hr); return r; }

    readMetadata(frame.Get(), r.info);
    readColorProfile(factory.Get(), frame.Get(), r.info);

    if (r.info.orientation >= 2 && r.info.orientation <= 8) {
        img = std::make_shared<Image>(applyExifOrientation(*img, r.info.orientation));
    }
    r.image = img;
    return r;
}

// ---- metadata 搬運 ----

struct WebPChunks {
    std::vector<uint8_t> exif; // TIFF 結構（已去掉 "Exif\0\0" 前綴）
    std::vector<uint8_t> xmp;
    std::vector<uint8_t> icc;
};

bool readFileBytes(const std::wstring& path, std::vector<uint8_t>& out);
bool isWebPFile(const std::wstring& path);

std::vector<uint8_t> stripExifPrefix(const uint8_t* p, size_t n) {
    if (n >= 6 && memcmp(p, "Exif\0\0", 6) == 0) return std::vector<uint8_t>(p + 6, p + n);
    return std::vector<uint8_t>(p, p + n);
}

WebPChunks readWebPChunks(const std::vector<uint8_t>& bytes) {
    WebPChunks out;
    WebPData data{bytes.data(), bytes.size()};
    WebPDemuxer* d = WebPDemux(&data);
    if (!d) return out;
    auto grab = [&](const char* fourcc, std::vector<uint8_t>& dst, bool exif) {
        WebPChunkIterator it;
        if (WebPDemuxGetChunk(d, fourcc, 1, &it)) {
            dst = exif ? stripExifPrefix(it.chunk.bytes, it.chunk.size) : std::vector<uint8_t>(it.chunk.bytes, it.chunk.bytes + it.chunk.size);
            WebPDemuxReleaseChunkIterator(&it);
        }
    };
    grab("EXIF", out.exif, true);
    grab("XMP ", out.xmp, false);
    grab("ICCP", out.icc, false);
    WebPDemuxDelete(d);
    return out;
}

ComPtr<IStream> memStream(const std::vector<uint8_t>& bytes) {
    ComPtr<IStream> st;
    st.Attach(SHCreateMemStream(bytes.data(), static_cast<UINT>(bytes.size())));
    return st;
}

// 從一段位元組建 WIC metadata reader（XMP 給 XML、IFD 給 TIFF 結構等）。
ComPtr<IWICMetadataReader> metadataReaderFromBytes(IWICImagingFactory* factory, const GUID& format, const std::vector<uint8_t>& bytes) {
    ComPtr<IWICMetadataReader> reader;
    if (!factory || bytes.empty()) return reader;
    ComPtr<IWICComponentFactory> cf;
    if (FAILED(factory->QueryInterface(IID_PPV_ARGS(&cf)))) return reader;
    auto st = memStream(bytes);
    if (!st) return reader;
    if (FAILED(cf->CreateMetadataReader(format, nullptr, WICPersistOptionDefault, st.Get(), &reader))) reader.Reset();
    return reader;
}

// EXIF（TIFF 結構）-> App1 reader（內容是 "Exif\0\0" + TIFF），走 JPEG 一樣的解析。
ComPtr<IWICMetadataReader> app1ReaderFromExif(IWICImagingFactory* factory, const std::vector<uint8_t>& tiff) {
    std::vector<uint8_t> app1;
    app1.reserve(tiff.size() + 6);
    app1.insert(app1.end(), {'E', 'x', 'i', 'f', 0, 0});
    app1.insert(app1.end(), tiff.begin(), tiff.end());
    return metadataReaderFromBytes(factory, GUID_MetadataFormatApp1, app1);
}

// 把 IFD 樹（IWICMetadataReader）逐項複製到一個 query writer：一般欄位直接設，
// 子 IFD（Exif / GPS / Interop）各建一個 writer 遞迴。IFD1（縮圖）略過。
void copyIfdTree(IWICComponentFactory* cf, IWICMetadataReader* src, IWICMetadataQueryWriter* dst) {
    UINT count = 0;
    if (!src || !dst || FAILED(src->GetCount(&count))) return;
    for (UINT i = 0; i < count; ++i) {
        PROPVARIANT schema, id, value;
        PropVariantInit(&schema); PropVariantInit(&id); PropVariantInit(&value);
        if (SUCCEEDED(src->GetValueByIndex(i, &schema, &id, &value)) && (id.vt == VT_UI2 || id.vt == VT_UI4)) {
            unsigned tag = id.vt == VT_UI2 ? id.uiVal : static_cast<unsigned>(id.ulVal);
            if (value.vt == VT_UNKNOWN && value.punkVal) {
                ComPtr<IWICMetadataReader> sub;
                if (SUCCEEDED(value.punkVal->QueryInterface(IID_PPV_ARGS(&sub)))) {
                    GUID fmt{};
                    sub->GetMetadataFormat(&fmt);
                    const wchar_t* name = isGuid(fmt, GUID_MetadataFormatExif) ? L"/exif"
                                        : isGuid(fmt, GUID_MetadataFormatGps) ? L"/gps"
                                        : isGuid(fmt, GUID_MetadataFormatInterop) ? L"/interop" : nullptr;
                    ComPtr<IWICMetadataQueryWriter> subW;
                    if (name && SUCCEEDED(cf->CreateQueryWriter(fmt, nullptr, &subW))) {
                        copyIfdTree(cf, sub.Get(), subW.Get());
                        PROPVARIANT v;
                        PropVariantInit(&v);
                        v.vt = VT_UNKNOWN;
                        v.punkVal = subW.Get();
                        dst->SetMetadataByName(name, &v);
                    }
                }
            } else if (tag != 0x8769 && tag != 0x8825 && tag != 0xA005) { // 指標欄位由 writer 自己產生
                wchar_t key[32];
                swprintf(key, 32, L"/{ushort=%u}", tag);
                dst->SetMetadataByName(key, &value);
            }
        }
        PropVariantClear(&schema); PropVariantClear(&id); PropVariantClear(&value);
    }
}

// 來源檔裡可以搬的東西。WIC 來源保留 decoder / frame（同容器整包複製要用），WebP 來源是三個 chunk。
struct SourceMeta {
    bool valid = false;
    bool webp = false;
    GUID container{};
    ComPtr<IWICBitmapDecoder> decoder;
    ComPtr<IWICBitmapFrameDecode> frame;
    WebPChunks chunks;        // WebP 來源的 chunk；WIC 來源時 icc 也會填
    int width = 0, height = 0; // 來源檔裡實際儲存的像素大小（沒套 orientation）
    std::wstring path;
};

std::vector<uint8_t> iccBytesOf(IWICImagingFactory* factory, IWICBitmapFrameDecode* frame) {
    std::vector<uint8_t> out;
    UINT count = 0;
    if (FAILED(frame->GetColorContexts(0, nullptr, &count)) || count == 0) return out;
    std::vector<ComPtr<IWICColorContext>> ctxs(count);
    std::vector<IWICColorContext*> raw(count);
    for (UINT i = 0; i < count; ++i) {
        if (FAILED(factory->CreateColorContext(&ctxs[i]))) return out;
        raw[i] = ctxs[i].Get();
    }
    if (FAILED(frame->GetColorContexts(count, raw.data(), &count))) return out;
    for (UINT i = 0; i < count; ++i) {
        WICColorContextType type;
        if (FAILED(ctxs[i]->GetType(&type)) || type != WICColorContextProfile) continue;
        UINT size = 0;
        ctxs[i]->GetProfileBytes(0, nullptr, &size);
        if (size == 0) continue;
        out.resize(size);
        if (SUCCEEDED(ctxs[i]->GetProfileBytes(size, out.data(), &size))) return out;
        out.clear();
    }
    return out;
}

SourceMeta readSourceMeta(IWICImagingFactory* factory, const std::wstring& path) {
    SourceMeta m;
    if (path.empty()) return m;
    m.path = path;
    if (isWebPFile(path)) {
        std::vector<uint8_t> bytes;
        if (!readFileBytes(path, bytes)) return m;
        m.chunks = readWebPChunks(bytes);
        int w = 0, h = 0;
        if (WebPGetInfo(bytes.data(), bytes.size(), &w, &h)) { m.width = w; m.height = h; }
        m.webp = true;
        m.valid = true;
        return m;
    }
    if (!factory) return m;
    if (FAILED(factory->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &m.decoder))) return m;
    if (FAILED(m.decoder->GetFrame(0, &m.frame))) return m;
    m.decoder->GetContainerFormat(&m.container);
    m.chunks.icc = iccBytesOf(factory, m.frame.Get());
    UINT w = 0, h = 0;
    m.frame->GetSize(&w, &h);
    m.width = static_cast<int>(w);
    m.height = static_cast<int>(h);
    m.valid = true;
    return m;
}

// JPEG 檔裡的 APP1 段：EXIF（"Exif\0\0" 前綴）或 XMP（"http://ns.adobe.com/xap/1.0/\0" 前綴）的原始內容。
// 跨到 WebP 時直接搬這段位元組最保險；WIC 的 reader 序列化不可靠。
std::vector<uint8_t> jpegApp1Payload(const std::vector<uint8_t>& file, const char* prefix, size_t prefixLen) {
    size_t i = 2;
    if (file.size() < 4 || file[0] != 0xFF || file[1] != 0xD8) return {};
    while (i + 4 <= file.size() && file[i] == 0xFF) {
        uint8_t marker = file[i + 1];
        if (marker == 0xD8 || (marker >= 0xD0 && marker <= 0xD7) || marker == 0x01) { i += 2; continue; }
        if (marker == 0xDA || marker == 0xD9) break; // 影像資料開始 / 結束
        size_t len = (static_cast<size_t>(file[i + 2]) << 8) | file[i + 3];
        if (len < 2 || i + 2 + len > file.size()) break;
        if (marker == 0xE1 && len - 2 >= prefixLen && memcmp(&file[i + 4], prefix, prefixLen) == 0)
            return std::vector<uint8_t>(file.begin() + static_cast<long>(i + 4 + prefixLen), file.begin() + static_cast<long>(i + 2 + len));
        i += 2 + len;
    }
    return {};
}

struct SourceBytes {
    std::vector<uint8_t> exif, xmp;
};

// WIC 來源要搬到 WebP 的 EXIF / XMP 位元組。只有 JPEG 來源做得到（掃 APP1 段）。
SourceBytes bytesForWebP(const SourceMeta& m) {
    SourceBytes out;
    if (m.webp) { out.exif = m.chunks.exif; out.xmp = m.chunks.xmp; return out; }
    if (!isGuid(m.container, GUID_ContainerFormatJpeg)) return out;
    std::vector<uint8_t> file;
    if (!readFileBytes(m.path, file)) return out;
    out.exif = jpegApp1Payload(file, "Exif\0\0", 6);
    static const char kXmp[] = "http://ns.adobe.com/xap/1.0/";
    out.xmp = jpegApp1Payload(file, kXmp, sizeof(kXmp)); // sizeof 含結尾的 \0，正好是前綴長度
    return out;
}

// 在編碼器 frame 上套 metadata。frame 已 Initialize，還沒 SetSize。
// 同容器：整包複製；跨容器：盡力搬 EXIF（/ifd）與 XMP；ICC 走 color context。搬不過去的靜默略過。
void applyMetadata(IWICImagingFactory* factory, IWICBitmapFrameEncode* frame, const GUID& target, const SourceMeta& m,
                   const Image& img, bool sizeChanged) {
    if (!m.valid) return;
    const bool toJpeg = isGuid(target, GUID_ContainerFormatJpeg), toTiff = isGuid(target, GUID_ContainerFormatTiff);
    const bool toPng = isGuid(target, GUID_ContainerFormatPng);
    if (isGuid(target, GUID_ContainerFormatBmp)) return;

    bool copied = false;
    if (!m.webp && isGuid(m.container, target)) {
        ComPtr<IWICMetadataBlockWriter> bw;
        ComPtr<IWICMetadataBlockReader> br;
        if (SUCCEEDED(frame->QueryInterface(IID_PPV_ARGS(&bw))) && SUCCEEDED(m.frame->QueryInterface(IID_PPV_ARGS(&br))))
            copied = SUCCEEDED(bw->InitializeFromBlockReader(br.Get()));
    }
    ComPtr<IWICMetadataQueryWriter> qw;
    frame->GetMetadataQueryWriter(&qw);
    if (!copied && qw && (toJpeg || toTiff || toPng)) {
        ComPtr<IWICComponentFactory> cf;
        factory->QueryInterface(IID_PPV_ARGS(&cf));
        ComPtr<IWICMetadataQueryReader> srcQ;
        if (m.frame) m.frame->GetMetadataQueryReader(&srcQ);
        // 子區塊要用 query writer 塞進去：WIC 來源從 query reader 的子區塊轉成 writer；
        // WebP 來源從 EXIF 位元組建 reader，逐項複製到新的 IFD writer。
        auto subtreeWriter = [&](const wchar_t* path) -> ComPtr<IWICMetadataQueryWriter> {
            ComPtr<IWICMetadataQueryWriter> w;
            if (!srcQ) return w;
            PROPVARIANT v;
            PropVariantInit(&v);
            if (SUCCEEDED(srcQ->GetMetadataByName(path, &v)) && v.vt == VT_UNKNOWN && v.punkVal) {
                ComPtr<IWICMetadataQueryReader> sub;
                if (SUCCEEDED(v.punkVal->QueryInterface(IID_PPV_ARGS(&sub)))) factory->CreateQueryWriterFromReader(sub.Get(), nullptr, &w);
            }
            PropVariantClear(&v);
            return w;
        };
        if (toJpeg || toTiff) {
            ComPtr<IWICMetadataQueryWriter> ifdW;
            if (!m.webp) {
                ifdW = subtreeWriter(L"/app1/ifd");
                if (!ifdW) ifdW = subtreeWriter(L"/ifd");
            } else if (!m.chunks.exif.empty() && cf) {
                if (auto app1 = app1ReaderFromExif(factory, m.chunks.exif)) {
                    PROPVARIANT schema, id, value;
                    PropVariantInit(&schema); PropVariantInit(&id); PropVariantInit(&value);
                    ComPtr<IWICMetadataReader> ifdR;
                    if (SUCCEEDED(app1->GetValueByIndex(0, &schema, &id, &value)) && value.vt == VT_UNKNOWN && value.punkVal)
                        value.punkVal->QueryInterface(IID_PPV_ARGS(&ifdR));
                    if (ifdR && SUCCEEDED(cf->CreateQueryWriter(GUID_MetadataFormatIfd, nullptr, &ifdW))) copyIfdTree(cf.Get(), ifdR.Get(), ifdW.Get());
                    PropVariantClear(&schema); PropVariantClear(&id); PropVariantClear(&value);
                }
            }
            if (ifdW) {
                // TIFF 來源的 IFD0 帶著描述像素排列的欄位（StripOffsets、Compression…），
                // 搬進 JPEG 的 APP1 就不對了；XMP（700）與 ICC（34675）另外走自己的路。
                for (unsigned tag : {254u, 255u, 256u, 257u, 258u, 259u, 262u, 263u, 264u, 265u, 266u, 273u, 277u, 278u, 279u,
                                     280u, 281u, 284u, 317u, 320u, 322u, 323u, 324u, 325u, 330u, 338u, 339u, 340u, 341u, 347u,
                                     700u, 34675u})
                    ifdW->RemoveMetadataByName((L"/{ushort=" + std::to_wstring(tag) + L"}").c_str());
                PROPVARIANT v;
                PropVariantInit(&v);
                v.vt = VT_UNKNOWN;
                v.punkVal = ifdW.Get();
                qw->SetMetadataByName(toJpeg ? L"/app1/ifd" : L"/ifd", &v);
            }
        }
        if (!m.webp) {
            // XMP 在 JPEG / PNG 是獨立區塊（/xmp），在 TIFF 是 IFD0 的 700 號欄位（/ifd/xmp）。
            auto xmpW = subtreeWriter(L"/xmp");
            if (!xmpW) xmpW = subtreeWriter(L"/ifd/xmp");
            if (xmpW) {
                PROPVARIANT v;
                PropVariantInit(&v);
                v.vt = VT_UNKNOWN;
                v.punkVal = xmpW.Get();
                qw->SetMetadataByName(toTiff ? L"/ifd/xmp" : L"/xmp", &v);
            }
        }
    }
    // ICC
    if (!m.chunks.icc.empty() && factory) {
        ComPtr<IWICColorContext> cc;
        if (SUCCEEDED(factory->CreateColorContext(&cc)) &&
            SUCCEEDED(cc->InitializeFromMemory(m.chunks.icc.data(), static_cast<UINT>(m.chunks.icc.size())))) {
            IWICColorContext* raw = cc.Get();
            frame->SetColorContexts(1, &raw);
        }
    }
    // 與像素不符的欄位：orientation 一律 1，尺寸變了改 Exif 的 PixelX/YDimension。
    if (qw && (toJpeg || toTiff)) {
        const wchar_t* ifd = toJpeg ? L"/app1/ifd" : L"/ifd";
        PROPVARIANT v;
        PropVariantInit(&v);
        std::wstring key = std::wstring(ifd) + L"/{ushort=274}";
        if (SUCCEEDED(qw->GetMetadataByName(key.c_str(), &v))) {
            PropVariantClear(&v);
            PropVariantInit(&v);
            v.vt = VT_UI2;
            v.uiVal = 1;
            qw->SetMetadataByName(key.c_str(), &v);
        }
        PropVariantClear(&v);
        if (sizeChanged) {
            for (auto [tag, val] : {std::pair{40962u, img.width}, std::pair{40963u, img.height}}) {
                std::wstring k = std::wstring(ifd) + L"/exif/{ushort=" + std::to_wstring(tag) + L"}";
                PROPVARIANT old;
                PropVariantInit(&old);
                if (SUCCEEDED(qw->GetMetadataByName(k.c_str(), &old))) {
                    PROPVARIANT nv;
                    PropVariantInit(&nv);
                    nv.vt = VT_UI4;
                    nv.ulVal = static_cast<ULONG>(val);
                    qw->SetMetadataByName(k.c_str(), &nv);
                }
                PropVariantClear(&old);
            }
        }
        // 內嵌縮圖：舊的丟掉（IFD1），存檔時由 frame->SetThumbnail 重新產生。
        for (const wchar_t* t : {L"/app1/thumb", L"/app1/ifd/thumb", L"/app1/{ushort=1}", L"/ifd/thumb"}) qw->RemoveMetadataByName(t);
    }
}

// 存檔用的縮圖（最長邊 160）。
Image makeThumbnail(const Image& straight) {
    const int maxSide = 160;
    float s = static_cast<float>(maxSide) / std::max(straight.width, straight.height);
    if (s >= 1.0f) return straight;
    return resample(straight, std::max(1, static_cast<int>(straight.width * s)), std::max(1, static_cast<int>(straight.height * s)));
}

bool readFileBytes(const std::wstring& path, std::vector<uint8_t>& out) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                           FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size{};
    GetFileSizeEx(h, &size);
    out.resize(static_cast<size_t>(size.QuadPart));
    size_t done = 0;
    while (done < out.size()) {
        DWORD chunk = static_cast<DWORD>(std::min<size_t>(out.size() - done, 1u << 26));
        DWORD got = 0;
        if (!ReadFile(h, out.data() + done, chunk, &got, nullptr) || got == 0) break;
        done += got;
    }
    CloseHandle(h);
    return done == out.size();
}

LoadResult loadWebP(const std::wstring& path) {
    LoadResult r;
    std::vector<uint8_t> bytes;
    if (!readFileBytes(path, bytes)) { r.error = tr(L"Could not read the file"); return r; }
    WebPBitstreamFeatures feat{};
    if (WebPGetFeatures(bytes.data(), bytes.size(), &feat) != VP8_STATUS_OK) {
        r.error = tr(L"Not a valid WebP file");
        return r;
    }
    auto img = std::make_shared<Image>(feat.width, feat.height);
    WebPDecoderConfig config;
    if (!WebPInitDecoderConfig(&config)) { r.error = tr(L"libwebp initialization failed"); return r; }
    config.output.colorspace = MODE_bgrA; // premultiplied BGRA
    config.output.is_external_memory = 1;
    config.output.u.RGBA.rgba = img->pixels.data();
    config.output.u.RGBA.stride = static_cast<int>(img->stride());
    config.output.u.RGBA.size = img->pixels.size();
    if (WebPDecode(bytes.data(), bytes.size(), &config) != VP8_STATUS_OK) {
        r.error = tr(L"WebP decoding failed");
        return r;
    }
    r.info.formatName = tr(L"WebP image");
    r.info.colorModel = L"RGB";
    r.info.bitsPerPixel = feat.has_alpha ? 32 : 24;
    // 容器裡的 EXIF / XMP / ICCP chunk：資訊面板要顯示，orientation 也要套。
    WebPChunks chunks = readWebPChunks(bytes);
    if (!chunks.exif.empty()) {
        auto factory = makeFactory();
        if (auto reader = app1ReaderFromExif(factory.Get(), chunks.exif)) walkReader(reader.Get(), r.info.sections, r.info, 0);
    }
    if (!chunks.xmp.empty()) {
        auto factory = makeFactory();
        if (auto reader = metadataReaderFromBytes(factory.Get(), GUID_MetadataFormatXMP, chunks.xmp)) walkReader(reader.Get(), r.info.sections, r.info, 0);
    }
    if (!chunks.icc.empty()) r.info.colorProfile = iccProfileDescription(chunks.icc.data(), chunks.icc.size());
    std::vector<MetaSection> kept;
    for (auto& sec : r.info.sections)
        if (!sec.items.empty()) kept.push_back(std::move(sec));
    r.info.sections = std::move(kept);
    if (r.info.orientation >= 2 && r.info.orientation <= 8) img = std::make_shared<Image>(applyExifOrientation(*img, r.info.orientation));
    r.image = img;
    return r;
}

bool isWebPFile(const std::wstring& path) {
    if (extensionOf(path) == L".webp") return true;
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    char hdr[12] = {};
    DWORD got = 0;
    ReadFile(h, hdr, 12, &got, nullptr);
    CloseHandle(h);
    return got == 12 && memcmp(hdr, "RIFF", 4) == 0 && memcmp(hdr + 8, "WEBP", 4) == 0;
}

bool hasTransparency(const Image& img) {
    for (int y = 0; y < img.height; ++y) {
        const uint8_t* p = img.row(y) + 3;
        for (int x = 0; x < img.width; ++x, p += 4)
            if (*p != 255) return true;
    }
    return false;
}

bool writeFileBytes(const std::wstring& path, const uint8_t* data, size_t size) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    size_t done = 0;
    while (done < size) {
        DWORD chunk = static_cast<DWORD>(std::min<size_t>(size - done, 1u << 26));
        DWORD put = 0;
        if (!WriteFile(h, data + done, chunk, &put, nullptr) || put == 0) break;
        done += put;
    }
    CloseHandle(h);
    return done == size;
}

bool saveWebP(const Image& straight, const std::wstring& tmpPath, const SourceMeta& meta, bool sizeChanged, std::wstring* error) {
    uint8_t* out = nullptr;
    size_t n = WebPEncodeBGRA(straight.pixels.data(), straight.width, straight.height,
                              static_cast<int>(straight.stride()), 90.0f, &out);
    if (n == 0 || !out) { if (error) *error = tr(L"WebP encoding failed"); return false; }
    bool ok = false;
    // 有 metadata 要帶就用 mux 把 EXIF / XMP / ICCP chunk 組進容器。
    SourceBytes sb = meta.valid ? bytesForWebP(meta) : SourceBytes{};
    std::vector<uint8_t>& exif = sb.exif;
    std::vector<uint8_t>& xmp = sb.xmp;
    const std::vector<uint8_t>& icc = meta.valid ? meta.chunks.icc : std::vector<uint8_t>{};
    if (!exif.empty()) patchExifTiff(exif, sizeChanged, straight.width, straight.height);
    if (!exif.empty() || !xmp.empty() || !icc.empty()) {
        WebPData bitstream{out, n};
        if (WebPMux* mux = WebPMuxCreate(&bitstream, 0)) {
            auto set = [&](const char* fourcc, const std::vector<uint8_t>& v) {
                if (v.empty()) return;
                WebPData d{v.data(), v.size()};
                WebPMuxSetChunk(mux, fourcc, &d, 1);
            };
            set("ICCP", icc);
            set("EXIF", exif);
            set("XMP ", xmp);
            WebPData assembled{nullptr, 0};
            if (WebPMuxAssemble(mux, &assembled) == WEBP_MUX_OK) {
                ok = writeFileBytes(tmpPath, assembled.bytes, assembled.size);
                WebPDataClear(&assembled);
                WebPMuxDelete(mux);
                WebPFree(out);
                if (!ok && error) *error = tr(L"Writing the file failed");
                return ok;
            }
            WebPMuxDelete(mux);
        }
    }
    ok = writeFileBytes(tmpPath, out, n);
    WebPFree(out);
    if (!ok && error) *error = tr(L"Writing the file failed");
    return ok;
}

bool saveWithWic(const Image& straight, const std::wstring& tmpPath, const GUID& container, bool keepAlpha,
                 const SourceMeta& meta, bool sizeChanged, std::wstring* error) {
    auto factory = makeFactory();
    if (!factory) { if (error) *error = tr(L"Could not create the WIC factory"); return false; }

    ComPtr<IWICStream> stream;
    HRESULT hr = factory->CreateStream(&stream);
    if (SUCCEEDED(hr)) hr = stream->InitializeFromFilename(tmpPath.c_str(), GENERIC_WRITE);
    if (FAILED(hr)) { if (error) *error = hrText(tr(L"Could not create the file"), hr); return false; }

    ComPtr<IWICBitmapEncoder> encoder;
    hr = factory->CreateEncoder(container, nullptr, &encoder);
    if (SUCCEEDED(hr)) hr = encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache);
    if (FAILED(hr)) { if (error) *error = hrText(tr(L"Could not create the encoder"), hr); return false; }

    ComPtr<IWICBitmapFrameEncode> frame;
    ComPtr<IPropertyBag2> props;
    hr = encoder->CreateNewFrame(&frame, &props);
    if (FAILED(hr)) { if (error) *error = hrText(tr(L"Could not create the frame"), hr); return false; }
    if (props && isGuid(container, GUID_ContainerFormatJpeg)) {
        PROPBAG2 opt{};
        wchar_t name[] = L"ImageQuality";
        opt.pstrName = name;
        VARIANT v;
        VariantInit(&v);
        v.vt = VT_R4;
        v.fltVal = 0.9f;
        props->Write(1, &opt, &v);
    }
    hr = frame->Initialize(props.Get());
    if (SUCCEEDED(hr) && meta.valid) applyMetadata(factory.Get(), frame.Get(), container, meta, straight, sizeChanged);
    if (SUCCEEDED(hr)) hr = frame->SetSize(static_cast<UINT>(straight.width), static_cast<UINT>(straight.height));
    if (SUCCEEDED(hr)) hr = frame->SetResolution(straight.dpiX, straight.dpiY);
    if (FAILED(hr)) { if (error) *error = hrText(tr(L"Frame setup failed"), hr); return false; }
    if (meta.valid && (isGuid(container, GUID_ContainerFormatJpeg) || isGuid(container, GUID_ContainerFormatTiff))) {
        // EXIF 縮圖從編輯後的圖重新產生；不支援的編碼器會失敗，忽略。
        Image thumb = makeThumbnail(straight);
        ComPtr<IWICBitmap> tb;
        if (SUCCEEDED(factory->CreateBitmapFromMemory(static_cast<UINT>(thumb.width), static_cast<UINT>(thumb.height),
                                                      GUID_WICPixelFormat32bppBGRA, static_cast<UINT>(thumb.stride()),
                                                      static_cast<UINT>(thumb.pixels.size()), const_cast<BYTE*>(thumb.pixels.data()), &tb)))
            frame->SetThumbnail(tb.Get());
    }

    GUID want = keepAlpha ? GUID_WICPixelFormat32bppBGRA : GUID_WICPixelFormat24bppBGR;
    frame->SetPixelFormat(&want); // 編碼器不支援時會改成它支援的格式，WriteSource 會自動轉換

    ComPtr<IWICBitmap> bitmap;
    hr = factory->CreateBitmapFromMemory(static_cast<UINT>(straight.width), static_cast<UINT>(straight.height),
                                         GUID_WICPixelFormat32bppBGRA, static_cast<UINT>(straight.stride()),
                                         static_cast<UINT>(straight.pixels.size()),
                                         const_cast<BYTE*>(straight.pixels.data()), &bitmap);
    if (FAILED(hr)) { if (error) *error = hrText(tr(L"Could not create the source bitmap"), hr); return false; }

    hr = frame->WriteSource(bitmap.Get(), nullptr);
    if (SUCCEEDED(hr)) hr = frame->Commit();
    if (SUCCEEDED(hr)) hr = encoder->Commit();
    if (FAILED(hr)) { if (error) *error = hrText(tr(L"Writing the image failed"), hr); return false; }
    return true;
}

} // namespace

void patchExifTiff(std::vector<uint8_t>& t, bool sizeChanged, int newW, int newH) {
    if (t.size() < 8) return;
    bool le = t[0] == 'I' && t[1] == 'I';
    if (!le && !(t[0] == 'M' && t[1] == 'M')) return;
    auto rd16 = [&](size_t o) -> unsigned { return le ? (t[o] | (t[o + 1] << 8)) : ((t[o] << 8) | t[o + 1]); };
    auto rd32 = [&](size_t o) -> uint32_t {
        return le ? (t[o] | (t[o + 1] << 8) | (t[o + 2] << 16) | (static_cast<uint32_t>(t[o + 3]) << 24))
                  : ((static_cast<uint32_t>(t[o]) << 24) | (t[o + 1] << 16) | (t[o + 2] << 8) | t[o + 3]);
    };
    auto wr16 = [&](size_t o, unsigned v) { if (le) { t[o] = v & 0xFF; t[o + 1] = (v >> 8) & 0xFF; } else { t[o] = (v >> 8) & 0xFF; t[o + 1] = v & 0xFF; } };
    auto wr32 = [&](size_t o, uint32_t v) {
        if (le) { t[o] = v & 0xFF; t[o + 1] = (v >> 8) & 0xFF; t[o + 2] = (v >> 16) & 0xFF; t[o + 3] = (v >> 24) & 0xFF; }
        else { t[o] = (v >> 24) & 0xFF; t[o + 1] = (v >> 16) & 0xFF; t[o + 2] = (v >> 8) & 0xFF; t[o + 3] = v & 0xFF; }
    };
    if (rd16(2) != 42) return;
    uint32_t ifd0 = rd32(4);
    auto validIfd = [&](uint32_t off) { return off >= 8 && off + 2 <= t.size() && off + 2 + static_cast<size_t>(rd16(off)) * 12 + 4 <= t.size(); };
    if (!validIfd(ifd0)) return;
    uint32_t exifIfd = 0;
    unsigned n0 = rd16(ifd0);
    for (unsigned i = 0; i < n0; ++i) {
        size_t e = ifd0 + 2 + static_cast<size_t>(i) * 12;
        unsigned tag = rd16(e), type = rd16(e + 2);
        uint32_t count = rd32(e + 4);
        if (tag == 0x0112 && type == 3 && count == 1) wr16(e + 8, 1); // Orientation -> 1
        if (tag == 0x8769 && (type == 4 || type == 13) && count == 1) exifIfd = rd32(e + 8);
    }
    wr32(ifd0 + 2 + static_cast<size_t>(n0) * 12, 0); // 切掉 IFD1（縮圖）
    if (sizeChanged && validIfd(exifIfd)) {
        unsigned n1 = rd16(exifIfd);
        for (unsigned i = 0; i < n1; ++i) {
            size_t e = exifIfd + 2 + static_cast<size_t>(i) * 12;
            unsigned tag = rd16(e), type = rd16(e + 2);
            uint32_t count = rd32(e + 4);
            if ((tag != 0xA002 && tag != 0xA003) || count != 1) continue;
            uint32_t v = static_cast<uint32_t>(tag == 0xA002 ? newW : newH);
            if (type == 4) wr32(e + 8, v);
            else if (type == 3 && v <= 0xFFFF) { wr16(e + 8, v); wr16(e + 10, 0); }
        }
    }
}

std::vector<std::wstring> metadataLossOnSave(const ImageInfo& info, const std::wstring& sourceExt, const std::wstring& targetExt) {
    auto norm = [](std::wstring e) {
        if (e == L".jpeg" || e == L".jpe" || e == L".jfif") return std::wstring(L".jpg");
        if (e == L".tiff") return std::wstring(L".tif");
        if (e == L".dib") return std::wstring(L".bmp");
        return e;
    };
    std::wstring src = norm(sourceExt), dst = norm(targetExt);
    std::vector<std::wstring> lost;
    if (src == dst) return lost;
    // 每種目標格式能裝的區塊。EXIF 家族（TIFF / Exif / GPS / Interoperability）一起算。
    auto keeps = [&](const std::wstring& section) {
        bool exifFamily = section == L"TIFF" || section == L"Exif" || section == L"GPS" || section == L"Interoperability" || section == L"TIFF (sub-IFD)";
        if (dst == L".jpg") return exifFamily || section == L"XMP" || section == L"IPTC" || section == L"Photoshop" || section == L"ICC" || section == L"JFIF";
        if (dst == L".tif") return exifFamily || section == L"XMP" || section == L"ICC";
        if (dst == L".webp") return (exifFamily && src == L".jpg") || (section == L"XMP" && src == L".jpg") || section == L"ICC";
        if (dst == L".png") return section == L"ICC" || section == L"XMP";
        return false; // bmp
    };
    bool exifLost = false;
    for (const auto& sec : info.sections) {
        if (keeps(sec.name)) continue;
        bool exifFamily = sec.name == L"TIFF" || sec.name == L"Exif" || sec.name == L"GPS" || sec.name == L"Interoperability" || sec.name == L"TIFF (sub-IFD)";
        if (exifFamily) { if (!exifLost) { exifLost = true; lost.push_back(L"Exif"); } continue; }
        lost.push_back(sec.name);
    }
    if (!info.colorProfile.empty() && !keeps(L"ICC")) lost.push_back(L"ICC");
    return lost;
}

LoadResult loadImage(const std::wstring& path) {
    ComInit com;
    if (isWebPFile(path)) return loadWebP(path);
    return loadWithWic(path);
}

bool readImageSize(const std::wstring& path, int& width, int& height) {
    width = height = 0;
    if (isWebPFile(path)) {
        // WebPGetInfo 只看檔頭，讀前 64 bytes 就夠。
        HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE) return false;
        uint8_t head[64];
        DWORD got = 0;
        ReadFile(h, head, sizeof(head), &got, nullptr);
        CloseHandle(h);
        int w = 0, hh = 0;
        if (!WebPGetInfo(head, got, &w, &hh)) return false;
        width = w;
        height = hh;
        return true;
    }
    ComInit com;
    auto factory = makeFactory();
    if (!factory) return false;
    ComPtr<IWICBitmapDecoder> decoder;
    if (FAILED(factory->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &decoder))) return false;
    ComPtr<IWICBitmapFrameDecode> frame;
    if (FAILED(decoder->GetFrame(0, &frame))) return false;
    UINT w = 0, hh = 0;
    if (FAILED(frame->GetSize(&w, &hh)) || w == 0 || hh == 0) return false;
    int orientation = 1;
    ComPtr<IWICMetadataQueryReader> q;
    if (SUCCEEDED(frame->GetMetadataQueryReader(&q))) {
        for (const wchar_t* key : {L"/app1/ifd/{ushort=274}", L"/ifd/{ushort=274}"}) {
            PROPVARIANT v;
            PropVariantInit(&v);
            if (SUCCEEDED(q->GetMetadataByName(key, &v))) {
                if (v.vt == VT_UI2) orientation = v.uiVal;
                else if (v.vt == VT_UI4) orientation = static_cast<int>(v.ulVal);
                PropVariantClear(&v);
                break;
            }
            PropVariantClear(&v);
        }
    }
    bool swap = orientation >= 5 && orientation <= 8;
    width = static_cast<int>(swap ? hh : w);
    height = static_cast<int>(swap ? w : hh);
    return true;
}

bool canSaveExtension(const std::wstring& ext) {
    return ext == L".png" || ext == L".jpg" || ext == L".jpeg" || ext == L".jpe" || ext == L".jfif" ||
           ext == L".bmp" || ext == L".dib" || ext == L".tif" || ext == L".tiff" || ext == L".webp";
}

bool saveImage(const Image& img, const std::wstring& path, std::wstring* error, const SaveOptions& opt) {
    ComInit com;
    std::wstring ext = extensionOf(path);
    if (!canSaveExtension(ext)) { if (error) { wchar_t b[128]; swprintf(b, 128, tr(L"Saving this format is not supported: %s"), ext.c_str()); *error = b; } return false; }

    Image straight = img;
    unpremultiply(straight);
    const bool alpha = hasTransparency(straight);
    std::wstring tmp = path + L".mp-saving.tmp";

    // 來源 metadata。尺寸有沒有變要以「套過 orientation 後」的來源尺寸比。
    // 來源 metadata。EXIF 的 PixelX/YDimension 指的是檔案裡實際的像素格，所以跟檔案裡的寬高比
    //（orientation 轉正過的圖，寬高對調了，也算變）。
    SourceMeta meta = readSourceMeta(makeFactory().Get(), opt.metadataSource);
    bool sizeChanged = meta.valid && (meta.width != img.width || meta.height != img.height);

    bool ok = false;
    if (ext == L".webp") {
        ok = saveWebP(straight, tmp, meta, sizeChanged, error);
    } else {
        GUID container = GUID_ContainerFormatPng;
        bool keepAlpha = alpha;
        if (ext == L".jpg" || ext == L".jpeg" || ext == L".jpe" || ext == L".jfif") { container = GUID_ContainerFormatJpeg; keepAlpha = false; }
        else if (ext == L".bmp" || ext == L".dib") container = GUID_ContainerFormatBmp;
        else if (ext == L".tif" || ext == L".tiff") container = GUID_ContainerFormatTiff;
        ok = saveWithWic(straight, tmp, container, keepAlpha, meta, sizeChanged, error);
    }
    // 來源檔常常就是要覆寫的那個檔：decoder 還握著它時 MoveFileEx 會失敗，先放掉。
    meta = SourceMeta{};
    if (!ok) { DeleteFileW(tmp.c_str()); return false; }
    if (!MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        if (error) *error = tr(L"Could not replace the original file");
        DeleteFileW(tmp.c_str());
        return false;
    }
    return true;
}

} // namespace mp
