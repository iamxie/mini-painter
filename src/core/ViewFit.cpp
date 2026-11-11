#include "core/ViewFit.h"

#include <algorithm>
#include <cmath>

namespace mp {

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

} // namespace mp
