#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace mp {

// 壓縮檔（zip / 7z / rar，含 cbz / cb7 / cbr）：透過 7-Zip 的 7z.dll（bit7z 包裝）整包解到暫存資料夾，
// 之後把暫存資料夾當一般資料夾用。7z.dll 放在 exe 旁，執行時才載入；沒有就不支援壓縮檔。
//
// 執行緒：7z.dll 載入後整個行程共用、不卸載。下面的函式可以在任何執行緒呼叫，也可以同時呼叫
// （每次呼叫各開自己的 reader；共用的只有 DLL 的 handle 與建立物件的函式指標，bit7z 4.1 原始碼確認過）。
// 還沒載入時，其他函式回 NoLibrary：要用壓縮檔的那一方（UI）負責先呼叫 loadArchiveLibrary。

// 載入 7z.dll 的結果。
struct LibraryLoad {
    enum Status {
        Ok,
        FileMissing, // 那個路徑沒有檔案
        LoadFailed,  // 檔案在但載不進來（位元數不對、不是 7z.dll、缺相依）；systemError 是 Windows 的錯誤碼
    };
    Status status = LoadFailed;
    unsigned long systemError = 0;
    bool ok() const { return status == Ok; }
    explicit operator bool() const { return ok(); }
};
// 載入 7z.dll。可重複呼叫；載過就直接回 Ok（不看這次給的路徑）。
LibraryLoad loadArchiveLibrary(const std::wstring& dllPath);
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
    Incomplete,    // 有項目解不出來（損毀、寫不進去）：files / names 只剩落地的，expected / written 給訊息用，error 是第一趟的原因
    Failed,        // 其他錯誤（一張都解不出來），看 error
};

// 解不開的原因。status 是 NotArchive / Incomplete / Failed 時有意義。
enum class ArchiveFailure {
    None,
    InvalidArchive,    // 不是壓縮檔，或認不出格式
    DataError,         // 資料損毀
    CrcError,          // 檢查碼不符
    HeadersError,      // 檔頭損毀
    UnexpectedEnd,     // 檔案不完整（提早結束）
    UnsupportedMethod, // 用了這版 7-Zip 不支援的壓縮方法
    CannotCreateFolder, // 建不出解壓的目標資料夾
    NoLibrary,         // 7z.dll 沒載入
    System,            // Windows 回報的錯誤，systemError 是錯誤碼
    Other,             // 7-Zip / bit7z 回報的其他錯誤，detail 是它的訊息（英文，沒有翻譯）
};
struct ArchiveError {
    ArchiveFailure reason = ArchiveFailure::None;
    unsigned long systemError = 0;
    std::wstring detail;
    bool empty() const { return reason == ArchiveFailure::None; }
};
// reason 的英文說明，也是翻譯 key（core 不翻譯，app/UiText 的 archiveErrorText 組成畫面上的句子）。None 回空字串。
const wchar_t* archiveFailureMessage(ArchiveFailure reason);

// 一個壓縮檔裡的圖片解開後總共最多多大（預設值；錯誤訊息裡的上限也用這個數字）。
constexpr uint64_t kArchiveMaxTotalBytes = 8ull << 30;

struct ExtractOptions {
    std::wstring password;
    // 解壓進度（done / total 都是 bytes）。回 false 取消。可以為空。
    std::function<bool(uint64_t done, uint64_t total)> progress;
    uint64_t maxTotalBytes = kArchiveMaxTotalBytes; // 擋 zip bomb
};

struct ArchiveResult {
    ArchiveStatus status = ArchiveStatus::Failed;
    ArchiveError error;
    // 解出來的圖片完整路徑與對應的壓縮檔內名字（/ 分隔），兩者等長、同序，依壓縮檔內完整路徑自然排序。
    // 磁碟檔名是序號（00001.png …），不是壓縮檔內的名字：那個名字可能含 Windows 不允許的字元、可能兩個項目撞名，
    // 用序號就不會解失敗、不會掉頁。顯示給使用者看的用 names。
    std::vector<std::wstring> files;
    std::vector<std::wstring> names;
    int expected = 0;                // 挑出來要解的項目數
    int written = 0;                 // 真的落地的數
    uint64_t totalBytes = 0;         // 圖片項目解壓後的總大小
    bool solid = false;
    // 解成功（含 Incomplete：壞的略過、其他的可以看）。
    bool usable() const { return status == ArchiveStatus::Ok || status == ArchiveStatus::Incomplete; }
};

// 開壓縮檔、挑出支援格式的圖片項目、整包解到 destDir（攤平，檔名用序號），回傳檔案清單。
// 中途有項目解不出來（例如某一頁 CRC 錯）時，壞的略過、其他的照解，回 Incomplete；一張都解不出來才 Failed。
// 沒有副檔名的小項目也會解出來看檔頭（檔名用錯字碼頁解時「.」會被吃掉），是圖片就留下並補副檔名。
// destDir 不存在會建立。失敗時 destDir 裡可能留下部分檔案，呼叫端自己清。
ArchiveResult extractImages(const std::wstring& archivePath, const std::wstring& destDir, const ExtractOptions& opt);

// 試密碼列表時的回呼。都可以為空。
struct PasswordHooks {
    // 開始試第 index 個（從 1 起）、共 total 個。
    std::function<void(int index, int total)> onTry;
    // 使用者要取消了嗎。每試一個之前、試完最後一個之後都會問。
    std::function<bool()> cancelled;
};
// 在 passwords 裡找出開得了 archivePath 的那一個（用 testPassword 逐一試）。回傳它的位置；都不對回 -1。
// 取消時 cancelled 設成 true、回 -1：連「試最後一個的途中按了取消」也算取消，不會當成「都不對」。
int findPassword(const std::wstring& archivePath, const std::vector<std::wstring>& passwords, const PasswordHooks& hooks,
                 bool& cancelled);

struct PasswordExtract {
    ArchiveResult result;
    std::wstring password;  // 解開用的密碼（opt.password 或密碼列表裡的）；沒加密、沒解開是空的
    bool listTried = false; // 密碼列表試過一輪了（都不對時 result.status 是 NeedPassword：呼叫端不必再試一次，直接問使用者）
};
// 開一個可能加密的壓縮檔，流程只有這一份（畫面上開檔與背景的換本預備都用它）：
// 先照 opt（通常不帶密碼）解；要密碼而且有密碼列表，就清掉 destDir、逐一試，對了就用它解；都不對回 NeedPassword。
PasswordExtract extractWithPasswords(const std::wstring& archivePath, const std::wstring& destDir, const ExtractOptions& opt,
                                     const std::vector<std::wstring>& passwords, const PasswordHooks& hooks = {});

// 整個壓縮檔照原本的資料夾結構解到 destDir（不挑項目、不改名）。給自動更新解發佈包用。
// 失敗時回傳的 error 不是空的。
ArchiveError extractAll(const std::wstring& archivePath, const std::wstring& destDir);

// 只驗密碼，不解到磁碟：開檔後把一個加密項目解到記憶體（solid 用第一個，否則用最小的）。
// 回 Ok（密碼對，或這個檔根本沒加密）、WrongPassword、NeedPassword（沒給密碼但有加密）、NotArchive、NoLibrary、Failed。
ArchiveStatus testPassword(const std::wstring& archivePath, const std::wstring& password);

// 看檔頭判斷圖片格式，回傳副檔名（含點、小寫：.png .jpg .webp .bmp .tif）。不是支援的圖片或讀不到檔回空字串。
std::wstring sniffImageExtension(const std::wstring& path);

} // namespace mp
