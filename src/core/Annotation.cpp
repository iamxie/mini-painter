#include "core/Annotation.h"

#include "core/Lang.h"

#include <algorithm>
#include <cmath>

namespace mp {

float strokeWidthForLevel(int level) {
    static const float widths[kStrokeLevels] = {1, 2, 3, 5, 8, 12, 18};
    return widths[std::clamp(level, 0, kStrokeLevels - 1)];
}

// ---- 建立 ----

Annotation makeShape(Kind kind, Pt c, float size, const StrokeStyle& style) {
    Annotation a;
    a.kind = kind;
    a.stroke = style;
    float half = std::max(8.0f, size * 0.15f);
    switch (kind) {
    case Kind::Line:
        a.p0 = {c.x - half, c.y};
        a.p1 = {c.x + half, c.y};
        a.mid = c;
        break;
    case Kind::Magnifier:
        a.center = c;
        a.radius = half;
        a.zoom = 2.0f;
        break;
    case Kind::Mosaic:
        a.rect = Rc::fromCenter(c, half, half);
        a.blockSize = 16;
        break;
    case Kind::Bubble:
        a.rect = Rc::fromCenter(c, half * 1.3f, half);
        a.tailAngle = 0.75f * kPi;
        a.tailTip = {c.x - half * 1.3f, c.y + half * 1.8f};
        break;
    case Kind::Star:
        a.rect = Rc::fromCenter(c, half, half);
        a.pointCount = 5;
        a.innerRatio = 0.5f;
        break;
    case Kind::Polygon:
        a.rect = Rc::fromCenter(c, half, half);
        a.pointCount = 6;
        a.innerRatio = 1.0f;
        break;
    default:
        a.rect = Rc::fromCenter(c, half, half);
        break;
    }
    return a;
}

Annotation makeText(Pt c, float size, const StrokeStyle& style, const TextStyle& textStyle) {
    Annotation a;
    a.kind = Kind::Text;
    a.stroke = style;
    a.stroke.stroke = Color::noColor(); // 預設沒有外框
    a.text = textStyle;
    float halfW = std::max(40.0f, size * 0.2f);
    a.rect = {c.x - halfW, c.y - textStyle.size, c.x + halfW, c.y + textStyle.size};
    a.textValue = tr(L"Text");
    return a;
}

Annotation makeFreehand(const std::vector<Pt>& path, const StrokeStyle& style) {
    Annotation a;
    a.kind = Kind::Freehand;
    a.stroke = style;
    Rc b{path[0].x, path[0].y, path[0].x, path[0].y};
    for (auto p : path) b = b.unite({p.x, p.y, p.x, p.y});
    a.origW = std::max(1.0f, b.width());
    a.origH = std::max(1.0f, b.height());
    a.rect = {b.left, b.top, b.left + a.origW, b.top + a.origH};
    a.strokePoints.reserve(path.size());
    for (auto p : path) a.strokePoints.push_back(p - b.topLeft());
    return a;
}

// ---- 共用 ----

std::vector<Handle> rectHandles(const Rc& r) {
    Pt c = r.center();
    return {
        {H_TL, {r.left, r.top}, HandleKind::Blue},   {H_T, {c.x, r.top}, HandleKind::Blue},
        {H_TR, {r.right, r.top}, HandleKind::Blue},  {H_R, {r.right, c.y}, HandleKind::Blue},
        {H_BR, {r.right, r.bottom}, HandleKind::Blue}, {H_B, {c.x, r.bottom}, HandleKind::Blue},
        {H_BL, {r.left, r.bottom}, HandleKind::Blue}, {H_L, {r.left, c.y}, HandleKind::Blue},
    };
}

Rc dragRectHandle(const Rc& r, int id, Pt p) {
    Rc n = r;
    switch (id) {
    case H_TL: n.left = p.x; n.top = p.y; break;
    case H_T: n.top = p.y; break;
    case H_TR: n.right = p.x; n.top = p.y; break;
    case H_R: n.right = p.x; break;
    case H_BR: n.right = p.x; n.bottom = p.y; break;
    case H_B: n.bottom = p.y; break;
    case H_BL: n.left = p.x; n.bottom = p.y; break;
    case H_L: n.left = p.x; break;
    default: return r;
    }
    return n.normalized();
}

std::vector<Pt> freehandPoints(const Annotation& a) {
    std::vector<Pt> out;
    out.reserve(a.strokePoints.size());
    float sx = a.rect.width() / a.origW, sy = a.rect.height() / a.origH;
    for (auto p : a.strokePoints) out.push_back({a.rect.left + p.x * sx, a.rect.top + p.y * sy});
    return out;
}

Pt lineControlPoint(const Annotation& a) {
    // 曲線在 t=0.5 的點是 (p0 + 2c + p1) / 4，反推 c。
    Pt m = lerp(a.p0, a.p1, 0.5f);
    return a.mid * 2.0f - m;
}

std::vector<Pt> starPoints(const Annotation& a) {
    std::vector<Pt> pts;
    Pt c = a.rect.center();
    float rx = a.rect.width() * 0.5f, ry = a.rect.height() * 0.5f;
    int n = std::clamp(a.pointCount, kMinPoints, kMaxPoints);
    if (a.kind == Kind::Polygon) {
        for (int i = 0; i < n; ++i) {
            float ang = -kPi / 2 + i * 2 * kPi / n;
            pts.push_back({c.x + rx * std::cos(ang), c.y + ry * std::sin(ang)});
        }
        return pts;
    }
    float inner = std::clamp(a.innerRatio, 0.05f, 1.0f);
    for (int i = 0; i < 2 * n; ++i) {
        float ang = -kPi / 2 + i * kPi / n;
        float r = (i % 2 == 0) ? 1.0f : inner;
        pts.push_back({c.x + rx * r * std::cos(ang), c.y + ry * r * std::sin(ang)});
    }
    return pts;
}

static constexpr float kTailHalfAngle = 0.35f;

static Pt ellipsePoint(const Rc& r, float ang) {
    Pt c = r.center();
    return {c.x + r.width() * 0.5f * std::cos(ang), c.y + r.height() * 0.5f * std::sin(ang)};
}

void bubbleTail(const Annotation& a, Pt& rootA, Pt& rootB, Pt& tip) {
    rootA = ellipsePoint(a.rect, a.tailAngle + kTailHalfAngle);
    rootB = ellipsePoint(a.rect, a.tailAngle - kTailHalfAngle);
    tip = a.tailTip;
}

std::vector<Pt> bubbleOutline(const Annotation& a) {
    // 從 rootA 順時針繞一圈到 rootB（跳過尾巴那一段），再到尖端。
    std::vector<Pt> pts;
    const int segs = 48;
    float start = a.tailAngle + kTailHalfAngle;
    float sweep = 2 * kPi - 2 * kTailHalfAngle;
    for (int i = 0; i <= segs; ++i) pts.push_back(ellipsePoint(a.rect, start + sweep * i / segs));
    pts.push_back(a.tailTip);
    return pts;
}

std::vector<Pt> arrowHead(Pt tip, Pt dir, float strokeWidth) {
    float len = std::hypot(dir.x, dir.y);
    if (len < 1e-3f) dir = {1, 0}; else dir = dir * (1.0f / len);
    float size = strokeWidth * 3.0f + 6.0f;
    Pt base = tip - dir * size;
    Pt n{-dir.y, dir.x};
    float halfW = size * 0.5f;
    return {tip, base + n * halfW, base - n * halfW};
}

// ---- 綠點的角度對應 ----

static float countAngle(int count) { return -kPi / 2 + (count - kMinPoints) * (2 * kPi / 18.0f); }

static int countFromAngle(float ang, int current) {
    // ang 轉成從頂點順時針的 0..2π
    float t = ang + kPi / 2;
    while (t < 0) t += 2 * kPi;
    while (t >= 2 * kPi) t -= 2 * kPi;
    int n = kMinPoints + static_cast<int>(std::lround(t / (2 * kPi / 18.0f)));
    n = std::clamp(n, kMinPoints, kMaxPoints);
    // 轉過頭繞一圈回來時不跳變。
    if (std::abs(n - current) > 9) return current;
    return n;
}

static float zoomAngle(float zoom) {
    float t = (zoom - kMinZoom) / (kMaxZoom - kMinZoom);
    return -0.75f * kPi + t * 0.9f * kPi;
}

static float zoomFromAngle(float ang) {
    float t = (ang + 0.75f * kPi) / (0.9f * kPi);
    return kMinZoom + std::clamp(t, 0.0f, 1.0f) * (kMaxZoom - kMinZoom);
}

static Pt onEllipse(const Rc& r, float ang, float scale) {
    Pt c = r.center();
    return {c.x + r.width() * 0.5f * scale * std::cos(ang), c.y + r.height() * 0.5f * scale * std::sin(ang)};
}

static float angleFromCenter(const Rc& r, Pt p) {
    Pt c = r.center();
    float rx = std::max(1.0f, r.width() * 0.5f), ry = std::max(1.0f, r.height() * 0.5f);
    return std::atan2((p.y - c.y) / ry, (p.x - c.x) / rx);
}

// ---- 幾何查詢 ----

Rc bounds(const Annotation& a) {
    switch (a.kind) {
    case Kind::Line: {
        Rc b = Rc::fromPoints(a.p0, a.p1);
        return b.unite({a.mid.x, a.mid.y, a.mid.x, a.mid.y});
    }
    case Kind::Magnifier: return Rc::fromCenter(a.center, a.radius, a.radius);
    case Kind::Bubble: return a.rect.unite({a.tailTip.x, a.tailTip.y, a.tailTip.x, a.tailTip.y});
    default: return a.rect;
    }
}

std::vector<Handle> handles(const Annotation& a) {
    switch (a.kind) {
    case Kind::Freehand: {
        auto all = rectHandles(a.rect);
        return {all[H_TL], all[H_TR], all[H_BR], all[H_BL]};
    }
    case Kind::Line:
        return {{0, a.p0, HandleKind::Blue}, {1, a.p1, HandleKind::Blue}, {2, a.mid, HandleKind::Green}};
    case Kind::Rect: case Kind::RoundRect: case Kind::Ellipse:
        return rectHandles(a.rect);
    case Kind::Bubble: {
        auto h = rectHandles(a.rect);
        h.push_back({8, ellipsePoint(a.rect, a.tailAngle), HandleKind::Green});
        h.push_back({9, a.tailTip, HandleKind::Green});
        return h;
    }
    case Kind::Star: {
        auto h = rectHandles(a.rect);
        float ang = countAngle(a.pointCount);
        h.push_back({8, onEllipse(a.rect, ang, 1.0f), HandleKind::Green});
        h.push_back({9, onEllipse(a.rect, ang, std::clamp(a.innerRatio, 0.05f, 1.0f)), HandleKind::Green});
        return h;
    }
    case Kind::Polygon: {
        auto h = rectHandles(a.rect);
        h.push_back({8, onEllipse(a.rect, countAngle(a.pointCount), 0.5f), HandleKind::Green});
        return h;
    }
    case Kind::Magnifier: {
        Rc r = Rc::fromCenter(a.center, a.radius, a.radius);
        return {{0, onEllipse(r, kPi / 4, 1.0f), HandleKind::Blue},
                {1, onEllipse(r, zoomAngle(a.zoom), 1.0f), HandleKind::Green}};
    }
    case Kind::Mosaic: {
        float t = static_cast<float>(a.blockSize - kMinBlock) / (kMaxBlock - kMinBlock);
        return {{0, {a.rect.right, a.rect.bottom}, HandleKind::Blue},
                {1, {a.rect.left + a.rect.width() * t, a.rect.top}, HandleKind::Green}};
    }
    case Kind::Text: {
        auto all = rectHandles(a.rect);
        return {all[H_L], all[H_R]};
    }
    }
    return {};
}

HandleGuide handleGuide(const Annotation& a, int id) {
    HandleGuide g;
    auto circle = [&](const Rc& r, float scale) {
        g.kind = GuideKind::Circle;
        Pt c = r.center();
        g.ellipse = Rc::fromCenter(c, r.width() * 0.5f * scale, r.height() * 0.5f * scale);
    };
    switch (a.kind) {
    case Kind::Bubble:
        if (id == 8) circle(a.rect, 1.0f);
        break;
    case Kind::Polygon:
        if (id == 8) circle(a.rect, 0.5f);
        break;
    case Kind::Star:
        if (id == 8) circle(a.rect, 1.0f);
        if (id == 9) {
            g.kind = GuideKind::Line;
            g.a = a.rect.center();
            g.b = onEllipse(a.rect, countAngle(a.pointCount), 1.0f);
        }
        break;
    case Kind::Magnifier:
        if (id == 1) circle(Rc::fromCenter(a.center, a.radius, a.radius), 1.0f);
        break;
    case Kind::Mosaic:
        if (id == 1) {
            g.kind = GuideKind::Line;
            g.a = {a.rect.left, a.rect.top};
            g.b = {a.rect.right, a.rect.top};
        }
        break;
    default: break;
    }
    return g;
}

static bool nearPolyline(const std::vector<Pt>& pts, Pt p, float tol, bool closed) {
    if (pts.empty()) return false;
    if (pts.size() == 1) return dist(pts[0], p) <= tol;
    for (size_t i = 0; i + 1 < pts.size(); ++i)
        if (distToSegment(p, pts[i], pts[i + 1]) <= tol) return true;
    if (closed && distToSegment(p, pts.back(), pts.front()) <= tol) return true;
    return false;
}

static bool insidePolygon(const std::vector<Pt>& pts, Pt p) {
    bool in = false;
    for (size_t i = 0, j = pts.size() - 1; i < pts.size(); j = i++) {
        if ((pts[i].y > p.y) != (pts[j].y > p.y) &&
            p.x < (pts[j].x - pts[i].x) * (p.y - pts[i].y) / (pts[j].y - pts[i].y) + pts[i].x)
            in = !in;
    }
    return in;
}

bool hitTest(const Annotation& a, Pt p, float tol) {
    float half = a.stroke.width() * 0.5f + tol;
    switch (a.kind) {
    case Kind::Freehand:
        return nearPolyline(freehandPoints(a), p, half, false);
    case Kind::Line: {
        std::vector<Pt> pts;
        Pt c = lineControlPoint(a);
        for (int i = 0; i <= 32; ++i) pts.push_back(quadBezier(a.p0, c, a.p1, i / 32.0f));
        return nearPolyline(pts, p, half, false);
    }
    case Kind::Rect: case Kind::RoundRect: case Kind::Mosaic:
        return a.rect.inflate(half).contains(p);
    case Kind::Ellipse: {
        Pt c = a.rect.center();
        float rx = a.rect.width() * 0.5f + half, ry = a.rect.height() * 0.5f + half;
        if (rx <= 0 || ry <= 0) return false;
        float dx = (p.x - c.x) / rx, dy = (p.y - c.y) / ry;
        return dx * dx + dy * dy <= 1.0f;
    }
    case Kind::Bubble: {
        auto pts = bubbleOutline(a);
        return insidePolygon(pts, p) || nearPolyline(pts, p, half, true);
    }
    case Kind::Star: case Kind::Polygon: {
        auto pts = starPoints(a);
        return insidePolygon(pts, p) || nearPolyline(pts, p, half, true);
    }
    case Kind::Magnifier:
        return dist(a.center, p) <= a.radius + half;
    case Kind::Text:
        return a.rect.inflate(tol).contains(p);
    }
    return false;
}

void moveBy(Annotation& a, float dx, float dy) {
    a.rect = a.rect.offset(dx, dy);
    a.p0 = a.p0 + Pt{dx, dy};
    a.p1 = a.p1 + Pt{dx, dy};
    a.mid = a.mid + Pt{dx, dy};
    a.tailTip = a.tailTip + Pt{dx, dy};
    a.center = a.center + Pt{dx, dy};
}

bool dragHandle(Annotation& a, int id, Pt p) {
    switch (a.kind) {
    case Kind::Freehand:
        if (id != H_TL && id != H_TR && id != H_BR && id != H_BL) return false;
        a.rect = dragRectHandle(a.rect, id, p);
        if (a.rect.width() < 1) a.rect.right = a.rect.left + 1;
        if (a.rect.height() < 1) a.rect.bottom = a.rect.top + 1;
        return true;
    case Kind::Line: {
        if (id == 2) { a.mid = p; return true; }
        if (id != 0 && id != 1) return false;
        // 端點移動時，中點保持相對於兩端中點的偏移。
        Pt oldM = lerp(a.p0, a.p1, 0.5f);
        Pt offset = a.mid - oldM;
        if (id == 0) a.p0 = p; else a.p1 = p;
        a.mid = lerp(a.p0, a.p1, 0.5f) + offset;
        return true;
    }
    case Kind::Rect: case Kind::RoundRect: case Kind::Ellipse:
        if (id < 0 || id > 7) return false;
        a.rect = dragRectHandle(a.rect, id, p);
        return true;
    case Kind::Bubble:
        if (id >= 0 && id <= 7) { a.rect = dragRectHandle(a.rect, id, p); return true; }
        if (id == 8) { a.tailAngle = angleFromCenter(a.rect, p); return true; }
        if (id == 9) { a.tailTip = p; return true; }
        return false;
    case Kind::Star:
        if (id >= 0 && id <= 7) { a.rect = dragRectHandle(a.rect, id, p); return true; }
        if (id == 8) { a.pointCount = countFromAngle(angleFromCenter(a.rect, p), a.pointCount); return true; }
        if (id == 9) {
            // 沿「中心到外側綠點」的方向投影，距離 / 外半徑 = 內半徑比例。
            Pt c = a.rect.center();
            float ang = countAngle(a.pointCount);
            Pt dir{std::cos(ang), std::sin(ang)};
            float rx = std::max(1.0f, a.rect.width() * 0.5f), ry = std::max(1.0f, a.rect.height() * 0.5f);
            // 先把點換到單位圓座標。
            float ux = (p.x - c.x) / rx, uy = (p.y - c.y) / ry;
            float proj = ux * dir.x + uy * dir.y;
            a.innerRatio = std::clamp(proj, 0.05f, 1.0f);
            return true;
        }
        return false;
    case Kind::Polygon:
        if (id >= 0 && id <= 7) { a.rect = dragRectHandle(a.rect, id, p); return true; }
        if (id == 8) { a.pointCount = countFromAngle(angleFromCenter(a.rect, p), a.pointCount); return true; }
        return false;
    case Kind::Magnifier:
        if (id == 0) { a.radius = std::max(8.0f, dist(a.center, p)); return true; }
        if (id == 1) { a.zoom = zoomFromAngle(std::atan2(p.y - a.center.y, p.x - a.center.x)); return true; }
        return false;
    case Kind::Mosaic:
        if (id == 0) {
            a.rect.right = std::max(a.rect.left + 8, p.x);
            a.rect.bottom = std::max(a.rect.top + 8, p.y);
            return true;
        }
        if (id == 1) {
            float t = std::clamp((p.x - a.rect.left) / std::max(1.0f, a.rect.width()), 0.0f, 1.0f);
            a.blockSize = kMinBlock + static_cast<int>(std::lround(t * (kMaxBlock - kMinBlock)));
            return true;
        }
        return false;
    case Kind::Text:
        if (id == H_L) { a.rect.left = std::min(p.x, a.rect.right - 20); return true; }
        if (id == H_R) { a.rect.right = std::max(p.x, a.rect.left + 20); return true; }
        return false;
    }
    return false;
}

void rotateCCW90(Annotation& a, int imageWidth) {
    // (x, y) -> (y, W - x)
    auto rot = [imageWidth](Pt p) { return Pt{p.y, imageWidth - p.x}; };
    auto rotRect = [&](const Rc& r) { return Rc::fromPoints(rot({r.left, r.top}), rot({r.right, r.bottom})); };
    switch (a.kind) {
    case Kind::Freehand: {
        // 筆跡本身也要轉：相對座標 (x, y) -> (y, origW - x)，外框跟著換邊。
        for (auto& p : a.strokePoints) p = {p.y, a.origW - p.x};
        std::swap(a.origW, a.origH);
        a.rect = rotRect(a.rect);
        break;
    }
    case Kind::Line:
        a.p0 = rot(a.p0); a.p1 = rot(a.p1); a.mid = rot(a.mid);
        break;
    case Kind::Bubble:
        a.rect = rotRect(a.rect);
        a.tailTip = rot(a.tailTip);
        a.tailAngle -= kPi / 2;
        break;
    case Kind::Magnifier:
        a.center = rot(a.center);
        break;
    case Kind::Text:
        a.rect = rotRect(a.rect);
        break;
    default:
        a.rect = rotRect(a.rect);
        break;
    }
}

void scaleBy(Annotation& a, float sx, float sy) {
    auto sc = [sx, sy](Pt p) { return Pt{p.x * sx, p.y * sy}; };
    a.rect = {a.rect.left * sx, a.rect.top * sy, a.rect.right * sx, a.rect.bottom * sy};
    a.p0 = sc(a.p0); a.p1 = sc(a.p1); a.mid = sc(a.mid);
    a.tailTip = sc(a.tailTip);
    a.center = sc(a.center);
    a.radius *= std::min(sx, sy);
}

} // namespace mp
