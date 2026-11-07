#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace mp {

// 壓縮檔（zip / 7z / rar，含 cbz / cb7 / cbr）：透過 7-Zip 的 7z.dll（bit7z 包裝）整包解到暫存資料夾，
// 之後把暫存資料夾當一般資料夾用。7z.dll 放在 exe 旁，執行時才載入；沒有就不支援壓縮檔。

// 載入 7z.dll。可重複呼叫；載過就直接回 true。失敗（檔案不在、不是 7z.dll）回 false。
bool loadArchiveLibrary(const std::wstring& dllPath);
bool archiveLibraryLoaded();

// 用檔頭判斷是不是 zip / 7z / rar（不看副檔名）。讀不到檔回 false。
bool looksLikeArchive(const std::wstring& path);

// 壓縮檔裡這個項目要不要略過：macOS 的 __MACOSX/、Thumbs.db、desktop.ini、點開頭的檔案或資料夾。
// relPath 用 / 或 \ 分隔都行。
bool isArchiveEntrySkipped(const std::wstring& relPath);

enum class ArchiveStatus {
    Ok,
    NoLibrary,     // 7z.dll 沒載入
    NotArchive,    // 開不起來、不是支援的壓縮檔
    NeedPassword,  // 有加密，沒給密碼
    WrongPassword, // 給的密碼不對
    Cancelled,     // progress 回呼回 false
    NoImages,      // 裡面沒有支援的圖片
    TooLarge,      // 解壓後大小超過 maxTotalBytes
    NoSpace,       // 目標磁碟剩餘空間不夠
    Incomplete,    // 有項目解不出來（損毀、寫不進去）：files / names 只剩落地的，expected / written 給訊息用，error 是第一趟的訊息
    Failed,        // 其他錯誤（一張都解不出來），看 error
};

struct ExtractOptions {
    std::wstring password;
    // 解壓進度（done / total 都是 bytes）。回 false 取消。可以為空。
    std::function<bool(uint64_t done, uint64_t total)> progress;
    uint64_t maxTotalBytes = 8ull << 30; // 擋 zip bomb
};

struct ArchiveResult {
    ArchiveStatus status = ArchiveStatus::Failed;
    std::wstring error;              // Failed 時 7-Zip / bit7z 的訊息（英文）
    // 解出來的圖片完整路徑與對應的壓縮檔內名字（/ 分隔），兩者等長、同序，依壓縮檔內完整路徑自然排序。
    // 磁碟檔名是序號（00001.png …），不是壓縮檔內的名字：那個名字可能含 Windows 不允許的字元、可能兩個項目撞名，
    // 用序號就不會解失敗、不會掉頁。顯示給使用者看的用 names。
    std::vector<std::wstring> files;
    std::vector<std::wstring> names;
    int expected = 0;                // 挑出來要解的項目數
    int written = 0;                 // 真的落地的數
    uint64_t totalBytes = 0;         // 圖片項目解壓後的總大小
    bool solid = false;
};

// 開壓縮檔、挑出支援格式的圖片項目、整包解到 destDir（攤平，檔名用序號），回傳檔案清單。
// 中途有項目解不出來（例如某一頁 CRC 錯）時，壞的略過、其他的照解，回 Incomplete；一張都解不出來才 Failed。
// 沒有副檔名的小項目也會解出來看檔頭（檔名用錯字碼頁解時「.」會被吃掉），是圖片就留下並補副檔名。
// destDir 不存在會建立。失敗時 destDir 裡可能留下部分檔案，呼叫端自己清。
ArchiveResult extractImages(const std::wstring& archivePath, const std::wstring& destDir, const ExtractOptions& opt);

// 只驗密碼，不解到磁碟：開檔後把一個加密項目解到記憶體（solid 用第一個，否則用最小的）。
// 回 Ok（密碼對，或這個檔根本沒加密）、WrongPassword、NeedPassword（沒給密碼但有加密）、NotArchive、NoLibrary、Failed。
ArchiveStatus testPassword(const std::wstring& archivePath, const std::wstring& password);

// 列出 dir 底下（遞迴）所有支援格式的圖片，依完整路徑自然排序。
std::vector<std::wstring> listImageFilesRecursive(const std::wstring& dir);

// 看檔頭判斷圖片格式，回傳副檔名（含點、小寫：.png .jpg .webp .bmp .tif）。不是支援的圖片或讀不到檔回空字串。
std::wstring sniffImageExtension(const std::wstring& path);

} // namespace mp
