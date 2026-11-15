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
#include <cstring>
#include <new>
#include <optional>
#include <vector>

#include "core/ImageOps.h"
#include "core/Lang.h"
#include "core/PathUtil.h"

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

// PROPVARIANT 的初始化與釋放。
struct PropVar {
    PROPVARIANT v;
    PropVar() { PropVariantInit(&v); }
    ~PropVar() { PropVariantClear(&v); }
    PropVar(const PropVar&) = delete;
    PropVar& operator=(const PropVar&) = delete;
};

IoError failure(IoErrorCode code, HRESULT hr = S_OK) {
    IoError e;
    e.code = code;
    e.hresult = hr;
    return e;
}

IoError systemFailure(IoErrorCode code, DWORD lastError) {
    IoError e;
    e.code = code;
    e.systemError = lastError;
    return e;
}

ComPtr<IWICImagingFactory> makeFactory() {
    ComPtr<IWICImagingFactory> f;
    CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&f));
    return f;
}

bool isGuid(const GUID& a, const GUID& b) { return IsEqualGUID(a, b) != FALSE; }

// ---- 格式對照（WIC 的容器 GUID；支援哪些格式見 ImageFormat.cpp）----

ImageFormat formatOfContainer(const GUID& container) {
    if (isGuid(container, GUID_ContainerFormatJpeg)) return ImageFormat::Jpeg;
    if (isGuid(container, GUID_ContainerFormatPng)) return ImageFormat::Png;
    if (isGuid(container, GUID_ContainerFormatBmp)) return ImageFormat::Bmp;
    if (isGuid(container, GUID_ContainerFormatTiff)) return ImageFormat::Tiff;
    return ImageFormat::Unknown;
}

// WIC 編碼器的容器；WebP 與 Unknown 沒有，回 false。
bool containerOfFormat(ImageFormat format, GUID& container) {
    switch (format) {
    case ImageFormat::Jpeg: container = GUID_ContainerFormatJpeg; return true;
    case ImageFormat::Png: container = GUID_ContainerFormatPng; return true;
    case ImageFormat::Bmp: container = GUID_ContainerFormatBmp; return true;
    case ImageFormat::Tiff: container = GUID_ContainerFormatTiff; return true;
    default: return false;
    }
}

// ---- 像素格式描述 ----

ColorModel colorModelOf(const GUID& fmt) {
    if (isGuid(fmt, GUID_WICPixelFormatBlackWhite) || isGuid(fmt, GUID_WICPixelFormat2bppGray) ||
        isGuid(fmt, GUID_WICPixelFormat4bppGray) || isGuid(fmt, GUID_WICPixelFormat8bppGray) ||
        isGuid(fmt, GUID_WICPixelFormat16bppGray) || isGuid(fmt, GUID_WICPixelFormat16bppGrayFixedPoint) ||
        isGuid(fmt, GUID_WICPixelFormat16bppGrayHalf) || isGuid(fmt, GUID_WICPixelFormat32bppGrayFloat) ||
        isGuid(fmt, GUID_WICPixelFormat32bppGrayFixedPoint))
        return ColorModel::Grayscale;
    if (isGuid(fmt, GUID_WICPixelFormat1bppIndexed) || isGuid(fmt, GUID_WICPixelFormat2bppIndexed) ||
        isGuid(fmt, GUID_WICPixelFormat4bppIndexed) || isGuid(fmt, GUID_WICPixelFormat8bppIndexed))
        return ColorModel::Indexed;
    if (isGuid(fmt, GUID_WICPixelFormat32bppCMYK) || isGuid(fmt, GUID_WICPixelFormat64bppCMYK) ||
        isGuid(fmt, GUID_WICPixelFormat40bppCMYKAlpha) || isGuid(fmt, GUID_WICPixelFormat80bppCMYKAlpha))
        return ColorModel::Cmyk;
    return ColorModel::Rgb;
}

int bitsPerPixelOf(IWICImagingFactory* factory, const GUID& fmt) {
    ComPtr<IWICComponentInfo> ci;
    if (FAILED(factory->CreateComponentInfo(fmt, &ci))) return 0;
    ComPtr<IWICPixelFormatInfo> pi;
    if (FAILED(ci.As(&pi))) return 0;
    UINT bpp = 0;
    if (FAILED(pi->GetBitsPerPixel(&bpp))) return 0;
    return static_cast<int>(bpp);
}

// ---- metadata：讀出來給資訊面板 ----

// JFIF（APP0）欄位的名稱（英文翻譯 key）；沒有對應回 nullptr。
const wchar_t* jfifTagName(unsigned tag) {
    switch (tag) {
    case 0: return trKey(L"JFIF version");
    case 1: return trKey(L"Density unit");
    case 2: return trKey(L"X density");
    case 3: return trKey(L"Y density");
    case 4: return trKey(L"Thumbnail width");
    case 5: return trKey(L"Thumbnail height");
    case 6: return trKey(L"Thumbnail");
    default: return nullptr;
    }
}

