#pragma once
#include <string>
#include <unordered_map>
#include <vector>

namespace mp {

// 一個語系：程式裡的英文字串 -> 翻譯。英文本身不需要表，查不到就用 key。
struct LangTable {
    std::wstring code;  // 檔頭 _code=，例如 L"zh-TW"
    std::wstring name;  // 檔頭 _name=，例如 L"繁體中文"，顯示在語言選單
    std::unordered_map<std::wstring, std::wstring> map;
    bool empty() const { return map.empty(); }
};

// 解析 lang 檔內容。格式：一行 key=value，# 或 ; 開頭是註解，
// 跳脫：\n 換行、\t tab、\\ 反斜線、\= 等號（key 裡的等號要跳脫）。
// 前後空白會修掉。壞行略過。
LangTable parseLangText(const std::wstring& text);

// 讀 UTF-8（可有 BOM）的 lang 檔。檔案不存在或讀不到回傳 false。
bool loadLangFile(const std::wstring& path, LangTable& out);

// 列出資料夾裡每個 *.txt 並解析。沒有 _code 的檔略過。順序依檔名。
std::vector<LangTable> loadLangDir(const std::wstring& dir);

// 決定要用哪個語系代碼。
// setting：使用者選的代碼，空字串表示跟隨系統。
// systemLocale：例如 L"zh-TW"、L"en-US"、L"ja-JP"。
// available：可用的代碼清單（內建的 en、zh-TW 加 lang/ 掃到的）。
// 規則：setting 有且可用 -> setting；系統代碼完全相同 -> 它；
// zh 系：TW / HK / MO 或含 Hant -> zh-TW，其他 zh 不做前綴比對；
// 其他語言比對 '-' 前面的語言部分；都沒有 -> en。
std::wstring pickLanguage(const std::wstring& setting, const std::wstring& systemLocale, const std::vector<std::wstring>& available);

// 目前使用中的語系。setActiveLang 傳空表 = 英文。
void setActiveLang(LangTable table);
const LangTable& activeLang();

// 查翻譯。查不到回傳 key 本身（key 必須是字串常值，指標要一直有效）。
// 回傳的指標在下次 setActiveLang 之前都有效。
const wchar_t* tr(const wchar_t* key);
inline std::wstring trs(const wchar_t* key) { return tr(key); }

} // namespace mp
