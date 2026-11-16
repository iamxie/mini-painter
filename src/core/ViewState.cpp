#include "core/ViewState.h"

#include <cmath>

namespace mp {

namespace {
constexpr float kActualSizeTol = 1e-4f; // 倍率和 1 差這麼多以內算「實際大小」
constexpr float kScrollMovedPx = 0.5f;  // 位移變了超過這麼多才算畫面有動
} // namespace

bool ViewState::atActualSize() const { return std::abs(view_.zoom - 1.0f) <= kActualSizeTol; }

void ViewState::fit(const ViewFrame& f, bool resetOffset) {
    if (!f.hasContent()) return;
    fit_ = true;
    // 裁切模式剛進入時整張圖放進畫布（不改檢視模式設定）
    const ViewMode mode = f.cropping && cropFit_ ? ViewMode::FitWindow : f.mode;
    view_.zoom = fitZoom(mode, f.canvasW, f.canvasH, f.contentW, f.contentH, f.upscaleSmall);
    if (resetOffset) view_.offset = {0, 0};
    clamp(f);
}

void ViewState::reflow(const ViewFrame& f, bool resetOffset) {
    if (fit_) fit(f, resetOffset);
    else clamp(f);
}

void ViewState::zoomTo(const ViewFrame& f, float zoom, Pt anchorCanvas) {
    if (!f.hasContent()) return;
    view_ = zoomAbout(view_, zoom, anchorCanvas);
    fit_ = false;
    cropFit_ = false;
    clamp(f);
}

bool ViewState::scrollBy(const ViewFrame& f, float dx, float dy) {
    if (!f.hasContent()) return false;
    const Pt before = view_.offset;
    view_.offset = before + Pt{dx, dy};
    clamp(f);
    return std::abs(view_.offset.x - before.x) > kScrollMovedPx || std::abs(view_.offset.y - before.y) > kScrollMovedPx;
}

void ViewState::panTo(const ViewFrame& f, Pt offset) {
    if (!f.hasContent()) return;
    view_.offset = offset;
    clamp(f);
}

void ViewState::scrollToBottom(const ViewFrame& f) {
    if (!f.hasContent()) return;
    view_.offset.y = -1e9f;
    clamp(f);
}

void ViewState::clamp(const ViewFrame& f) {
    if (!f.hasContent()) return;
    view_.offset = clampOffset(view_.offset, view_.zoom, f.contentW, f.contentH, f.canvasW, f.canvasH);
}

void ViewState::enterCrop(const ViewFrame& f) {
    savedView_ = view_;
    savedFit_ = fit_;
    cropFit_ = true;
    fit(f);
}

void ViewState::leaveCrop(const ViewFrame& f) {
    cropFit_ = false;
    view_ = savedView_;
    fit_ = savedFit_;
    reflow(f, false);
}

} // namespace mp