// 這個 metadata 區塊歸資訊面板的哪一區。App1 / App13 只是容器，本身不顯示（回 false）。
bool sectionIdOf(const GUID& fmt, MetaSectionId& id) {
    if (isGuid(fmt, GUID_MetadataFormatApp1) || isGuid(fmt, GUID_MetadataFormatApp13)) return false;
    if (isGuid(fmt, GUID_MetadataFormatIfd)) id = MetaSectionId::Tiff;
    else if (isGuid(fmt, GUID_MetadataFormatSubIfd)) id = MetaSectionId::TiffSubIfd;
    else if (isGuid(fmt, GUID_MetadataFormatExif)) id = MetaSectionId::Exif;
    else if (isGuid(fmt, GUID_MetadataFormatGps)) id = MetaSectionId::Gps;
    else if (isGuid(fmt, GUID_MetadataFormatInterop)) id = MetaSectionId::Interop;
    else if (isGuid(fmt, GUID_MetadataFormatXMP) || isGuid(fmt, GUID_MetadataFormatXMPStruct) ||
             isGuid(fmt, GUID_MetadataFormatXMPBag) || isGuid(fmt, GUID_MetadataFormatXMPSeq) ||
             isGuid(fmt, GUID_MetadataFormatXMPAlt))
        id = MetaSectionId::Xmp;
    else if (isGuid(fmt, GUID_MetadataFormatIPTC)) id = MetaSectionId::Iptc;
    else if (isGuid(fmt, GUID_MetadataFormatIRB)) id = MetaSectionId::Photoshop;
    else if (isGuid(fmt, GUID_MetadataFormatApp0)) id = MetaSectionId::Jfif;
    else if (isGuid(fmt, GUID_MetadataFormatChunktEXt) || isGuid(fmt, GUID_MetadataFormatChunkiTXt) ||
             isGuid(fmt, GUID_MetadataFormatChunkgAMA) || isGuid(fmt, GUID_MetadataFormatChunkcHRM) ||
             isGuid(fmt, GUID_MetadataFormatChunksRGB) || isGuid(fmt, GUID_MetadataFormatChunktIME) ||
             isGuid(fmt, GUID_MetadataFormatChunkbKGD) || isGuid(fmt, GUID_MetadataFormatChunkhIST) ||
             isGuid(fmt, GUID_MetadataFormatChunkiCCP))
        id = MetaSectionId::Png;
    else id = MetaSectionId::Other;
    return true;
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

MetaValue textValue(std::wstring text) {
    MetaValue m;
    m.text = std::move(text);
    return m;
}

MetaValue numberValue(MetaValue::Kind kind, unsigned long long number) {
    MetaValue m;
    m.kind = kind;
    m.number = number;
    return m;
}

MetaValue scalarValue(const PROPVARIANT& v) {
    wchar_t buf[64];
    switch (v.vt) {
    case VT_UI1: swprintf(buf, 64, L"%u", v.bVal); return textValue(buf);
    case VT_I1: swprintf(buf, 64, L"%d", v.cVal); return textValue(buf);
    case VT_UI2: swprintf(buf, 64, L"%u", v.uiVal); return textValue(buf);
    case VT_I2: swprintf(buf, 64, L"%d", v.iVal); return textValue(buf);
    case VT_UI4: swprintf(buf, 64, L"%u", v.ulVal); return textValue(buf);
    case VT_I4: swprintf(buf, 64, L"%d", v.lVal); return textValue(buf);
    case VT_UI8: {
        // Exif 的 unsigned rational：低 32 位分子，高 32 位分母。
        unsigned num = v.uhVal.LowPart, den = v.uhVal.HighPart;
        if (den == 0) swprintf(buf, 64, L"%llu", v.uhVal.QuadPart);
        else if (den == 1) swprintf(buf, 64, L"%u", num);
        else swprintf(buf, 64, L"%u/%u (%.4g)", num, den, static_cast<double>(num) / den);
        return textValue(buf);
    }
    case VT_I8: {
        int num = v.hVal.LowPart, den = v.hVal.HighPart;
        if (den == 0) swprintf(buf, 64, L"%lld", v.hVal.QuadPart);
        else if (den == 1) swprintf(buf, 64, L"%d", num);
        else swprintf(buf, 64, L"%d/%d (%.4g)", num, den, static_cast<double>(num) / den);
        return textValue(buf);
    }
    case VT_R4: swprintf(buf, 64, L"%.4g", v.fltVal); return textValue(buf);
    case VT_R8: swprintf(buf, 64, L"%.6g", v.dblVal); return textValue(buf);
    case VT_BOOL: return numberValue(MetaValue::Kind::Bool, v.boolVal ? 1 : 0);
    case VT_LPSTR: return textValue(toW(v.pszVal));
    case VT_LPWSTR: return textValue(v.pwszVal ? v.pwszVal : L"");
    case VT_BSTR: return textValue(v.bstrVal ? v.bstrVal : L"");
    case VT_EMPTY: case VT_NULL: return textValue(L"");
    default: return numberValue(MetaValue::Kind::UnknownType, v.vt);
    }
}

// 純量當成一段字（陣列的元素、PNG chunk 的名字）：這裡沒辦法交給 app 翻譯，布林與不認得的型別用英文。
// WIC 的 metadata 實際上不會給這兩種元素。
std::wstring scalarText(const PROPVARIANT& v) {
    MetaValue m = scalarValue(v);
    switch (m.kind) {
    case MetaValue::Kind::Bool: return m.number ? L"Yes" : L"No";
    case MetaValue::Kind::UnknownType: return L"(type " + std::to_wstring(m.number) + L")";
    default: return m.text;
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

MetaValue valueOf(const PROPVARIANT& v, unsigned tag) {
    wchar_t buf[64];
    if (v.vt == VT_BLOB) return numberValue(MetaValue::Kind::ByteCount, v.blob.cbSize);
    if (!(v.vt & VT_VECTOR)) return scalarValue(v);
    VARTYPE et = v.vt & VT_TYPEMASK;
    ULONG n = v.caub.cElems; // 所有 vector 的 cElems 位置相同
    // 有些寫入器把 rational 存成兩個整數。
    if (n == 2 && (et == VT_UI4 || et == VT_I4) && isRationalTag(tag)) {
        double num = et == VT_UI4 ? v.caul.pElems[0] : v.cal.pElems[0];
        double den = et == VT_UI4 ? v.caul.pElems[1] : v.cal.pElems[1];
        if (den == 1) { swprintf(buf, 64, L"%.0f", num); return textValue(buf); }
        if (den != 0) { swprintf(buf, 64, L"%.0f/%.0f (%.4g)", num, den, num / den); return textValue(buf); }
    }
    if (et == VT_UI1) {
        if (n > 32) return numberValue(MetaValue::Kind::ByteCount, n);
        // 短的 byte 陣列多半是 ASCII 版本字串，例如 Exif 版本 "0230"。
        bool printable = n > 0;
        for (ULONG i = 0; i < n; ++i)
            if (v.caub.pElems[i] < 32 || v.caub.pElems[i] > 126) { printable = false; break; }
        if (printable) {
            std::wstring s;
            for (ULONG i = 0; i < n; ++i) s += static_cast<wchar_t>(v.caub.pElems[i]);
            return textValue(s);
        }
    }
    std::wstring out;
    ULONG shown = std::min<ULONG>(n, 8);
    for (ULONG i = 0; i < shown; ++i) {
        PropVar e;
        if (FAILED(PropVariantGetElem(v, i, &e.v))) break;
        if (i) out += L", ";
        out += scalarText(e.v);
    }
    if (n > shown) out += L", …";
    return textValue(out);
}

// 回傳索引而不是參照：走訪子區塊時 sections 會 push_back 重新配置，拿著參照或指標會變成懸空。
size_t sectionOf(std::vector<MetaSection>& sections, MetaSectionId id) {
    for (size_t i = 0; i < sections.size(); ++i)
        if (sections[i].id == id) return i;
    sections.push_back({id, {}});
    return sections.size() - 1;
}

void walkReader(IWICMetadataReader* reader, std::vector<MetaSection>& sections, int depth);

constexpr size_t kNoSection = static_cast<size_t>(-1);

void addItem(IWICMetadataReader* reader, const GUID& fmt, size_t sectionIdx, std::vector<MetaSection>& sections, int depth,
             UINT i) {
    PropVar schema, id, value;
    if (FAILED(reader->GetValueByIndex(i, &schema.v, &id.v, &value.v))) return;
    if (value.v.vt == VT_UNKNOWN && value.v.punkVal) {
        ComPtr<IWICMetadataReader> child;
        if (SUCCEEDED(value.v.punkVal->QueryInterface(IID_PPV_ARGS(&child)))) walkReader(child.Get(), sections, depth + 1);
        return;
    }
    if (sectionIdx == kNoSection) return;
    MetaItem item;
    unsigned tag = 0xFFFFFFFF;
    if (id.v.vt == VT_UI2 || id.v.vt == VT_UI4 || id.v.vt == VT_I4) {
        tag = id.v.vt == VT_UI2 ? id.v.uiVal : static_cast<unsigned>(id.v.ulVal);
        const bool jfif = isGuid(fmt, GUID_MetadataFormatApp0);
        if (const wchar_t* key = jfif ? jfifTagName(tag) : exifTagName(tag)) {
            item.label = key;
            item.labelIsKey = true;
        } else {
            wchar_t buf[32];
            swprintf(buf, 32, L"Tag 0x%04X", tag);
            item.label = buf;
        }
        if (jfif && tag == 0) { // 版本是 ushort，例如 0x0101 -> 1.01
            unsigned ver = value.v.vt == VT_UI2 ? value.v.uiVal : 0;
            wchar_t vb[16];
            swprintf(vb, 16, L"%u.%02u", ver >> 8, ver & 0xFF);
            item.value = textValue(vb);
            sections[sectionIdx].items.push_back(std::move(item));
            return;
        }
        if (tag == 0x927C) { // MakerNote 不展開
            item.value = numberValue(MetaValue::Kind::ByteCount, (value.v.vt & VT_VECTOR) ? value.v.caub.cElems : 0ul);
            sections[sectionIdx].items.push_back(std::move(item));
            return;
        }
    } else {
        item.label = chunkPrefixOf(fmt) + scalarText(id.v);
    }
    item.value = valueOf(value.v, tag);
    if (item.value.kind == MetaValue::Kind::Text && item.value.text.empty()) return;
    sections[sectionIdx].items.push_back(std::move(item));
}

void walkReader(IWICMetadataReader* reader, std::vector<MetaSection>& sections, int depth) {
    if (depth > 6) return;
    GUID fmt{};
    if (FAILED(reader->GetMetadataFormat(&fmt))) return;
    MetaSectionId id = MetaSectionId::Other;
    const bool shown = sectionIdOf(fmt, id);
    UINT count = 0;
    if (FAILED(reader->GetCount(&count))) return;
    size_t sectionIdx = shown ? sectionOf(sections, id) : kNoSection;
    for (UINT i = 0; i < count; ++i) addItem(reader, fmt, sectionIdx, sections, depth, i);
}

// 空區塊不顯示。
void dropEmptySections(std::vector<MetaSection>& sections) {
    std::vector<MetaSection> kept;
    for (auto& s : sections)
        if (!s.items.empty()) kept.push_back(std::move(s));
    sections = std::move(kept);
}

void readMetadata(IWICBitmapFrameDecode* frame, ImageInfo& info) {
    ComPtr<IWICMetadataBlockReader> block;
    if (FAILED(frame->QueryInterface(IID_PPV_ARGS(&block)))) return;
    UINT count = 0;
    if (FAILED(block->GetCount(&count))) return;
    for (UINT i = 0; i < count; ++i) {
        ComPtr<IWICMetadataReader> reader;
        if (SUCCEEDED(block->GetReaderByIndex(i, &reader))) walkReader(reader.Get(), info.sections, 0);
    }
    dropEmptySections(info.sections);
}

// IFD0 的 Orientation（JPEG 在 /app1/ifd，TIFF 在 /ifd）。沒有就是 1。
// loadImage 轉正像素與 readImageSize 對調寬高都用這一個，兩邊不會不一致。
int orientationOf(IWICBitmapFrameDecode* frame) {
    ComPtr<IWICMetadataQueryReader> q;
    if (FAILED(frame->GetMetadataQueryReader(&q))) return 1;
    for (const wchar_t* key : {L"/app1/ifd/{ushort=274}", L"/ifd/{ushort=274}"}) {
        PropVar v;
        if (FAILED(q->GetMetadataByName(key, &v.v))) continue;
        int o = v.v.vt == VT_UI2 ? v.v.uiVal : v.v.vt == VT_UI4 ? static_cast<int>(v.v.ulVal) : 1;
        return o >= 1 && o <= 8 ? o : 1;
    }
    return 1;
}

// frame 帶的 color context（ICC profile 或 Exif 色彩空間）。
std::vector<ComPtr<IWICColorContext>> colorContextsOf(IWICImagingFactory* factory, IWICBitmapFrameDecode* frame) {
    std::vector<ComPtr<IWICColorContext>> ctxs;
    UINT count = 0;
    if (FAILED(frame->GetColorContexts(0, nullptr, &count)) || count == 0) return ctxs;
    ctxs.resize(count);
    std::vector<IWICColorContext*> raw(count);
    for (UINT i = 0; i < count; ++i) {
        if (FAILED(factory->CreateColorContext(&ctxs[i]))) return {};
        raw[i] = ctxs[i].Get();
    }
    if (FAILED(frame->GetColorContexts(count, raw.data(), &count))) return {};
    ctxs.resize(count);
    return ctxs;
}

// color context 裡的 ICC profile 位元組；不是 profile 或讀不到回空的。
std::vector<uint8_t> profileBytesOf(IWICColorContext* ctx) {
    std::vector<uint8_t> out;
    WICColorContextType type;
    if (FAILED(ctx->GetType(&type)) || type != WICColorContextProfile) return out;
    UINT size = 0;
    if (FAILED(ctx->GetProfileBytes(0, nullptr, &size)) || size == 0) return out;
    out.resize(size);
    if (FAILED(ctx->GetProfileBytes(size, out.data(), &size))) out.clear();
    return out;
}

void readColorProfile(IWICImagingFactory* factory, IWICBitmapFrameDecode* frame, ImageInfo& info) {
    for (const auto& ctx : colorContextsOf(factory, frame)) {
        WICColorContextType type;
        if (FAILED(ctx->GetType(&type))) continue;
        if (type == WICColorContextProfile) {
            std::vector<uint8_t> bytes = profileBytesOf(ctx.Get());
            if (bytes.empty()) continue;
            info.colorProfile = iccProfileDescription(bytes.data(), bytes.size());
            if (!info.colorProfile.empty()) return;
        } else if (type == WICColorContextExifColorSpace) {
            UINT cs = 0;
            if (SUCCEEDED(ctx->GetExifColorSpace(&cs))) {
                if (cs == 1) info.colorProfile = L"sRGB IEC61966-2.1";
                else if (cs == 2) info.colorProfile = L"Adobe RGB (1998)";
            }
        }
    }
}

std::vector<uint8_t> iccBytesOf(IWICImagingFactory* factory, IWICBitmapFrameDecode* frame) {
    for (const auto& ctx : colorContextsOf(factory, frame)) {
        std::vector<uint8_t> bytes = profileBytesOf(ctx.Get());
        if (!bytes.empty()) return bytes;
    }
    return {};
}

LoadResult loadWithWic(const std::wstring& path) {
    LoadResult r;
    auto factory = makeFactory();
    if (!factory) { r.error = failure(IoErrorCode::WicUnavailable); return r; }

    ComPtr<IWICBitmapDecoder> decoder;
    HRESULT hr = factory->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ,
                                                    WICDecodeMetadataCacheOnDemand, &decoder);
    if (FAILED(hr)) { r.error = failure(IoErrorCode::DecodeFailed, hr); return r; }

    GUID container{};
    if (SUCCEEDED(decoder->GetContainerFormat(&container))) r.info.format = formatOfContainer(container);

    ComPtr<IWICBitmapFrameDecode> frame;
    hr = decoder->GetFrame(0, &frame);
    if (FAILED(hr)) { r.error = failure(IoErrorCode::NoFrame, hr); return r; }

    GUID fmt{};
    if (SUCCEEDED(frame->GetPixelFormat(&fmt))) {
        r.info.colorModel = colorModelOf(fmt);
        r.info.bitsPerPixel = bitsPerPixelOf(factory.Get(), fmt);
    }

    UINT w = 0, h = 0;
    hr = frame->GetSize(&w, &h);
    if (FAILED(hr) || w == 0 || h == 0) { r.error = failure(IoErrorCode::EmptyImage, FAILED(hr) ? hr : S_OK); return r; }

    auto img = std::make_shared<Image>(static_cast<int>(w), static_cast<int>(h));
    if (FAILED(frame->GetResolution(&img->dpiX, &img->dpiY)) || img->dpiX <= 0) img->dpiX = 96;
    if (img->dpiY <= 0) img->dpiY = 96;

    ComPtr<IWICFormatConverter> conv;
    hr = factory->CreateFormatConverter(&conv);
    if (SUCCEEDED(hr))
        hr = conv->Initialize(frame.Get(), GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0.0,
                              WICBitmapPaletteTypeMedianCut);
    if (FAILED(hr)) { r.error = failure(IoErrorCode::ConvertFailed, hr); return r; }
    hr = conv->CopyPixels(nullptr, static_cast<UINT>(img->stride()), static_cast<UINT>(img->pixels.size()),
                          img->pixels.data());
    if (FAILED(hr)) { r.error = failure(IoErrorCode::ReadPixelsFailed, hr); return r; }

    readMetadata(frame.Get(), r.info);
    readColorProfile(factory.Get(), frame.Get(), r.info);
    r.info.orientation = orientationOf(frame.Get());

    if (r.info.orientation >= 2) img = std::make_shared<Image>(applyExifOrientation(*img, r.info.orientation));
    r.image = img;
    return r;
}

// ---- metadata 搬運 ----

struct WebPChunks {
    std::vector<uint8_t> exif; // TIFF 結構（已去掉 "Exif\0\0" 前綴）
    std::vector<uint8_t> xmp;
    std::vector<uint8_t> icc;
};

bool readFileBytes(const std::wstring& path, std::vector<uint8_t>& out) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                           FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(h, &size)) { CloseHandle(h); return false; }
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
        PropVar schema, id, value;
        if (FAILED(src->GetValueByIndex(i, &schema.v, &id.v, &value.v)) || (id.v.vt != VT_UI2 && id.v.vt != VT_UI4)) continue;
        unsigned tag = id.v.vt == VT_UI2 ? id.v.uiVal : static_cast<unsigned>(id.v.ulVal);
        if (value.v.vt == VT_UNKNOWN && value.v.punkVal) {
            ComPtr<IWICMetadataReader> sub;
            if (FAILED(value.v.punkVal->QueryInterface(IID_PPV_ARGS(&sub)))) continue;
            GUID fmt{};
            sub->GetMetadataFormat(&fmt);
            const wchar_t* name = isGuid(fmt, GUID_MetadataFormatExif) ? L"/exif"
                                : isGuid(fmt, GUID_MetadataFormatGps) ? L"/gps"
                                : isGuid(fmt, GUID_MetadataFormatInterop) ? L"/interop" : nullptr;
            ComPtr<IWICMetadataQueryWriter> subW;
            if (name && SUCCEEDED(cf->CreateQueryWriter(fmt, nullptr, &subW))) {
                copyIfdTree(cf, sub.Get(), subW.Get());
                PROPVARIANT v; // 借用 subW，不擁有：不能 PropVariantClear
                PropVariantInit(&v);
                v.vt = VT_UNKNOWN;
                v.punkVal = subW.Get();
                dst->SetMetadataByName(name, &v);
            }
        } else if (tag != 0x8769 && tag != 0x8825 && tag != 0xA005) { // 指標欄位由 writer 自己產生
            wchar_t key[32];
            swprintf(key, 32, L"/{ushort=%u}", tag);
            dst->SetMetadataByName(key, &value.v);
        }
    }
}

