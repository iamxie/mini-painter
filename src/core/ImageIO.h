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

// 讀檔。格式看檔案內容，不看副檔名：WebP 走 libwebp，BMP、TIFF、JPEG、PNG 走 WIC；內建認不得的與 HEIF 先問生效中的
// 讀圖 plugin（core/Plugins.h，info.pluginFormat 記格式名），plugin 也不認得的交給 WIC（系統裝的 codec）。
// HEIF 走 WIC 時由系統的解碼器套用 irot / imir（EXIF 裡的方向欄位它會拿掉，orientation 是 1）；透明會被丟掉
// （WIC 的 HEIF 解碼器只給 32bppBGR）。缺擴充時錯誤代碼見 classifyHeifFailure。
// EXIF orientation 會套用到像素上（info.orientation 仍記錄原值；plugin 解的由 plugin 轉，orientation 是 1）。
// 不丟例外：圖太大、記憶體配不出來時回 OutOfMemory。
LoadResult loadImage(const std::wstring& path);

// 快速解碼（瀏覽資料夾的縮圖與預覽：要求是快，不講究縮圖品質）。縮到 maxW × maxH 以內（不放大），EXIF 方向轉正。
// WIC 的解碼器能直接縮小解的（JPEG 可以 1/2、1/4、1/8）就縮小解，其他先解整張再用 resampleFast 縮。
// 格式的判斷和 loadImage 一樣（WebP、plugin、WIC；HEIF 缺擴充時的錯誤代碼也一樣）。
struct ScaledDecode {
    ImagePtr image;            // 縮好的圖；失敗時為 nullptr
    int width = 0, height = 0; // 原圖（轉正後）的寬高
    ImageInfo info;            // withMetadata 是 false 時只有格式、色彩模型、位元深度、orientation
    IoError error;
};
ScaledDecode decodeScaled(const std::wstring& path, int maxW, int maxH, bool withMetadata);
// w × h 照比例縮進 maxW × maxH（不放大），每邊至少 1。
void fitWithin(int w, int h, int maxW, int maxH, int& outW, int& outH);
// 縮圖存進快取的位元組：不透明的存 JPEG，有透明的存 PNG。失敗回空的。
std::vector<uint8_t> encodeThumbnail(const Image& img);
// encodeThumbnail 的反向（WIC 解得開的都行）。失敗回 nullptr。
ImagePtr decodeThumbnail(const std::vector<uint8_t>& bytes);
// 讓這條執行緒初始化 COM（和這個檔的 WIC 共用同一份，執行緒結束時收掉）。背景執行緒用 shell 的 COM 物件之前呼叫。
void ensureThreadCom();

// HEIF 檔（檔頭是 HEIF）解不開時，看失敗的那一步與 HRESULT 判斷是不是缺 Microsoft Store 的擴充（2026-11-22 VM 實測）：
// - 建不出解碼器（DecodeFailed）且是 WINCODEC_ERR_COMPONENTINITIALIZEFAILURE（擴充移除後）或 COMPONENTNOTFOUND
//   （從沒裝過）：缺 HEIF 影像延伸模組，NeedHeifExtension。
// - 哪一步都好，HRESULT 是 MF_E_TOPO_CODEC_NOT_FOUND（實測在讀像素那一步）：缺影像編碼的解碼器，codec 是 Av1
//   回 NeedAv1Extension，其他回 NeedHevcExtension（HEIC 幾乎都是 HEVC；看不出 brand 時也這樣說）。
// - 其他原封不動（例如檔案損毀、10 位元解不開）。
// loadImage 用它改寫錯誤代碼，HRESULT 留著（給回報問題用）。公開是為了測試。
IoErrorCode classifyHeifFailure(IoErrorCode failed, long hresult, HeifCodec codec);

// 只讀尺寸，不解碼像素（兩頁並排要知道每頁寬高）。EXIF orientation 會轉過來（5～8 時寬高對調），
// 和 loadImage 給的圖同一個方向。讀不到回傳 false。plugin 的格式問 plugin 的 readSize。
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
// 存檔時，path 旁邊的暫存檔都叫 <path>.<行程編號>-<序號>.mp-saving.tmp：每次存檔一個新的名字，
// 兩個視窗同時存同一個檔也不會拿到同一個暫存檔；存完（成功或失敗）只刪自己的那一個。
// 存檔開始時順便刪掉同一個 path 的、名字裡的行程已經結束的暫存檔（存到一半當掉留下的）；還在跑的行程的不碰。

// 建好下一個暫存檔（空檔，用「已經有就失敗」的方式建，不會蓋掉別人的）並回傳路徑。建不起來回空字串，lastError 填原因。
std::wstring claimSaveTemp(const std::wstring& path, unsigned long& lastError);

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
