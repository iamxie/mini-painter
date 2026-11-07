#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "core/Image.h"
#include "core/Metadata.h"

namespace mp {

struct LoadResult {
    ImagePtr image;      // 失敗時為 nullptr
    ImageInfo info;
    std::wstring error;  // 失敗原因
};

// 讀檔。BMP、TIFF、JPEG、PNG 走 WIC，WebP 走 libwebp。
// EXIF orientation 會套用到像素上（info.orientation 仍記錄原值）。
LoadResult loadImage(const std::wstring& path);

// 只讀尺寸，不解碼像素（兩頁並排要知道每頁寬高）。EXIF orientation 會轉過來（6 / 8 時寬高對調）。
// 讀不到回傳 false。
bool readImageSize(const std::wstring& path, int& width, int& height);

struct SaveOptions {
    // 要照抄 metadata 的來源檔（通常是這張圖原本的檔）。空字串 = 不保留。
    // 全部照抄，只改與像素不符的：Orientation 一律 1、尺寸變了改 PixelX/YDimension、EXIF 縮圖重新產生。
    std::wstring metadataSource;
};

// 存檔。格式依副檔名決定。先寫到暫存檔再取代原檔。JPEG 品質固定 0.9。
bool saveImage(const Image& img, const std::wstring& path, std::wstring* error, const SaveOptions& opt = {});

// 另存成 targetExt 時，來源有而目標格式裝不下的區塊名（例如 L"Exif"、L"IPTC"、L"ICC"）。
// 同格式回傳空。info 是來源檔讀出來的資訊。
std::vector<std::wstring> metadataLossOnSave(const ImageInfo& info, const std::wstring& sourceExt, const std::wstring& targetExt);

// 手動修補 EXIF（TIFF 結構的位元組）：Orientation 改 1、sizeChanged 時改 Exif 的 PixelXDimension / PixelYDimension、
// 切掉 IFD1（內嵌縮圖）。給 WebP 路徑用；結構不合法時原樣不動。公開是為了測試。
void patchExifTiff(std::vector<uint8_t>& tiff, bool sizeChanged, int newW, int newH);

// 存檔用的副檔名是不是支援寫出的格式。
bool canSaveExtension(const std::wstring& ext);

} // namespace mp