// JPEG 的 XMP 區段前綴（含結尾的 \0）。
const char kJpegXmpPrefix[] = "http://ns.adobe.com/xap/1.0/";

// 把一段 XMP 包進一張 1×1 的 JPEG（放在記憶體裡）再用 WIC 解開，拿它的 frame 當 XMP 的來源：
// 這樣 WebP 來源的 XMP 可以和 WIC 來源走同一條「從 query reader 的 /xmp 轉成 writer」的路。
// 一個 APP1 區段裝不下（約 64 KB）時回空的。
// 從它轉出來的 writer 還連著這張 JPEG：要活到編碼器 Commit 之後（放在 SourceMeta 裡）。提早放掉的話
// 設進目標的那一步會失敗。
struct XmpCarrier {
    ComPtr<IStream> stream;
    ComPtr<IWICBitmapDecoder> decoder;
    ComPtr<IWICBitmapFrameDecode> frame;
};

XmpCarrier makeXmpCarrier(IWICImagingFactory* factory, const std::vector<uint8_t>& xmp) {
    XmpCarrier c;
    const size_t payload = sizeof(kJpegXmpPrefix) + xmp.size(); // sizeof 含 \0
    if (!factory || xmp.empty() || payload + 2 > 0xFFFF) return c;
    // 1×1 的 JPEG
    ComPtr<IStream> out;
    out.Attach(SHCreateMemStream(nullptr, 0));
    ComPtr<IWICBitmapEncoder> enc;
    ComPtr<IWICBitmapFrameEncode> fe;
    BYTE pixel[3] = {255, 255, 255};
    WICPixelFormatGUID fmt = GUID_WICPixelFormat24bppBGR;
    if (!out || FAILED(factory->CreateEncoder(GUID_ContainerFormatJpeg, nullptr, &enc)) ||
        FAILED(enc->Initialize(out.Get(), WICBitmapEncoderNoCache)) || FAILED(enc->CreateNewFrame(&fe, nullptr)) ||
        FAILED(fe->Initialize(nullptr)) || FAILED(fe->SetSize(1, 1)) || FAILED(fe->SetPixelFormat(&fmt)) ||
        FAILED(fe->WritePixels(1, 3, 3, pixel)) || FAILED(fe->Commit()) || FAILED(enc->Commit()))
        return c;
    STATSTG st{};
    if (FAILED(out->Stat(&st, STATFLAG_NONAME)) || st.cbSize.QuadPart < 4 || st.cbSize.QuadPart > (1 << 20)) return c;
    std::vector<uint8_t> jpeg(static_cast<size_t>(st.cbSize.QuadPart));
    LARGE_INTEGER zero{};
    ULONG got = 0;
    if (FAILED(out->Seek(zero, STREAM_SEEK_SET, nullptr)) ||
        FAILED(out->Read(jpeg.data(), static_cast<ULONG>(jpeg.size()), &got)) || got != jpeg.size())
        return c;
    if (jpeg[0] != 0xFF || jpeg[1] != 0xD8) return c;
    // 在 SOI 後面插一個 APP1：FF E1、長度（含自己 2 bytes，big endian）、前綴、XMP。
    std::vector<uint8_t> withXmp;
    withXmp.reserve(jpeg.size() + payload + 4);
    withXmp.insert(withXmp.end(), {0xFF, 0xD8, 0xFF, 0xE1});
    withXmp.push_back(static_cast<uint8_t>((payload + 2) >> 8));
    withXmp.push_back(static_cast<uint8_t>((payload + 2) & 0xFF));
    withXmp.insert(withXmp.end(), kJpegXmpPrefix, kJpegXmpPrefix + sizeof(kJpegXmpPrefix));
    withXmp.insert(withXmp.end(), xmp.begin(), xmp.end());
    withXmp.insert(withXmp.end(), jpeg.begin() + 2, jpeg.end());
    c.stream = memStream(withXmp);
    if (!c.stream || FAILED(factory->CreateDecoderFromStream(c.stream.Get(), nullptr, WICDecodeMetadataCacheOnLoad, &c.decoder)) ||
        FAILED(c.decoder->GetFrame(0, &c.frame))) {
        c = XmpCarrier{};
    }
    return c;
}

