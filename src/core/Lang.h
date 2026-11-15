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
    std::vector<std::wstring> rejected; // 佔位符和 key 對不上、被丟掉的 key（見 parseLangText）
    bool empty() const { return map.empty(); }
};

// 把 printf 格式字串裡會吃引數的轉換依序列出來，逗號分隔，例如 L"%s: %d%%" -> L"s,d"；寬度或精度寫 * 的也算一個。
// 格式不合法（% 後面不是轉換、字串結尾是單獨的 %、用了 %n）回 false。公開是為了測試。
bool formatSpecs(const std::wstring& format, std::wstring& specs);

// 解析 lang 檔內容。格式：一行 key=value，# 或 ; 開頭是註解，
// 跳脫：\n 換行、\t tab、\\ 反斜線、\= 等號（key 裡的等號要跳脫）。
// 前後空白會修掉。壞行略過。
// 佔位符：程式會把翻譯當成 printf 的格式字串用，所以 key 是合法的格式字串時，翻譯裡會吃引數的轉換
// （%s、%d…）必須和 key 一模一樣、順序也一樣；對不上的那一行丟掉（顯示英文），key 記在 rejected。
// 翻譯檔是使用者可以放的外部檔，%s 寫成 %d 或多寫一個會讓程式讀到不存在的引數。
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
// 這是沒有上鎖的全域變數：setActiveLang 只能在啟動時、還沒有別的執行緒之前呼叫（換語言靠重新啟動）。
// core 自己不呼叫 tr()，背景執行緒跑的 core 函式（解碼、解壓）不會讀到它。
void setActiveLang(LangTable table);
const LangTable& activeLang();

// 查翻譯。查不到回傳 key 本身（key 必須是字串常值，指標要一直有效）。
// 回傳的指標在下次 setActiveLang 之前都有效。
const wchar_t* tr(const wchar_t* key);
inline std::wstring trs(const wchar_t* key) { return tr(key); }

// 標記「這個字串常值是翻譯 key」，本身不查表。core 回傳英文 key、由 app 在顯示時 tr() 的地方用它，
// scripts/lang_check.py 靠這個名字找到 key。
constexpr const wchar_t* trKey(const wchar_t* key) { return key; }

} // namespace mp
