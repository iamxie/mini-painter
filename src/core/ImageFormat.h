#pragma once
#include <cstddef>
#include <string>
#include <vector>

namespace mp {

// 支援的圖片格式。「支援哪些格式、每種格式能做什麼」只寫在 ImageFormat.cpp 的那張表，
// 副檔名清單、存檔能力、檔頭辨識、檔案對話框的篩選都從表導出。加一種格式：加一個 enum、表裡加一列、
// ImageIO.cpp 補解碼 / 編碼。
enum class ImageFormat { Unknown, Bmp, Tiff, Jpeg, Png, WebP, Heif };

struct ImageFormatInfo {
    ImageFormat format;
    const wchar_t* name;                    // 顯示名的英文翻譯 key，例如 L"PNG image"（app 用 tr 翻）
    const wchar_t* shortName;               // 檔案對話框的篩選用，例如 L"PNG"
    std::vector<const wchar_t*> extensions; // 小寫、含點；第一個是存檔時補上的預設副檔名
    bool canSave;                           // 寫得出來。寫不出來的格式開起來唯讀，只能「輸出成」（isViewOnlyFormat）
    bool keepsTransparency;                 // 存檔時留得住透明
    // 交給系統另外裝的 WIC codec 解（HEIF：Microsoft Store 的擴充）。這種格式的副檔名不算內建：
    // 讀圖 plugin 可以宣告，生效中的 plugin 認得的檔先給 plugin（core/Plugins 的 askPluginsFirst）。
    bool systemCodec = false;
};

// 全部格式，固定順序（不含 Unknown）。
const std::vector<ImageFormatInfo>& imageFormats();
// 某個格式那一列；Unknown 回 nullptr。
const ImageFormatInfo* imageFormatInfo(ImageFormat format);
// 顯示名的英文翻譯 key；Unknown 是 L"Image"。
const wchar_t* imageFormatName(ImageFormat format);

// 副檔名（含點，不分大小寫）是哪種格式；不認得回 Unknown。
ImageFormat imageFormatOfExtension(const std::wstring& ext);
// 看檔案開頭的位元組認格式（16 bytes 就夠）；不認得回 Unknown。
ImageFormat sniffImageFormat(const unsigned char* head, size_t size);
// 讀檔頭認格式，不看副檔名。讀不到或不認得回 Unknown。
ImageFormat detectImageFormat(const std::wstring& path);

// 寫不出來的格式（表裡 canSave 是 false 的）：開起來唯讀，只能「輸出成」。Unknown（例如 GIF）不算。
bool isViewOnlyFormat(ImageFormat format);

// HEIF 容器裡的影像是哪種編碼，看 ftyp box 的 brand（主要的與相容的）。解不開時用來說要裝哪個擴充。
enum class HeifCodec { Unknown, Hevc, Av1 };
// head 是檔案開頭；ftyp box 比 head 長時只看 head 裡的部分。不是 HEIF 或看不出來回 Unknown。
HeifCodec heifCodecOf(const unsigned char* head, size_t size);
// 讀檔頭（最多 256 bytes）呼叫 heifCodecOf。
HeifCodec detectHeifCodec(const std::wstring& path);

// 檔案對話框。
// 一種格式的檔名樣式，例如 L"*.jpg;*.jpeg;*.jpe;*.jfif"。
std::wstring imagePatterns(ImageFormat format);
// 全部開得了的圖片副檔名的樣式（含 plugin 的）。
std::wstring allImagePatterns();
// 存檔對話框列出的格式與順序：寫得出來的格式都在，第一個（PNG）是預設。
const std::vector<ImageFormat>& saveDialogFormats();
// 存檔對話框的篩選字串：每種格式「名稱 (*.ext)\0樣式\0」，順序同 saveDialogFormats()。
std::wstring saveDialogFilter();
// 存檔對話框的預設檔名：副檔名寫得出來就照原樣；寫不出來（HEIC、JPEG XL、GIF、沒有副檔名）就換成 fallback 格式的
// 第一個副檔名。留著原副檔名的話，對話框會先問「x.heic 已經存在，要取代嗎？」，之後才補成 x.heic.png。空字串回空字串。
std::wstring saveDialogDefaultName(const std::wstring& path, ImageFormat fallback);

// 下面幾個是上面那張表的捷徑。
// 開得了的副檔名：上面那張表，加上生效中的讀圖 plugin 接手的（core/Plugins.h）。
// 副檔名是不是開得了的圖片格式（不分大小寫，含點，例如 L".jpg"）。
bool isSupportedExtension(const std::wstring& ext);
// 全部開得了的副檔名（小寫、含點）：表裡的在前，plugin 的接在後面。
std::vector<std::wstring> supportedExtensions();
// 存檔用的副檔名是不是支援寫出的格式。
bool canSaveExtension(const std::wstring& ext);
// 這個副檔名存檔時留得住透明（PNG、TIFF、WebP）。JPEG、BMP 不行。
bool extensionKeepsTransparency(const std::wstring& ext);

} // namespace mp