// 來源檔裡可以搬的東西。WIC 來源保留 decoder / frame（同容器整包複製要用），WebP 來源是三個 chunk。
struct SourceMeta {
    bool valid = false;
    ImageFormat format = ImageFormat::Unknown; // 來源檔內容的格式
    ComPtr<IWICBitmapDecoder> decoder;
    ComPtr<IWICBitmapFrameDecode> frame;
    WebPChunks chunks;        // WebP 來源的 chunk；WIC 來源時 icc 也會填
    XmpCarrier xmpCarrier;    // WebP 來源帶 XMP 時：包著那段 XMP 的 JPEG（見 makeXmpCarrier）
    int width = 0, height = 0; // 來源檔裡實際儲存的像素大小（沒套 orientation）
    std::wstring path;

    bool webp() const { return format == ImageFormat::WebP; }
};

SourceMeta readSourceMeta(IWICImagingFactory* factory, const std::wstring& path) {
    SourceMeta m;
    if (path.empty()) return m;
    m.path = path;
    if (detectImageFormat(path) == ImageFormat::WebP) {
        std::vector<uint8_t> bytes;
        if (!readFileBytes(path, bytes)) return m;
        m.chunks = readWebPChunks(bytes);
        int w = 0, h = 0;
        if (WebPGetInfo(bytes.data(), bytes.size(), &w, &h)) { m.width = w; m.height = h; }
        m.format = ImageFormat::WebP;
        m.xmpCarrier = makeXmpCarrier(factory, m.chunks.xmp);
        m.valid = true;
        return m;
    }
    if (!factory) return m;
    if (FAILED(factory->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &m.decoder))) return m;
    if (FAILED(m.decoder->GetFrame(0, &m.frame))) return m;
    GUID container{};
    if (SUCCEEDED(m.decoder->GetContainerFormat(&container))) m.format = formatOfContainer(container);
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

