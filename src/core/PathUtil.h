#pragma once
#include <cstdint>
#include <functional>
#include <string>

namespace mp {

// a\b：a 是空的回 b；a 結尾已經有 \ 或 / 就不再加。
std::wstring joinPath(const std::wstring& a, const std::wstring& b);

// 取路徑的副檔名（含點，小寫）。沒有就回傳空字串。
std::wstring extensionOf(const std::wstring& path);

// 路徑的檔名部分（最後一個 \ 或 / 之後）。沒有分隔符號就是整個字串。
std::wstring fileNameOf(const std::wstring& path);

// 路徑是不是存在的資料夾。
bool isDirectoryPath(const std::wstring& path);

// 資料夾裡的一個項目。
struct DirEntry {
    std::wstring name;         // 檔名，不含路徑
    bool directory = false;
    bool reparsePoint = false; // 連結點、符號連結：走訪整棵樹時不要跟進去
    bool readOnly = false;
    uint64_t size = 0;         // 檔案大小；資料夾是 0
};

// 逐一給 dir 底下檔名符合 pattern 的項目（不含 . 與 ..）。fn 回 false 就停。順序是檔案系統給的，沒有排序。
// dir 不存在或列不了：一次都不呼叫。
void forEachDirEntry(const std::wstring& dir, const std::function<bool(const DirEntry&)>& fn,
                     const wchar_t* pattern = L"*");

} // namespace mp
