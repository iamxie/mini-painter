#pragma once
#include <string>

namespace mp {

// "1.2.3" 或 "v1.2.3" → 三段數字。格式不對回 false。
struct Version {
    int major = 0, minor = 0, patch = 0;
};
bool parseVersion(const std::wstring& s, Version& out);
// a < b 回負數、相等 0、a > b 正數。
int compareVersions(const Version& a, const Version& b);
// latest 比 current 新。任一個解析不了回 false。
bool isNewerVersion(const std::wstring& latest, const std::wstring& current);

} // namespace mp