// 要搬到 WebP 的 EXIF / XMP 位元組：WebP 來源是現成的 chunk，JPEG 來源掃 APP1 段。
// 其他來源做不到（metadataCarried 對這些組合回 false）。
SourceBytes bytesForWebP(const SourceMeta& m) {
    SourceBytes out;
    if (m.webp()) { out.exif = m.chunks.exif; out.xmp = m.chunks.xmp; return out; }
    if (m.format != ImageFormat::Jpeg) return out;
    std::vector<uint8_t> file;
    if (!readFileBytes(m.path, file)) return out;
    out.exif = jpegApp1Payload(file, "Exif\0\0", 6);
    out.xmp = jpegApp1Payload(file, kJpegXmpPrefix, sizeof(kJpegXmpPrefix)); // sizeof 含結尾的 \0，正好是前綴長度
    return out;
}

// query reader 底下 path 那個子區塊，轉成可以塞進別的 writer 的 query writer。沒有那個區塊回空的。
ComPtr<IWICMetadataQueryWriter> subtreeWriter(IWICImagingFactory* factory, IWICMetadataQueryReader* src, const wchar_t* path) {
    ComPtr<IWICMetadataQueryWriter> w;
    if (!src) return w;
    PropVar v;
    if (SUCCEEDED(src->GetMetadataByName(path, &v.v)) && v.v.vt == VT_UNKNOWN && v.v.punkVal) {
        ComPtr<IWICMetadataQueryReader> sub;
        if (SUCCEEDED(v.v.punkVal->QueryInterface(IID_PPV_ARGS(&sub)))) factory->CreateQueryWriterFromReader(sub.Get(), nullptr, &w);
    }
    return w;
}

// 把一個 query writer 當成子區塊設到 dst 的 path。
bool setSubtree(IWICMetadataQueryWriter* dst, const wchar_t* path, IWICMetadataQueryWriter* sub) {
    PROPVARIANT v; // 借用 sub，不擁有：不能 PropVariantClear
    PropVariantInit(&v);
    v.vt = VT_UNKNOWN;
    v.punkVal = sub;
    return SUCCEEDED(dst->SetMetadataByName(path, &v));
}

// TIFF 來源的 IFD0 帶著描述像素排列的欄位（StripOffsets、Compression…），搬進別的檔就不對了；
// XMP（700）與 ICC（34675）另外走自己的路。
constexpr unsigned kTiffStructureTags[] = {
    254,   // NewSubfileType
    255,   // SubfileType
    256,   // ImageWidth
    257,   // ImageLength
    258,   // BitsPerSample
    259,   // Compression
    262,   // PhotometricInterpretation
    263,   // Threshholding
    264,   // CellWidth
    265,   // CellLength
    266,   // FillOrder
    273,   // StripOffsets
    277,   // SamplesPerPixel
    278,   // RowsPerStrip
    279,   // StripByteCounts
    280,   // MinSampleValue
    281,   // MaxSampleValue
    284,   // PlanarConfiguration
    317,   // Predictor
    320,   // ColorMap
    322,   // TileWidth
    323,   // TileLength
    324,   // TileOffsets
    325,   // TileByteCounts
    330,   // SubIFDs
    338,   // ExtraSamples
    339,   // SampleFormat
    340,   // SMinSampleValue
    341,   // SMaxSampleValue
    347,   // JPEGTables
    700,   // XMP
    34675, // ICC profile
};

