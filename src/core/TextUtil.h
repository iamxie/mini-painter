#pragma once
#include <string>

namespace mp {

// UTF-8 <-> UTF-16。不合法的位元組照 Windows 的規則換成 U+FFFD。
std::wstring utf8ToWide(const std::string& s);
std::string wideToUtf8(const std::wstring& s);

// 去掉前後的空白（空格、tab、CR、LF）。
std::wstring trim(const std::wstring& s);

// 轉小寫（towlower，逐字元）。
std::wstring lowerCopy(const std::wstring& s);

// 文字 → [lo, hi] 的整數（設定面板的數字欄位與 ini 共用）。開頭不是數字（含空字串）回 fallback；
// 數字後面接著別的字照 stoi 的規則只取前面的數字（L"12abc" 是 12）。
// 位數多到 int 裝不下時，正的當成 hi、負的當成 lo。
int clampedIntFromText(const std::wstring& text, int fallback, int lo, int hi);

} // namespace mp
