#pragma once
// 測試共用的小工具。暫存檔都放在這個行程自己的根目錄底下，整輪測試跑完（含失敗）由 test_util.cpp 的 listener 刪掉。
#include <string>
#include <vector>

#include "core/Image.h"

namespace mp::test {

// 測試程式所在的資料夾，含結尾的反斜線。
std::wstring exeDir();
// CMake（MP_7Z_DLL）複製到測試程式旁的 7z.dll。
std::wstring dllPath();
// 載入 7z.dll；找不到時用看得懂的訊息讓測試失敗。
void requireArchiveLibrary();
// 二進位 fixture：先找測試程式旁的 fixtures\（建置時複製過去的），沒有再用原始碼樹的路徑。
std::wstring fixturePath(const wchar_t* name);

// 這個行程的暫存根目錄：%TEMP%\mp_tests_<pid>，不含結尾反斜線。
const std::wstring& tempRoot();
// 根目錄底下的一個路徑（不建立）。
std::wstring tempPath(const wchar_t* name);
// 根目錄底下的一個空資料夾：已經有就先整個刪掉再建。
std::wstring freshDir(const wchar_t* name);

bool exists(const std::wstring& path);
// 讀不到就讓測試失敗。
std::string readBytes(const std::wstring& path);
// 上層資料夾不在就先建。
void writeBytes(const std::wstring& path, const std::string& bytes);

// 單色不透明的圖。
Image solidImage(int w, int h, uint8_t b = 255, uint8_t g = 255, uint8_t r = 255);
// 寫一張白色 PNG（上層資料夾不在就先建）。
void writePng(const std::wstring& path, int w, int h);
// 一張白色 PNG 的檔案內容。
std::string pngBytes(int w, int h);

// dir 底下（遞迴）所有支援格式的圖片，完整路徑，自然排序。不存在回空的。
std::vector<std::wstring> listImagesRecursive(const std::wstring& dir);

// 這個 scope 內用空的語系（tr 回英文 key），離開時也設回空的。
// 每個 test case 開始時 listener 已經設成空的；會自己改語系的測試用這個收尾。
struct LangGuard {
    LangGuard();
    ~LangGuard();
};

// 這個 scope 內新配置的圖大於 bytes 就丟 std::bad_alloc（core/Image 的 setImageAllocLimitForTest），離開時關掉。
struct ImageAllocLimit {
    explicit ImageAllocLimit(size_t bytes) { setImageAllocLimitForTest(bytes); }
    ~ImageAllocLimit() { setImageAllocLimitForTest(0); }
    ImageAllocLimit(const ImageAllocLimit&) = delete;
    ImageAllocLimit& operator=(const ImageAllocLimit&) = delete;
};

// 每個 test case 的時間上限（秒）。超過就印出名稱並以結束碼 3 結束行程，CI 才看得出卡在哪。
// 環境變數 MP_TEST_TIMEOUT_SEC 可以改（驗證這個機制本身時用：mp_tests "[.hang]"）。
constexpr int kTestCaseTimeoutSec = 120;

} // namespace mp::test