// 在編碼器 frame 上套 metadata。frame 已 Initialize，還沒 SetSize。
// 同容器：整包複製；跨容器：照 metadataCarried 搬 EXIF（/ifd）與 XMP；ICC 走 color context。
// 回傳 false：有東西該帶卻沒寫進去（檔案照存，呼叫端回報 metadataDropped）。
bool applyMetadata(IWICImagingFactory* factory, IWICBitmapFrameEncode* frame, ImageFormat target, const SourceMeta& m,
                   int newW, int newH, bool sizeChanged) {
    if (!m.valid) return true;
    const bool toJpeg = target == ImageFormat::Jpeg, toTiff = target == ImageFormat::Tiff, toPng = target == ImageFormat::Png;
    if (target == ImageFormat::Bmp) return true;
    bool complete = true;

    bool copied = false;
    if (!m.webp() && m.format == target) {
        ComPtr<IWICMetadataBlockWriter> bw;
        ComPtr<IWICMetadataBlockReader> br;
        if (SUCCEEDED(frame->QueryInterface(IID_PPV_ARGS(&bw))) && SUCCEEDED(m.frame->QueryInterface(IID_PPV_ARGS(&br))))
            copied = SUCCEEDED(bw->InitializeFromBlockReader(br.Get()));
        if (!copied) complete = false; // 退到下面的跨容器搬法，IPTC 這類區塊帶不過去
    }
    ComPtr<IWICMetadataQueryWriter> qw;
    frame->GetMetadataQueryWriter(&qw);
    if (!copied && qw && (toJpeg || toTiff || toPng)) {
        ComPtr<IWICComponentFactory> cf;
        factory->QueryInterface(IID_PPV_ARGS(&cf));
        ComPtr<IWICMetadataQueryReader> srcQ;
        if (m.frame) m.frame->GetMetadataQueryReader(&srcQ);
        // EXIF：WIC 來源從 query reader 的 IFD 子區塊轉成 writer；WebP 來源從 EXIF 位元組建 reader，逐項複製到新的 IFD writer。
        if ((toJpeg || toTiff) && metadataCarried(m.format, target, MetaSectionId::Exif)) {
            ComPtr<IWICMetadataQueryWriter> ifdW;
            bool had = false;
            if (!m.webp()) {
                ifdW = subtreeWriter(factory, srcQ.Get(), L"/app1/ifd");
                if (!ifdW) ifdW = subtreeWriter(factory, srcQ.Get(), L"/ifd");
            } else if (!m.chunks.exif.empty()) {
                had = true;
                if (auto app1 = cf ? app1ReaderFromExif(factory, m.chunks.exif) : nullptr) {
                    PropVar schema, id, value;
                    ComPtr<IWICMetadataReader> ifdR;
                    if (SUCCEEDED(app1->GetValueByIndex(0, &schema.v, &id.v, &value.v)) && value.v.vt == VT_UNKNOWN && value.v.punkVal)
                        value.v.punkVal->QueryInterface(IID_PPV_ARGS(&ifdR));
                    if (ifdR && SUCCEEDED(cf->CreateQueryWriter(GUID_MetadataFormatIfd, nullptr, &ifdW))) copyIfdTree(cf.Get(), ifdR.Get(), ifdW.Get());
                }
            }
            if (ifdW) {
                for (unsigned tag : kTiffStructureTags) ifdW->RemoveMetadataByName((L"/{ushort=" + std::to_wstring(tag) + L"}").c_str());
                if (!setSubtree(qw.Get(), toJpeg ? L"/app1/ifd" : L"/ifd", ifdW.Get())) complete = false;
            } else if (had) {
                complete = false;
            }
        }
        // XMP 在 JPEG 是獨立區塊（/xmp），在 TIFF 是 IFD0 的 700 號欄位（/ifd/xmp）。PNG 不帶（見 metadataCarried）。
        // WebP 來源：把 XMP chunk 包進一張記憶體裡的 JPEG，從它的 /xmp 拿。
        if (metadataCarried(m.format, target, MetaSectionId::Xmp)) {
            ComPtr<IWICMetadataQueryWriter> xmpW;
            bool had = false;
            if (!m.webp()) {
                xmpW = subtreeWriter(factory, srcQ.Get(), L"/xmp");
                if (!xmpW) xmpW = subtreeWriter(factory, srcQ.Get(), L"/ifd/xmp");
            } else if (!m.chunks.xmp.empty()) {
                had = true;
                ComPtr<IWICMetadataQueryReader> carrierQ;
                if (m.xmpCarrier.frame && SUCCEEDED(m.xmpCarrier.frame->GetMetadataQueryReader(&carrierQ)))
                    xmpW = subtreeWriter(factory, carrierQ.Get(), L"/xmp");
            }
            if (xmpW) {
                if (!setSubtree(qw.Get(), toTiff ? L"/ifd/xmp" : L"/xmp", xmpW.Get())) complete = false;
            } else if (had) {
                complete = false;
            }
        }
    }
    // ICC
    if (!m.chunks.icc.empty() && factory && iccCarried(m.format, target)) {
        ComPtr<IWICColorContext> cc;
        IWICColorContext* raw = nullptr;
        if (FAILED(factory->CreateColorContext(&cc)) ||
            FAILED(cc->InitializeFromMemory(m.chunks.icc.data(), static_cast<UINT>(m.chunks.icc.size()))) ||
            (raw = cc.Get(), FAILED(frame->SetColorContexts(1, &raw))))
            complete = false;
    }
    // 與像素不符的欄位：orientation 一律 1，尺寸變了改 Exif 的 PixelX/YDimension。
    if (qw && (toJpeg || toTiff)) {
        const wchar_t* ifd = toJpeg ? L"/app1/ifd" : L"/ifd";
        std::wstring key = std::wstring(ifd) + L"/{ushort=274}";
        {
            PropVar old;
            if (SUCCEEDED(qw->GetMetadataByName(key.c_str(), &old.v))) {
                PropVar one;
                one.v.vt = VT_UI2;
                one.v.uiVal = 1;
                qw->SetMetadataByName(key.c_str(), &one.v);
            }
        }
        if (sizeChanged) {
            for (auto [tag, val] : {std::pair{40962u, newW}, std::pair{40963u, newH}}) {
                std::wstring k = std::wstring(ifd) + L"/exif/{ushort=" + std::to_wstring(tag) + L"}";
                PropVar old;
                if (SUCCEEDED(qw->GetMetadataByName(k.c_str(), &old.v))) {
                    PropVar nv;
                    nv.v.vt = VT_UI4;
                    nv.v.ulVal = static_cast<ULONG>(val);
                    qw->SetMetadataByName(k.c_str(), &nv.v);
                }
            }
        }
        // 內嵌縮圖：舊的丟掉（IFD1），存檔時由 frame->SetThumbnail 重新產生。
        for (const wchar_t* t : {L"/app1/thumb", L"/app1/ifd/thumb", L"/app1/{ushort=1}", L"/ifd/thumb"}) qw->RemoveMetadataByName(t);
    }
    return complete;
}

// 存檔用的縮圖（最長邊 160）。
Image makeThumbnail(const Image& img) {
    const int maxSide = 160;
    float s = static_cast<float>(maxSide) / std::max(img.width, img.height);
    if (s >= 1.0f) return img;
    return resample(img, std::max(1, static_cast<int>(img.width * s)), std::max(1, static_cast<int>(img.height * s)));
}

LoadResult loadWebP(const std::wstring& path) {
    LoadResult r;
    r.info.format = ImageFormat::WebP;
    std::vector<uint8_t> bytes;
    if (!readFileBytes(path, bytes)) { r.error = failure(IoErrorCode::FileUnreadable); return r; }
    WebPBitstreamFeatures feat{};
    if (WebPGetFeatures(bytes.data(), bytes.size(), &feat) != VP8_STATUS_OK) { r.error = failure(IoErrorCode::InvalidWebP); return r; }
    auto img = std::make_shared<Image>(feat.width, feat.height);
    WebPDecoderConfig config;
    if (!WebPInitDecoderConfig(&config)) { r.error = failure(IoErrorCode::WebPInitFailed); return r; }
    config.output.colorspace = MODE_bgrA; // premultiplied BGRA
    config.output.is_external_memory = 1;
    config.output.u.RGBA.rgba = img->pixels.data();
    config.output.u.RGBA.stride = static_cast<int>(img->stride());
    config.output.u.RGBA.size = img->pixels.size();
    if (WebPDecode(bytes.data(), bytes.size(), &config) != VP8_STATUS_OK) { r.error = failure(IoErrorCode::WebPDecodeFailed); return r; }
    r.info.colorModel = ColorModel::Rgb;
    r.info.bitsPerPixel = feat.has_alpha ? 32 : 24;
    // 容器裡的 EXIF / XMP / ICCP chunk：資訊面板要顯示，orientation 也要套。
    WebPChunks chunks = readWebPChunks(bytes);
    if (!chunks.exif.empty() || !chunks.xmp.empty()) {
        auto factory = makeFactory();
        if (!chunks.exif.empty())
            if (auto reader = app1ReaderFromExif(factory.Get(), chunks.exif)) walkReader(reader.Get(), r.info.sections, 0);
        if (!chunks.xmp.empty())
            if (auto reader = metadataReaderFromBytes(factory.Get(), GUID_MetadataFormatXMP, chunks.xmp)) walkReader(reader.Get(), r.info.sections, 0);
    }
    if (!chunks.icc.empty()) r.info.colorProfile = iccProfileDescription(chunks.icc.data(), chunks.icc.size());
    dropEmptySections(r.info.sections);
    r.info.orientation = exifOrientation(chunks.exif.data(), chunks.exif.size());
    if (r.info.orientation >= 2) img = std::make_shared<Image>(applyExifOrientation(*img, r.info.orientation));
    r.image = img;
    return r;
}

// WebP 檔（h 已開、位置不拘）的 EXIF chunk 裡的 Orientation。只讀各 chunk 的標頭，跳過影像資料。沒有回 1。
int webpOrientation(HANDLE h) {
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(h, &size)) return 1;
    uint64_t pos = 12; // "RIFF" + 大小 + "WEBP"
    for (int guard = 0; guard < 64 && pos + 8 <= static_cast<uint64_t>(size.QuadPart); ++guard) {
        LARGE_INTEGER to;
        to.QuadPart = static_cast<LONGLONG>(pos);
        uint8_t head[8];
        DWORD got = 0;
        if (!SetFilePointerEx(h, to, nullptr, FILE_BEGIN) || !ReadFile(h, head, 8, &got, nullptr) || got != 8) return 1;
        uint32_t len = head[4] | (head[5] << 8) | (head[6] << 16) | (static_cast<uint32_t>(head[7]) << 24);
        if (memcmp(head, "EXIF", 4) == 0) {
            std::vector<uint8_t> exif(std::min<uint32_t>(len, 1u << 16)); // Orientation 在 IFD0，開頭就有
            if (!ReadFile(h, exif.data(), static_cast<DWORD>(exif.size()), &got, nullptr) || got != exif.size()) return 1;
            exif = stripExifPrefix(exif.data(), exif.size());
            return exifOrientation(exif.data(), exif.size());
        }
        pos += 8 + static_cast<uint64_t>(len) + (len & 1); // chunk 補到偶數長度
    }
    return 1;
}

