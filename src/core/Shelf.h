#pragma once
#include <string>
#include <vector>

namespace mp {

// 書架：看漫畫時「翻到底接著看下一本」的規則。
// 一本書 = 直接底下有圖片的資料夾（子資料夾、裡面的壓縮檔都不算它的一部分），或一個壓縮檔（整包攤平算一本）。
// 書架 = 這本書所在那一層裡所有的書，依檔案總管的自然排序。只找同一層，不往上爬、不往下鑽。

// dir 直接底下（不遞迴）有沒有支援格式的圖片。
bool hasImageFiles(const std::wstring& dir);

// parentDir 底下所有的書（完整路徑）：直接有圖片的子資料夾，與壓縮檔（副檔名是壓縮檔，或不是圖片副檔名但檔頭是壓縮檔）。
std::vector<std::wstring> listBooks(const std::wstring& parentDir);

// 同一層的下一本（dir = +1）或上一本（dir = -1）。bookPath：資料夾書給資料夾路徑，壓縮檔書給壓縮檔路徑。
// 沒有（已經是最後／第一本、bookPath 不在書架上、沒有上一層）回空字串。每次呼叫都重新掃磁碟。
std::wstring neighborBook(const std::wstring& bookPath, int dir);

} // namespace mp
