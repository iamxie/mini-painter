#pragma once
// 用 bit7z 現場產壓縮檔當 fixture。整個測試程式共用一個 Bit7zLibrary。
#include <string>
#include <vector>

#include <bit7z/bit7zlibrary.hpp>
#include <bit7z/bitformat.hpp>

namespace mp::test {

struct ArchiveEntry {
    std::string path;    // 壓縮檔內路徑（UTF-8，/ 分隔）
    std::string content; // 內容
};

struct MakeArchiveOptions {
    std::string password;
    bool cryptHeaders = false;
    bool solid = false;
};

bit7z::Bit7zLibrary& sevenZip();

void makeArchive(const std::wstring& out, const bit7z::BitInOutFormat& fmt, const std::vector<ArchiveEntry>& entries,
                 const MakeArchiveOptions& mo = {});

} // namespace mp::test
