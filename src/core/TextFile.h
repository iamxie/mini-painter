#pragma once
#include <string>

namespace mp {

enum class FileRead {
    Ok,
    Missing,    // 檔案（或它的資料夾）不存在
    Unreadable, // 檔案在，但開不了或讀到一半失敗（被別的程式獨佔、沒權限…）
};

// 整個檔讀進來，不解讀內容。別的程式同時在寫或取代這個檔也開得起來（分享讀、寫、刪）。
FileRead readFileBytes(const std::wstring& path, std::string& out);

// 讀 UTF-8 文字檔（可以有 BOM）成寬字元。
FileRead readUtf8File(const std::wstring& path, std::wstring& out);

} // namespace mp
