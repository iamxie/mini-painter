#pragma once
#include <string>
#include <vector>

namespace mp {

// 瀏覽資料夾的縮圖大小：小、中、大（長邊的邏輯像素，畫的時候照螢幕 DPI 放大）。
// 96 與 256 是 Windows 縮圖快取本來就有的尺寸，160 從 256 縮。使用者 2026-11-24 定。
constexpr int kThumbSizeCount = 3;
inline constexpr int kThumbSides[kThumbSizeCount] = {96, 160, 256};
constexpr int kThumbSizeDefault = 1; // 中

// 瀏覽資料夾（目錄瀏覽模式）右邊縮圖區的一格。
enum class BrowseKind { Folder, Archive, Image };
struct BrowseEntry {
    std::wstring path; // 完整路徑
    std::wstring name; // 檔名（含副檔名），畫在縮圖下面
    BrowseKind kind = BrowseKind::Image;
};

// dir 這一層（不含子資料夾裡的）：子資料夾、壓縮檔、圖片，依這個順序，各自自然排序。
// 圖片和翻頁用同一份清單（listImageFiles，看副檔名；pinned 的置頂檔名排最前面），所以縮圖順序等於 ← → 的順序。
// 子資料夾與壓縮檔只看副檔名認（不讀檔頭），隱藏或系統屬性的不列（檔案總管預設也不顯示）。
std::vector<BrowseEntry> listBrowseEntries(const std::wstring& dir, const std::vector<std::wstring>& pinned);

// entries 裡 path 的位置（不分大小寫）；沒有回 -1。
int indexOfBrowsePath(const std::vector<BrowseEntry>& entries, const std::wstring& path);

// 要瀏覽哪個資料夾：path 是資料夾就是它自己，否則是它所在的資料夾。結尾不帶斜線（磁碟根目錄是 "D:\"）。
std::wstring browseFolderOf(const std::wstring& path);
// 上一層資料夾。已經是磁碟根目錄（"D:\"）或網路分享的根（"\\server\share"）回空字串。
std::wstring parentFolder(const std::wstring& dir);
// 從 dir 往上找第一個還在的資料夾（dir 被刪掉或改名時退回上一層用）。都不在回空字串。
std::wstring nearestExistingFolder(const std::wstring& dir);
// 標題列與提示用的資料夾名：最後一層的名字；磁碟根目錄是 "D:\"，網路分享的根是 "\\server\share"。
std::wstring folderDisplayName(const std::wstring& dir);
// 瀏覽模式的標題列：「name - label - appName」。name 是資料夾名（folderDisplayName），label 是翻譯好的「瀏覽資料夾」。
std::wstring composeBrowseTitle(const std::wstring& name, const std::wstring& label, const std::wstring& appName);

} // namespace mp
