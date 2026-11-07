#pragma once
#include <string>
#include <utility>
#include <vector>

namespace mp {

// 資訊面板的一個區塊，例如「Exif」。
struct MetaSection {
    std::wstring name;
    std::vector<std::pair<std::wstring, std::wstring>> items;
};

// 從檔案讀出來的描述資料。
struct ImageInfo {
    std::wstring formatName;      // 例如 L"JPEG 影像"
    std::wstring colorModel;      // 例如 L"RGB"、L"灰階"
    int bitsPerPixel = 0;         // 原始像素格式的位元深度
    int orientation = 1;          // EXIF orientation，沒有就是 1
    std::wstring colorProfile;    // ICC 描述；沒有嵌入就是空字串
    std::vector<MetaSection> sections; // Exif、PNG、TIFF 等，依檔案實際內容
};

// 把 ICC profile 的 desc tag 解出來。失敗回傳空字串。
std::wstring iccProfileDescription(const unsigned char* data, size_t size);

// 常見 Exif / TIFF tag id 的名稱（已依目前語系翻譯）。沒有對應時回傳空字串。
std::wstring exifTagName(unsigned tag);

// 把檔案大小格式化成「1.2 MB」這類字串。
std::wstring formatFileSize(unsigned long long bytes);

} // namespace mp
