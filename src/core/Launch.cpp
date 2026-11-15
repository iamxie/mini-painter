#include "core/Launch.h"

#include <cwchar>

namespace mp {

namespace {
constexpr wchar_t kAfterUpdate[] = L"--after-update";
}

LaunchArgs parseLaunchArgs(const std::vector<std::wstring>& argv) {
    LaunchArgs a;
    if (argv.size() >= 3 && argv[1] == kAfterUpdate) {
        a.afterUpdate = true;
        a.waitPid = wcstoul(argv[2].c_str(), nullptr, 10);
        if (argv.size() >= 4) a.path = argv[3];
    } else if (argv.size() >= 2) {
        a.path = argv[1];
    }
    return a;
}

std::wstring quoteArg(const std::wstring& arg) {
    if (!arg.empty() && arg.find_first_of(L" \t\"") == std::wstring::npos) return arg;
    std::wstring out = L"\"";
    size_t backslashes = 0;
    for (wchar_t c : arg) {
        if (c == L'\\') {
            ++backslashes;
            continue;
        }
        if (c == L'"') out.append(backslashes * 2 + 1, L'\\'); // 引號前的反斜線加倍，再加一個跳脫引號
        else out.append(backslashes, L'\\');
        backslashes = 0;
        out += c;
    }
    out.append(backslashes * 2, L'\\'); // 結尾的反斜線後面接的是收尾的引號
    out += L'"';
    return out;
}

std::wstring openArgs(const std::wstring& path) { return path.empty() ? L"" : quoteArg(path); }

std::wstring afterUpdateArgs(unsigned long pid, const std::wstring& path) {
    std::wstring args = std::wstring(kAfterUpdate) + L" " + std::to_wstring(pid);
    if (!path.empty()) args += L" " + quoteArg(path);
    return args;
}

} // namespace mp
