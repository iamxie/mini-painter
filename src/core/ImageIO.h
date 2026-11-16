#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "core/Image.h"
#include "core/ImageFormat.h"
#include "core/IoError.h"
#include "core/Metadata.h"

namespace mp {

struct LoadResult {
    ImagePtr image;  // 失敗時為 nullptr
    Dpi dpi;         // 檔案裡記的解析度（和圖同一個方向）；沒記或不合理時是 96
    ImageInfo info;
    IoError error;   // 失敗原因
};

// 這個檔的函式可以在任何執行緒呼叫。第一次在某條執行緒用到 WIC 時會初始化那條執行緒的 COM（單執行緒 apartment）
// 並建一個 WIC factory，之後同一條執行緒重用，執行緒結束時放掉。

// 讀檔。格式看檔案內容，不看副檔名：WebP 走 libwebp，其他走 WIC（BMP、TIFF、JPEG、PNG）。
// EXIF orientation 會套用到像素上（info.orientation 仍記錄原值）。
// 不丟例外：圖太大、記憶體配不出來時回 OutOfMemory。
LoadResult loadImage(const std::wstring& path);

// 只讀尺寸，不解碼像素（兩頁並排要知道每頁寬高）。EXIF orientation 會轉過來（5～8 時寬高對調），
// 和 loadImage 給的圖同一個方向。讀不到回傳 false。
bool readImageSize(const std::wstring& path, int& width, int& height);

struct SaveOptions {
    // 要照抄 metadata 的來源檔（通常是這張圖原本的檔）。空字串 = 不保留。
    // 全部照抄，只改與像素不符的：Orientation 一律 1、尺寸變了改 PixelX/YDimension、EXIF 縮圖重新產生。
    // 跨格式時帶得過去的範圍見 metadataCarried。
    std::wstring metadataSource;
    // 寫進檔案的解析度（WebP 沒有這個欄位）。
    Dpi dpi;
};

struct SaveResult {
    IoError error;
    // 檔案存好了（像素正確），但來源裡照 metadataCarried 該帶過去的 metadata 有一部分沒寫進去。
    bool metadataDropped = false;

    bool ok() const { return error.ok(); }
    explicit operator bool() const { return ok(); }
};

// 存檔。格式依副檔名決定。先寫到暫存檔再取代原檔，失敗不動原檔。JPEG 品質固定 0.9，WebP 有損、品質 90。
// 存不了透明的格式（見 extensionKeepsTransparency）先疊在白底上。不丟例外。
// 記憶體：不透明的圖直接拿來編碼，不複製；有透明的圖要換算像素，這個版本會複製一份。
SaveResult saveImage(const Image& img, const std::wstring& path, const SaveOptions& opt = {});
// 接手 img 的版本：有透明時直接在 img 上換算，不另外複製。呼叫後 img 的內容不能再用。
SaveResult saveImage(Image&& img, const std::wstring& path, const SaveOptions& opt = {});

// source 格式的檔存成 target 格式時，section 這種區塊帶不帶得過去。同格式全部帶。
// 存檔（saveImage）與事前的警告（metadataLossOnSave）都看這一個函式。
bool metadataCarried(ImageFormat source, ImageFormat target, MetaSectionId section);
// ICC profile 帶不帶得過去（BMP 不行，其他都行）。
bool iccCarried(ImageFormat source, ImageFormat target);

// 另存成 target 格式時，來源有而帶不過去的區塊名（英文翻譯 key，例如 L"Exif"、L"IPTC"、L"ICC"）。
// TIFF / Exif / GPS / Interoperability 這一家合併成一項 L"Exif"。同格式回傳空。info 是來源檔讀出來的資訊。
std::vector<std::wstring> metadataLossOnSave(const ImageInfo& info, ImageFormat target);

// 手動修補 EXIF（TIFF 結構的位元組）：Orientation 改 1、sizeChanged 時改 Exif 的 PixelXDimension / PixelYDimension、
// 切掉 IFD1（內嵌縮圖）。給 WebP 路徑用；結構不合法時原樣不動。公開是為了測試。
void patchExifTiff(std::vector<uint8_t>& tiff, bool sizeChanged, int newW, int newH);

} // namespace mp
