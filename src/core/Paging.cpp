#include "core/Paging.h"

#include <cwchar>

namespace mp {

bool SpreadCache::PathLess::operator()(const std::wstring& a, const std::wstring& b) const {
    return _wcsicmp(a.c_str(), b.c_str()) < 0;
}

const std::vector<Spread>& SpreadCache::get(const std::vector<std::wstring>& files, bool firstAlone, bool shiftOne,
                                            const IsWide& isWide) {
    if (valid_ && firstAlone == firstAlone_ && shiftOne == shiftOne_ && files == files_) return spreads_;
    std::vector<bool> wide;
    wide.reserve(files.size());
    std::map<std::wstring, bool, PathLess> kept;
    for (const auto& f : files) {
        auto it = wide_.find(f);
        const bool w = it != wide_.end() ? it->second : (isWide && isWide(f));
        kept.emplace(f, w);
        wide.push_back(w);
    }
    wide_ = std::move(kept);
    spreads_ = buildSpreads(wide, firstAlone, shiftOne);
    files_ = files;
    firstAlone_ = firstAlone;
    shiftOne_ = shiftOne;
    valid_ = true;
    return spreads_;
}

void SpreadCache::invalidateFile(const std::wstring& path) {
    wide_.erase(path);
    valid_ = false;
}

void SpreadCache::remember(const std::wstring& path, bool wide) {
    wide_[path] = wide;
    valid_ = false;
}

void SpreadCache::clear() {
    spreads_.clear();
    files_.clear();
    wide_.clear();
    valid_ = false;
}

int pageTurnTarget(int dir, int index, int count, const std::vector<Spread>* spreads) {
    if (dir == 0 || index < 0 || index >= count) return -1;
    const int step = dir > 0 ? 1 : -1;
    if (!spreads) {
        const int target = index + step;
        return target >= 0 && target < count ? target : -1;
    }
    const int cur = spreadIndexOf(*spreads, index);
    const int next = cur + step;
    if (cur < 0 || next < 0 || next >= static_cast<int>(spreads->size())) return -1;
    return (*spreads)[static_cast<size_t>(next)].first;
}

WantedPages wantedPages(const std::vector<std::wstring>& files, int index, int lastDir, const std::vector<Spread>* spreads) {
    WantedPages out;
    const int count = static_cast<int>(files.size());
    auto at = [&](int i) { return i >= 0 && i < count ? files[static_cast<size_t>(i)] : std::wstring(); };
    if (spreads) {
        const int cur = spreadIndexOf(*spreads, index), n = static_cast<int>(spreads->size());
        auto pages = [&](int k) {
            const Spread& sp = (*spreads)[static_cast<size_t>(k)];
            std::vector<std::wstring> v{at(sp.first)};
            if (sp.second >= 0) v.push_back(at(sp.second));
            return v;
        };
        const int next = cur + lastDir, prev = cur - lastDir;
        out.atEdge = cur >= 0 && (next < 0 || next >= n);
        if (cur >= 0 && !out.atEdge) out.others.push_back(pages(next));
        if (cur >= 0 && prev >= 0 && prev < n) out.others.push_back(pages(prev));
    } else {
        const int next = index + lastDir, prev = index - lastDir;
        out.atEdge = next < 0 || next >= count;
        if (!out.atEdge) out.others.push_back({at(next)});
        if (prev >= 0 && prev < count) out.others.push_back({at(prev)});
    }
    return out;
}

std::wstring composeTitle(const TitleInfo& t) {
    std::wstring s;
    if (!t.name.empty()) {
        if (!t.secondName.empty()) {
            // 依畫面上左右順序列出兩個檔名，頁碼寫成 1-2。
            s = t.rightToLeft ? t.secondName + L" | " + t.name : t.name + L" | " + t.secondName;
            if (t.dirty) s += L"*";
            if (t.spreadFirst >= 0 && t.count > 1)
                s += L"  (" + std::to_wstring(t.spreadFirst + 1) + L"-" + std::to_wstring(t.spreadSecond + 1) + L"/" +
                     std::to_wstring(t.count) + L")";
        } else {
            s = t.name;
            if (t.dirty) s += L"*";
            if (t.count > 1 && t.index >= 0) s += L"  (" + std::to_wstring(t.index + 1) + L"/" + std::to_wstring(t.count) + L")";
        }
        s += L" - ";
    }
    if (!t.profile.empty()) s += t.profile + (t.profileModified ? L"* - " : L" - ");
    s += t.appName;
    return s;
}

bool isInsideDir(const std::wstring& dir, const std::wstring& path) {
    if (dir.empty() || path.size() <= dir.size()) return false;
    if (_wcsnicmp(path.c_str(), dir.c_str(), dir.size()) != 0) return false;
    const wchar_t sep = path[dir.size()];
    return sep == L'\\' || sep == L'/';
}

std::wstring archiveEntryName(const std::wstring& dir, const std::vector<std::wstring>& files,
                              const std::vector<std::wstring>& names, const std::wstring& path) {
    if (!isInsideDir(dir, path)) return L"";
    for (size_t i = 0; i < files.size() && i < names.size(); ++i)
        if (_wcsicmp(files[i].c_str(), path.c_str()) == 0) return names[i];
    return fileNameOf(path);
}

} // namespace mp
