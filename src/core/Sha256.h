#pragma once
#include <string>

namespace mp {

// .sha256 檔內容（"<64 位 hex>  檔名" 或只有 hex）→ 小寫 hex。找不到回空字串。
std::string parseSha256Text(const std::string& text);
// 檔案的 SHA-256（小寫 hex）。讀不到回空字串。
std::string sha256OfFile(const std::wstring& path);

} // namespace mp