bool writeFileBytes(const std::wstring& path, const uint8_t* data, size_t size, DWORD& lastError) {
    lastError = 0;
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) { lastError = GetLastError(); return false; }
    size_t done = 0;
    while (done < size) {
        DWORD chunk = static_cast<DWORD>(std::min<size_t>(size - done, 1u << 26));
        DWORD put = 0;
        if (!WriteFile(h, data + done, chunk, &put, nullptr) || put == 0) { lastError = GetLastError(); break; }
        done += put;
    }
    CloseHandle(h);
    return done == size;
}

// 把 EXIF / XMP / ICCP chunk 組進編好的 WebP。組不起來回 false（out 不動）。
bool muxWebP(const uint8_t* bitstream, size_t size, const std::vector<uint8_t>& icc, const std::vector<uint8_t>& exif,
             const std::vector<uint8_t>& xmp, std::vector<uint8_t>& out) {
    WebPData data{bitstream, size};
    WebPMux* mux = WebPMuxCreate(&data, 0);
    if (!mux) return false;
    bool ok = true;
    auto set = [&](const char* fourcc, const std::vector<uint8_t>& v) {
        if (v.empty()) return;
        WebPData d{v.data(), v.size()};
        if (WebPMuxSetChunk(mux, fourcc, &d, 1) != WEBP_MUX_OK) ok = false;
    };
    set("ICCP", icc);
    set("EXIF", exif);
    set("XMP ", xmp);
    WebPData assembled{nullptr, 0};
    if (ok && WebPMuxAssemble(mux, &assembled) == WEBP_MUX_OK) out.assign(assembled.bytes, assembled.bytes + assembled.size);
    else ok = false;
    WebPDataClear(&assembled);
    WebPMuxDelete(mux);
    return ok;
}

IoError saveWebP(const StraightImage& straight, const std::wstring& tmpPath, const SourceMeta& meta, bool sizeChanged,
                 bool& metadataDropped) {
    uint8_t* encoded = nullptr;
    size_t n = WebPEncodeBGRA(straight.pixels.data(), straight.width, straight.height,
                              static_cast<int>(straight.stride()), 90.0f, &encoded);
    if (n == 0 || !encoded) return failure(IoErrorCode::WebPEncodeFailed);
    // 有 metadata 要帶就用 mux 把 EXIF / XMP / ICCP chunk 組進容器。組不起來時圖照存，只回報 metadata 沒帶到。
    std::vector<uint8_t> withMeta;
    if (meta.valid) {
        SourceBytes sb = bytesForWebP(meta);
        if (!sb.exif.empty()) patchExifTiff(sb.exif, sizeChanged, straight.width, straight.height);
        if (!sb.exif.empty() || !sb.xmp.empty() || !meta.chunks.icc.empty()) {
            if (!muxWebP(encoded, n, meta.chunks.icc, sb.exif, sb.xmp, withMeta)) {
                withMeta.clear();
                metadataDropped = true;
            }
        }
    }
    DWORD lastError = 0;
    bool ok = withMeta.empty() ? writeFileBytes(tmpPath, encoded, n, lastError)
                               : writeFileBytes(tmpPath, withMeta.data(), withMeta.size(), lastError);
    WebPFree(encoded);
    return ok ? IoError{} : systemFailure(IoErrorCode::WriteFailed, lastError);
}

IoError saveWithWic(IWICImagingFactory* factory, const StraightImage& straight, const std::wstring& tmpPath, ImageFormat target,
                    bool keepAlpha, const SourceMeta& meta, bool sizeChanged, const StraightImage* thumbnail,
                    bool& metadataDropped) {
    GUID container{};
    if (!containerOfFormat(target, container)) return failure(IoErrorCode::CreateEncoderFailed);

    ComPtr<IWICStream> stream;
    HRESULT hr = factory->CreateStream(&stream);
    if (SUCCEEDED(hr)) hr = stream->InitializeFromFilename(tmpPath.c_str(), GENERIC_WRITE);
    if (FAILED(hr)) return failure(IoErrorCode::CreateFileFailed, hr);

    ComPtr<IWICBitmapEncoder> encoder;
    hr = factory->CreateEncoder(container, nullptr, &encoder);
    if (SUCCEEDED(hr)) hr = encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache);
    if (FAILED(hr)) return failure(IoErrorCode::CreateEncoderFailed, hr);

    ComPtr<IWICBitmapFrameEncode> frame;
    ComPtr<IPropertyBag2> props;
    hr = encoder->CreateNewFrame(&frame, &props);
    if (FAILED(hr)) return failure(IoErrorCode::CreateFrameFailed, hr);
    if (props && target == ImageFormat::Jpeg) {
        PROPBAG2 opt{};
        wchar_t name[] = L"ImageQuality";
        opt.pstrName = name;
        VARIANT v;
        VariantInit(&v);
        v.vt = VT_R4;
        v.fltVal = 0.9f;
        hr = props->Write(1, &opt, &v);
        if (FAILED(hr)) return failure(IoErrorCode::FrameSetupFailed, hr); // 品質設不進去時不要用預設品質默默存
    }
    hr = frame->Initialize(props.Get());
    if (SUCCEEDED(hr) && meta.valid &&
        !applyMetadata(factory, frame.Get(), target, meta, straight.width, straight.height, sizeChanged))
        metadataDropped = true;
    if (SUCCEEDED(hr)) hr = frame->SetSize(static_cast<UINT>(straight.width), static_cast<UINT>(straight.height));
    if (SUCCEEDED(hr)) hr = frame->SetResolution(straight.dpiX, straight.dpiY);
    if (FAILED(hr)) return failure(IoErrorCode::FrameSetupFailed, hr);
    if (thumbnail) {
        // EXIF 縮圖從編輯後的圖重新產生；不支援的編碼器會失敗，忽略。
        ComPtr<IWICBitmap> tb;
        if (SUCCEEDED(factory->CreateBitmapFromMemory(static_cast<UINT>(thumbnail->width), static_cast<UINT>(thumbnail->height),
                                                      GUID_WICPixelFormat32bppBGRA, static_cast<UINT>(thumbnail->stride()),
                                                      static_cast<UINT>(thumbnail->pixels.size()),
                                                      const_cast<BYTE*>(thumbnail->pixels.data()), &tb)))
            frame->SetThumbnail(tb.Get());
    }

    GUID want = keepAlpha ? GUID_WICPixelFormat32bppBGRA : GUID_WICPixelFormat24bppBGR;
    hr = frame->SetPixelFormat(&want); // 編碼器不支援時會改成它支援的格式，WriteSource 會自動轉換
    if (FAILED(hr)) return failure(IoErrorCode::FrameSetupFailed, hr);

    ComPtr<IWICBitmap> bitmap;
    hr = factory->CreateBitmapFromMemory(static_cast<UINT>(straight.width), static_cast<UINT>(straight.height),
                                         GUID_WICPixelFormat32bppBGRA, static_cast<UINT>(straight.stride()),
                                         static_cast<UINT>(straight.pixels.size()),
                                         const_cast<BYTE*>(straight.pixels.data()), &bitmap);
    if (FAILED(hr)) return failure(IoErrorCode::CreateBitmapFailed, hr);

    hr = frame->WriteSource(bitmap.Get(), nullptr);
    if (SUCCEEDED(hr)) hr = frame->Commit();
    if (SUCCEEDED(hr)) hr = encoder->Commit();
    if (FAILED(hr)) return failure(IoErrorCode::WriteImageFailed, hr);
    return {};
}

