#include "app/UiText.h"

#include <windows.h>

#include <cwchar>

#include "core/Lang.h"

namespace mp {

std::wstring fmt1(const wchar_t* key, const std::wstring& a) {
    const wchar_t* f = tr(key);
    std::wstring out(wcslen(f) + a.size() + 8, L'\0');
    int n = swprintf(out.data(), out.size(), f, a.c_str());
    out.resize(n > 0 ? static_cast<size_t>(n) : 0);
    return out;
}

std::wstring fmt2(const wchar_t* key, const std::wstring& a, const std::wstring& b) {
    const wchar_t* f = tr(key);
    std::wstring out(wcslen(f) + a.size() + b.size() + 8, L'\0');
    int n = swprintf(out.data(), out.size(), f, a.c_str(), b.c_str());
    out.resize(n > 0 ? static_cast<size_t>(n) : 0);
    return out;
}

std::wstring systemErrorText(unsigned long code) {
    wchar_t buf[512];
    DWORD n = FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, code, 0, buf, 512, nullptr);
    std::wstring s(buf, n);
    while (!s.empty() && (s.back() == L'\r' || s.back() == L'\n' || s.back() == L' ' || s.back() == L'.' || s.back() == L'。'))
        s.pop_back();
    if (s.empty()) s = L"error " + std::to_wstring(code);
    return s;
}

std::wstring ioErrorText(const IoError& e) {
    if (e.ok()) return L"";
    const wchar_t* key = ioErrorMessage(e.code);
    std::wstring text = e.code == IoErrorCode::UnsupportedSaveFormat ? fmt1(key, e.detail) : trs(key);
    if (e.hresult != 0) {
        const wchar_t* f = tr(L"%s (HRESULT 0x%08X)");
        std::wstring out(wcslen(f) + text.size() + 16, L'\0');
        int n = swprintf(out.data(), out.size(), f, text.c_str(), static_cast<unsigned>(e.hresult));
        out.resize(n > 0 ? static_cast<size_t>(n) : 0);
        text = out;
    }
    if (e.systemError != 0) text += L" (" + systemErrorText(e.systemError) + L")";
    return text;
}

std::wstring metaLabelText(const MetaItem& item) { return item.labelIsKey ? trs(item.label.c_str()) : item.label; }

std::wstring metaValueText(const MetaValue& value) {
    wchar_t buf[64];
    switch (value.kind) {
    case MetaValue::Kind::Text: return value.text;
    case MetaValue::Kind::ByteCount:
        swprintf(buf, 64, tr(L"(%lu bytes)"), static_cast<unsigned long>(value.number));
        return buf;
    case MetaValue::Kind::Bool: return value.number ? tr(L"Yes") : tr(L"No");
    case MetaValue::Kind::UnknownType:
        swprintf(buf, 64, tr(L"(type %u)"), static_cast<unsigned>(value.number));
        return buf;
    }
    return L"";
}

std::wstring fileSizeText(unsigned long long bytes) { return formatFileSize(bytes, tr(L"%llu bytes")); }

} // namespace mp
