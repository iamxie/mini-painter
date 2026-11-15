#include "core/TextUtil.h"

#include <windows.h>

#include <algorithm>
#include <cwctype>
#include <stdexcept>

namespace mp {

std::wstring utf8ToWide(const std::string& s) {
    if (s.empty()) return L"";
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring out(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), n);
    return out;
}

std::string wideToUtf8(const std::wstring& s) {
    if (s.empty()) return "";
    int n = WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), n, nullptr, nullptr);
    return out;
}

std::wstring trim(const std::wstring& s) {
    size_t a = s.find_first_not_of(L" \t\r\n");
    if (a == std::wstring::npos) return L"";
    size_t b = s.find_last_not_of(L" \t\r\n");
    return s.substr(a, b - a + 1);
}

std::wstring lowerCopy(const std::wstring& s) {
    std::wstring lower = s;
    for (auto& ch : lower) ch = static_cast<wchar_t>(std::towlower(ch));
    return lower;
}

int clampedIntFromText(const std::wstring& text, int fallback, int lo, int hi) {
    try {
        return std::clamp(std::stoi(text), lo, hi);
    } catch (const std::invalid_argument&) {
        return fallback;
    } catch (const std::out_of_range&) {
        return text.find(L'-') != std::wstring::npos ? lo : hi;
    }
}

} // namespace mp
