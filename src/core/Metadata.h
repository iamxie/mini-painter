#pragma once
#include <string>
#include <vector>

#include "core/ImageFormat.h"

namespace mp {

// 這裡的資料都不帶翻譯：名稱是英文翻譯 key，由 app 在顯示時翻（core 不呼叫 tr）。
// 區塊用 enum 認，不拿顯示名比對。

enum class ColorModel { Rgb, Grayscale, Indexed, Cmyk };
// 顯示名的英文翻譯 key：L"RGB"、L"Grayscale"、L"Indexed"、L"CMYK"。
const wchar_t* colorModelName(ColorModel model);

// 資訊面板的區塊。
enum class MetaSectionId { Tiff, TiffSubIfd, Exif, Gps, Interop, Xmp, Iptc, Photoshop, Jfif, Png, Other };
// 顯示名的英文翻譯 key，例如 L"Exif"、L"Interoperability"。
const wchar_t* metaSectionName(MetaSectionId id);

// 一個欄位的值。大部分是現成的文字；需要翻譯的幾種只帶數字，由 app 組句子。
struct MetaValue {
    enum class Kind {
        Text,        // text 就是要顯示的字
        ByteCount,   // 不展開的二進位資料：number 是 bytes 數
        Bool,        // number 是 0 或 1
        UnknownType, // 不認得的型別：number 是 VARTYPE
    };
    Kind kind = Kind::Text;
    std::wstring text;
    unsigned long long number = 0;
};

struct MetaItem {
    std::wstring label;      // labelIsKey：英文翻譯 key（例如 L"Make"）；否則照字面顯示（例如 L"Tag 0x9999"、L"gAMA 1"）
    bool labelIsKey = false;
    MetaValue value;
};

struct MetaSection {
    MetaSectionId id = MetaSectionId::Other;
    std::vector<MetaItem> items;
};

// 從檔案讀出來的描述資料。
struct ImageInfo {
    ImageFormat format = ImageFormat::Unknown; // 檔案內容實際的格式（看檔頭，不看副檔名）；不在支援清單裡的是 Unknown
    ColorModel colorModel = ColorModel::Rgb;
    int bitsPerPixel = 0;         // 原始像素格式的位元深度
    int orientation = 1;          // EXIF orientation（IFD0 的 274 號欄位），沒有就是 1
    std::wstring colorProfile;    // ICC 描述；沒有嵌入就是空字串
    std::vector<MetaSection> sections; // Exif、PNG、TIFF 等，依檔案實際內容；沒有空的區塊
};

// 把 ICC profile 的 desc tag 解出來。失敗回傳空字串。
std::wstring iccProfileDescription(const unsigned char* data, size_t size);

// 常見 Exif / TIFF tag id 的名稱（英文翻譯 key）。沒有對應時回傳 nullptr。
const wchar_t* exifTagName(unsigned tag);

// EXIF（TIFF 結構的位元組）裡 IFD0 的 Orientation。沒有、結構不合法、或值不在 1～8 回 1。
int exifOrientation(const unsigned char* tiff, size_t size);


} // namespace mp
