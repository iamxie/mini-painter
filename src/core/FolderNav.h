#pragma once
#include <string>
#include <vector>

namespace mp {

// 副檔名是不是支援的圖片格式（不分大小寫，含點，例如 L".jpg"）。
bool isSupportedExtension(const std::wstring& ext);
// 全部支援的副檔名（小寫、含點）。
const std::vector<std::wstring>& supportedExtensions();
// 壓縮檔副檔名（小寫、含點）：.zip .7z .rar 與同格式改名的 .cbz .cb7 .cbr。
bool isArchiveExtension(const std::wstring& ext);
const std::vector<std::wstring>& archiveExtensions();
// 只有漫畫專用的三個（.cbz .cb7 .cbr）：登錄「預設程式」只用這些，不搶 zip / 7z / rar 的關聯。
const std::vector<std::wstring>& comicExtensions();
// 路徑是不是存在的資料夾。
bool isDirectoryPath(const std::wstring& path);
// 依檔案總管的排序（StrCmpLogicalW）就地排序。
void sortLogical(std::vector<std::wstring>& paths);
// 把檔名（不含路徑，不分大小寫）在 pinnedNames 裡的檔案搬到最前面：先依 pinnedNames 的順序，同名的維持原本順序；
// 其餘維持原本順序。
void movePinnedFirst(std::vector<std::wstring>& paths, const std::vector<std::wstring>& pinnedNames);
// 同上，但拿 names 的檔名來比，paths 跟著一起搬（兩者等長、一一對應；長度不同就不動）。壓縮檔用：磁碟檔名是序號，名字在 names。
void movePinnedFirst(std::vector<std::wstring>& paths, std::vector<std::wstring>& names,
                     const std::vector<std::wstring>& pinnedNames);
// 把檔名裡 Windows 不允許的字元（\ / : * ? " < > | 與控制字元）換成 _。
std::wstring sanitizeFileName(const std::wstring& name);

// 取路徑的副檔名（含點，小寫）。沒有就回傳空字串。
std::wstring extensionOf(const std::wstring& path);

// 列出資料夾裡支援的圖片檔完整路徑，依檔案總管的排序（StrCmpLogicalW）。
std::vector<std::wstring> listImageFiles(const std::wstring& dir);

// 同資料夾翻頁。
class FolderNav {
public:
    // 以某個檔案為起點建立清單。檔案不存在於清單時 index 為 -1。
    void load(const std::wstring& filePath);
    void setFiles(std::vector<std::wstring> files, const std::wstring& current);
    // load 列出資料夾後套 movePinnedFirst 用的清單（setFiles 不套，呼叫端自己排）。
    void setPinnedNames(std::vector<std::wstring> names) { pinned_ = std::move(names); }

    bool hasPrev() const { return index_ > 0; }
    bool hasNext() const { return index_ >= 0 && index_ + 1 < static_cast<int>(files_.size()); }
    std::wstring prev();
    std::wstring next();
    std::wstring current() const;
    int index() const { return index_; }
    int count() const { return static_cast<int>(files_.size()); }
    const std::vector<std::wstring>& files() const { return files_; }
    // 第 i 個檔案的路徑；超出範圍回傳空字串。
    std::wstring at(int i) const { return i >= 0 && i < count() ? files_[static_cast<size_t>(i)] : L""; }

private:
    std::vector<std::wstring> files_;
    std::vector<std::wstring> pinned_;
    int index_ = -1;
};

} // namespace mp
