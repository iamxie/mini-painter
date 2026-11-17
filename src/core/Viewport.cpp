#include "core/Viewport.h"

#include <algorithm>
#include <cmath>

namespace mp {

Pt canvasToImage(const Viewport& v, Pt canvas) { return {(canvas.x - v.offset.x) / v.zoom, (canvas.y - v.offset.y) / v.zoom}; }

Pt imageToCanvas(const Viewport& v, Pt image) { return {v.offset.x + image.x * v.zoom, v.offset.y + image.y * v.zoom}; }

Pt clampOffset(Pt offset, float zoom, float viewW, float viewH, float canvasW, float canvasH) {
    const float iw = viewW * zoom, ih = viewH * zoom;
    if (iw <= canvasW) offset.x = (canvasW - iw) / 2;
    else offset.x = std::clamp(offset.x, canvasW - iw, 0.0f);
    if (ih <= canvasH) offset.y = (canvasH - ih) / 2;
    else offset.y = std::clamp(offset.y, canvasH - ih, 0.0f);
    offset.x = std::round(offset.x);
    offset.y = std::round(offset.y);
    return offset;
}

Viewport zoomAbout(const Viewport& v, float newZoom, Pt anchorCanvas) {
    const Pt image = canvasToImage(v, anchorCanvas);
    Viewport out;
    out.zoom = std::clamp(newZoom, kViewMinZoom, kViewMaxZoom);
    out.offset = {anchorCanvas.x - image.x * out.zoom, anchorCanvas.y - image.y * out.zoom};
    return out;
}

Pt spreadSize(PageSize first, const PageSize* second) {
    if (!second) return {static_cast<float>(first.width), static_cast<float>(first.height)};
    return {static_cast<float>(first.width + second->width), static_cast<float>(std::max(first.height, second->height))};
}

Rc pageRectInSpread(PageSize first, const PageSize* second, bool wantSecond, bool rightToLeft) {
    const float w1 = static_cast<float>(first.width), h1 = static_cast<float>(first.height);
    if (!second) return wantSecond ? Rc{} : Rc{0, 0, w1, h1};
    const float w2 = static_cast<float>(second->width), h2 = static_cast<float>(second->height);
    const float h = std::max(h1, h2);
    const bool firstOnLeft = !rightToLeft;
    const float x1 = firstOnLeft ? 0 : w2, x2 = firstOnLeft ? w1 : 0;
    if (!wantSecond) return {x1, h - h1, x1 + w1, h};
    return {x2, h - h2, x2 + w2, h};
}

bool spreadWithPlaceholder(const PageSize* first, const PageSize* second, PageSize& outFirst, PageSize& outSecond) {
    if (!first && !second) return false;
    outFirst = first ? *first : *second;
    outSecond = second ? *second : *first;
    return true;
}

} // namespace mp
