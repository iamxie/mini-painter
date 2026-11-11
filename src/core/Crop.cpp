#include "core/Crop.h"

#include <algorithm>
#include <cmath>

namespace mp {

CropPart cropHitTest(const CropBox& box, Pt p, float tol, float corner) {
    float l = static_cast<float>(box.left), t = static_cast<float>(box.top);
    float r = static_cast<float>(box.right), b = static_cast<float>(box.bottom);
    if (p.x < l - tol || p.x > r + tol || p.y < t - tol || p.y > b + tol) return CropPart::None;
    float dl = std::abs(p.x - l), dr = std::abs(p.x - r), dt = std::abs(p.y - t), db = std::abs(p.y - b);
    float dx = std::min(dl, dr), dy = std::min(dt, db);
    bool left = dl <= dr, top = dt <= db;
    if ((dx <= tol && dy <= corner) || (dy <= tol && dx <= corner))
        return left ? (top ? CropPart::TopLeft : CropPart::BottomLeft) : (top ? CropPart::TopRight : CropPart::BottomRight);
    if (dx <= tol) return left ? CropPart::Left : CropPart::Right;
    if (dy <= tol) return top ? CropPart::Top : CropPart::Bottom;
    return CropPart::Inside;
}

CropBox dragCropBox(const CropBox& start, CropPart part, float dx, float dy, int imageW, int imageH) {
    CropBox out = start;
    auto rnd = [](float v) { return static_cast<int>(std::lround(v)); };
    if (part == CropPart::Inside) {
        int mx = std::clamp(rnd(dx), -start.left, imageW - start.right);
        int my = std::clamp(rnd(dy), -start.top, imageH - start.bottom);
        out.left += mx;
        out.right += mx;
        out.top += my;
        out.bottom += my;
        return out;
    }
    bool l = part == CropPart::Left || part == CropPart::TopLeft || part == CropPart::BottomLeft;
    bool r = part == CropPart::Right || part == CropPart::TopRight || part == CropPart::BottomRight;
    bool t = part == CropPart::Top || part == CropPart::TopLeft || part == CropPart::TopRight;
    bool b = part == CropPart::Bottom || part == CropPart::BottomLeft || part == CropPart::BottomRight;
    if (l) out.left = std::clamp(rnd(start.left + dx), 0, start.right - 1);
    if (r) out.right = std::clamp(rnd(start.right + dx), start.left + 1, imageW);
    if (t) out.top = std::clamp(rnd(start.top + dy), 0, start.bottom - 1);
    if (b) out.bottom = std::clamp(rnd(start.bottom + dy), start.top + 1, imageH);
    return out;
}

std::vector<Annotation> cropAnnotations(const std::vector<Annotation>& items, const CropBox& box) {
    std::vector<Annotation> out;
    for (const auto& a : items) {
        Rc bb = bounds(a).inflate(std::max(1.0f, a.stroke.width()));
        bool overlaps = bb.right > box.left && bb.left < box.right && bb.bottom > box.top && bb.top < box.bottom;
        if (!overlaps) continue;
        Annotation moved = a;
        moveBy(moved, static_cast<float>(-box.left), static_cast<float>(-box.top));
        out.push_back(std::move(moved));
    }
    return out;
}

} // namespace mp