bool isExifFamily(MetaSectionId id) {
    return id == MetaSectionId::Tiff || id == MetaSectionId::TiffSubIfd || id == MetaSectionId::Exif ||
           id == MetaSectionId::Gps || id == MetaSectionId::Interop;
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
    // off 來自檔案，可能接近 4 GB：用 size_t 算，32 位元的 off + 2 會繞回去。
    auto validIfd = [&](size_t off) { return off >= 8 && off + 2 <= t.size() && off + 2 + static_cast<size_t>(rd16(off)) * 12 + 4 <= t.size(); };
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

bool metadataCarried(ImageFormat source, ImageFormat target, MetaSectionId section) {
    if (source != ImageFormat::Unknown && source == target) return true;
    const bool exif = isExifFamily(section);
    switch (target) {
    case ImageFormat::Jpeg:
        return exif || section == MetaSectionId::Xmp || section == MetaSectionId::Iptc ||
               section == MetaSectionId::Photoshop || section == MetaSectionId::Jfif;
    case ImageFormat::Tiff:
        return exif || section == MetaSectionId::Xmp;
    case ImageFormat::WebP:
        // 只有 JPEG 來源搬得動：直接搬 APP1 區段的位元組。
        return (exif || section == MetaSectionId::Xmp) && source == ImageFormat::Jpeg;
    case ImageFormat::Png:
        return false; // WIC 的 PNG 編碼器沒有 /xmp 區塊（設進去會失敗），只帶得過去 ICC
    default:
        return false; // BMP 什麼都不裝
    }
}

bool iccCarried(ImageFormat source, ImageFormat target) {
    if (source != ImageFormat::Unknown && source == target) return true;
    return target == ImageFormat::Jpeg || target == ImageFormat::Tiff || target == ImageFormat::WebP ||
           target == ImageFormat::Png;
}

std::vector<std::wstring> metadataLossOnSave(const ImageInfo& info, ImageFormat target) {
    std::vector<std::wstring> lost;
    bool exifLost = false;
    for (const auto& sec : info.sections) {
        if (metadataCarried(info.format, target, sec.id)) continue;
        if (isExifFamily(sec.id)) { // 這一家合併成一項
            if (!exifLost) lost.push_back(metaSectionName(MetaSectionId::Exif));
            exifLost = true;
            continue;
        }
        lost.push_back(metaSectionName(sec.id));
    }
    if (!info.colorProfile.empty() && !iccCarried(info.format, target)) lost.push_back(L"ICC");
    return lost;
}

LoadResult loadImage(const std::wstring& path) {
    try {
        ComInit com;
        if (detectImageFormat(path) == ImageFormat::WebP) return loadWebP(path);
        return loadWithWic(path);
    } catch (const std::bad_alloc&) {
        // 檔頭宣稱的尺寸配不出記憶體（超大圖，或刻意構造的檔）。背景執行緒也會走到這裡，不能讓例外飛出去。
        LoadResult r;
        r.error = failure(IoErrorCode::OutOfMemory);
        return r;
    }
}

bool readImageSize(const std::wstring& path, int& width, int& height) {
    width = height = 0;
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    uint8_t head[64];
    DWORD got = 0;
    if (!ReadFile(h, head, sizeof(head), &got, nullptr)) got = 0;
    int orientation = 1;
    unsigned w = 0, hh = 0;
    if (sniffImageFormat(head, got) == ImageFormat::WebP) {
        // WebPGetInfo 只看檔頭，64 bytes 就夠。帶 EXIF 的檔一定是 VP8X 容器，旗標（第 20 byte）的 0x08 表示有 EXIF chunk。
        int ww = 0, wh = 0;
        bool ok = WebPGetInfo(head, got, &ww, &wh) && ww > 0 && wh > 0;
        if (ok && got > 20 && memcmp(head + 12, "VP8X", 4) == 0 && (head[20] & 0x08)) orientation = webpOrientation(h);
        CloseHandle(h);
        if (!ok) return false;
        w = static_cast<unsigned>(ww);
        hh = static_cast<unsigned>(wh);
    } else {
        CloseHandle(h);
        ComInit com;
        auto factory = makeFactory();
        if (!factory) return false;
        ComPtr<IWICBitmapDecoder> decoder;
        if (FAILED(factory->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &decoder))) return false;
        ComPtr<IWICBitmapFrameDecode> frame;
        if (FAILED(decoder->GetFrame(0, &frame))) return false;
        if (FAILED(frame->GetSize(&w, &hh)) || w == 0 || hh == 0) return false;
        orientation = orientationOf(frame.Get());
    }
    bool swap = orientation >= 5 && orientation <= 8;
    width = static_cast<int>(swap ? hh : w);
    height = static_cast<int>(swap ? w : hh);
    return true;
}

SaveResult saveImage(const Image& img, const std::wstring& path, const SaveOptions& opt) {
    SaveResult res;
    const std::wstring ext = extensionOf(path);
    const ImageFormat target = imageFormatOfExtension(ext);
    const ImageFormatInfo* format = imageFormatInfo(target);
    if (!format || !format->canSave) {
        res.error = failure(IoErrorCode::UnsupportedSaveFormat);
        res.error.detail = ext;
        return res;
    }
    const std::wstring tmp = path + L".mp-saving.tmp";
    try {
        ComInit com;
        auto factory = makeFactory();
        if (!factory && target != ImageFormat::WebP) { res.error = failure(IoErrorCode::WicUnavailable); return res; }

        // 來源 metadata。EXIF 的 PixelX/YDimension 指的是檔案裡實際的像素格，所以跟檔案裡的寬高比
        //（orientation 轉正過的圖，寬高對調了，也算變）。
        SourceMeta meta = readSourceMeta(factory.Get(), opt.metadataSource);
        const bool sizeChanged = meta.valid && (meta.width != img.width || meta.height != img.height);

        Image work = img;
        // JPEG 存不了 alpha；WIC 寫的 BMP 沒有 alpha mask，讀回當成不透明。直接丟掉 alpha 時透明處會變黑，所以先鋪白底。
        if (!format->keepsTransparency) compositeOnWhite(work);
        // 縮圖要在還是 premultiplied 的時候縮（resample 假設 premultiplied）。
        std::optional<StraightImage> thumbnail;
        if (meta.valid && (target == ImageFormat::Jpeg || target == ImageFormat::Tiff)) thumbnail = toStraight(makeThumbnail(work));
        const StraightImage straight = toStraight(std::move(work));

        if (target == ImageFormat::WebP) {
            res.error = saveWebP(straight, tmp, meta, sizeChanged, res.metadataDropped);
        } else {
            const bool keepAlpha = target != ImageFormat::Jpeg && hasTransparency(straight);
            res.error = saveWithWic(factory.Get(), straight, tmp, target, keepAlpha, meta, sizeChanged,
                                    thumbnail ? &*thumbnail : nullptr, res.metadataDropped);
        }
        // 來源檔常常就是要覆寫的那個檔：decoder 還握著它時 MoveFileEx 會失敗，先放掉。
        meta = SourceMeta{};
    } catch (const std::bad_alloc&) {
        res.error = failure(IoErrorCode::OutOfMemory);
    }
    if (!res.ok()) {
        DeleteFileW(tmp.c_str());
        return res;
    }
    if (!MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        res.error = systemFailure(IoErrorCode::ReplaceFailed, GetLastError());
        DeleteFileW(tmp.c_str());
    }
    return res;
}

} // namespace mp
