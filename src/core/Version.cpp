#include "core/Version.h"

namespace mp {

namespace {

bool parseInt(const std::wstring& s, int& out) {
    if (s.empty() || s.size() > 6) return false;
    int v = 0;
    for (wchar_t c : s) {
        if (c < L'0' || c > L'9') return false;
        v = v * 10 + (c - L'0');
    }
    out = v;
    return true;
}

} // namespace

bool parseVersion(const std::wstring& s, Version& out) {
    std::wstring t = s;
    if (!t.empty() && (t[0] == L'v' || t[0] == L'V')) t.erase(0, 1);
    size_t a = t.find(L'.');
    if (a == std::wstring::npos) return false;
    size_t b = t.find(L'.', a + 1);
    if (b == std::wstring::npos) return false;
    Version v;
    if (!parseInt(t.substr(0, a), v.major) || !parseInt(t.substr(a + 1, b - a - 1), v.minor) ||
        !parseInt(t.substr(b + 1), v.patch))
        return false;
    out = v;
    return true;
}

int compareVersions(const Version& a, const Version& b) {
    if (a.major != b.major) return a.major < b.major ? -1 : 1;
    if (a.minor != b.minor) return a.minor < b.minor ? -1 : 1;
    if (a.patch != b.patch) return a.patch < b.patch ? -1 : 1;
    return 0;
}

bool isNewerVersion(const std::wstring& latest, const std::wstring& current) {
    Version l, c;
    if (!parseVersion(latest, l) || !parseVersion(current, c)) return false;
    return compareVersions(l, c) > 0;
}

} // namespace mp
