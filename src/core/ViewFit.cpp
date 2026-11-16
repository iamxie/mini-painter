#include "core/ViewFit.h"

#include "core/Viewport.h"

#include <algorithm>
#include <cmath>

#include "core/Lang.h"

namespace mp {

namespace {
struct ViewModeInfo {
    ViewMode mode;
    const wchar_t* key;
    const wchar_t* name;
};
// 順序同 enum。
const ViewModeInfo kViewModes[] = {
    {ViewMode::FitWindow, L"fitwindow", trKey(L"Fit to Window")},
    {ViewMode::FitWidth, L"fitwidth", trKey(L"Fit to Width")},
    {ViewMode::FitHeight, L"fitheight", trKey(L"Fit to Height")},
    {ViewMode::Actual, L"actual", trKey(L"Actual Size")},
    {ViewMode::TwoPage, L"twopage", trKey(L"Two Pages Side by Side")},
};
static_assert(sizeof(kViewModes) / sizeof(kViewModes[0]) == kViewModeCount, "every ViewMode needs a row");
} // namespace

const wchar_t* viewModeKey(ViewMode mode) {
    for (const auto& m : kViewModes)
        if (m.mode == mode) return m.key;
    return kViewModes[0].key;
}

ViewMode viewModeFromKey(const std::wstring& key) {
    for (const auto& m : kViewModes)
        if (key == m.key) return m.mode;
    return ViewMode::FitWindow;
}

const wchar_t* viewModeName(ViewMode mode) {
    for (const auto& m : kViewModes)
        if (m.mode == mode) return m.name;
    return kViewModes[0].name;
}

float fitZoom(ViewMode mode, float canvasW, float canvasH, float viewW, float viewH, bool upscaleSmall) {
    float z;
    switch (mode) {
    case ViewMode::FitWidth: z = canvasW / viewW; break;
    case ViewMode::FitHeight: z = canvasH / viewH; break;
    case ViewMode::TwoPage: z = std::min(canvasW / viewW, canvasH / viewH); break; // 兩頁並排：符合高度，兩頁都放不下時再縮
    case ViewMode::Actual: z = 1.0f; break;
    default: z = std::min(canvasW / viewW, canvasH / viewH); break;
    }
    if (!upscaleSmall || mode == ViewMode::Actual) z = std::min(1.0f, z);
    return std::max(0.01f, z);
}

bool hqTargetSize(int w, int h, float zoom, unsigned maxSide, int& tw, int& th) {
    if (std::abs(zoom - 1.0f) <= 1e-4f) return false;
    tw = std::max(1, static_cast<int>(std::lround(w * zoom)));
    th = std::max(1, static_cast<int>(std::lround(h * zoom)));
    if (static_cast<unsigned>(tw) > maxSide || static_cast<unsigned>(th) > maxSide) return false;
    return static_cast<long long>(tw) * th <= kHqMaxPixels;
}

std::vector<std::pair<int, int>> screenHqTargets(ViewMode mode, float canvasW, float canvasH, bool upscaleSmall, unsigned maxSide,
                                                 const std::vector<std::pair<int, int>>& pages) {
    std::vector<std::pair<int, int>> out(pages.size(), {0, 0});
    if (pages.empty()) return out;
    const PageSize first{pages[0].first, pages[0].second};
    const PageSize second = pages.size() > 1 ? PageSize{pages[1].first, pages[1].second} : PageSize{};
    const Pt view = spreadSize(first, pages.size() > 1 ? &second : nullptr);
    const float zoom = fitZoom(mode, canvasW, canvasH, view.x, view.y, upscaleSmall);
    for (size_t i = 0; i < pages.size(); ++i) {
        int tw = 0, th = 0;
        if (hqTargetSize(pages[i].first, pages[i].second, zoom, maxSide, tw, th)) out[i] = {tw, th};
    }
    return out;
}

} // namespace mp
