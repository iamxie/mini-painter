#include "core/Annotation.h"

#include <algorithm>
#include <cmath>

namespace mp {

namespace {

constexpr float kStrokeWidths[kStrokeLevels] = {1, 2, 3, 5, 8, 12, 18};

// 角數 / 邊數的綠點繞一圈分成幾格（每個可選的數字一格）。
constexpr int kPointSteps = kMaxPoints - kMinPoints + 1;
// 對話框尾巴根部的半寬（弧度）。
constexpr float kTailHalfAngle = 0.35f;
// 文字方塊最窄多寬、馬賽克最小邊長、放大鏡最小半徑（圖片像素）。
constexpr float kMinTextWidth = 20.0f, kMinMosaicSide = 8.0f, kMinMagnifierRadius = 8.0f;
// 正多邊形的邊數綠點所在的圓，半徑是外框的幾分之幾。
constexpr float kPolygonDialScale = 0.5f;
// 命中判斷時曲線切成幾段。
constexpr int kCurveSegments = 32;

Rc pointRect(Pt p) { return {p.x, p.y, p.x, p.y}; }

Pt onEllipse(const Rc& r, float ang, float scale = 1.0f) {
    Pt c = r.center();
    return {c.x + r.width() * 0.5f * scale * std::cos(ang), c.y + r.height() * 0.5f * scale * std::sin(ang)};
}

float angleFromCenter(const Rc& r, Pt p) {
    Pt c = r.center();
    float rx = std::max(1.0f, r.width() * 0.5f), ry = std::max(1.0f, r.height() * 0.5f);
    return std::atan2((p.y - c.y) / ry, (p.x - c.x) / rx);
}

// ---- 綠點的角度對應 ----

float countAngle(int count) { return -kPi / 2 + (count - kMinPoints) * (2 * kPi / kPointSteps); }

int countFromAngle(float ang, int current) {
    // ang 轉成從頂點順時針的 0..2π
    float t = ang + kPi / 2;
    while (t < 0) t += 2 * kPi;
    while (t >= 2 * kPi) t -= 2 * kPi;
    int n = kMinPoints + static_cast<int>(std::lround(t / (2 * kPi / kPointSteps)));
    n = std::clamp(n, kMinPoints, kMaxPoints);
    // 轉過頭繞一圈回來時不跳變。
    if (std::abs(n - current) > kPointSteps / 2) return current;
    return n;
}

float zoomAngle(float zoom) {
    float t = (zoom - kMinZoom) / (kMaxZoom - kMinZoom);
    return -0.75f * kPi + t * 0.9f * kPi;
}

float zoomFromAngle(float ang) {
    float t = (ang + 0.75f * kPi) / (0.9f * kPi);
    return kMinZoom + std::clamp(t, 0.0f, 1.0f) * (kMaxZoom - kMinZoom);
}

// 星形與多邊形第一個頂點的角度：正上方，整張圖每逆時針轉 90 度就跟著退 90 度。
float firstVertexAngle(int turns) { return -kPi / 2 - turns * (kPi / 2); }

Rc magnifierBox(const MagnifierShape& s) { return Rc::fromCenter(s.center, s.radius, s.radius); }

std::vector<Pt> curvePoints(const LineShape& s) {
    std::vector<Pt> pts;
    Pt c = lineControlPoint(s);
    for (int i = 0; i <= kCurveSegments; ++i) pts.push_back(quadBezier(s.p0, c, s.p1, static_cast<float>(i) / kCurveSegments));
    return pts;
}

bool nearPolyline(const std::vector<Pt>& pts, Pt p, float tol, bool closed) {
    if (pts.empty()) return false;
    if (pts.size() == 1) return dist(pts[0], p) <= tol;
    for (size_t i = 0; i + 1 < pts.size(); ++i)
        if (distToSegment(p, pts[i], pts[i + 1]) <= tol) return true;
    if (closed && distToSegment(p, pts.back(), pts.front()) <= tol) return true;
    return false;
}

bool insidePolygon(const std::vector<Pt>& pts, Pt p) {
    bool in = false;
    for (size_t i = 0, j = pts.size() - 1; i < pts.size(); j = i++) {
        if ((pts[i].y > p.y) != (pts[j].y > p.y) &&
            p.x < (pts[j].x - pts[i].x) * (p.y - pts[i].y) / (pts[j].y - pts[i].y) + pts[i].x)
            in = !in;
    }
    return in;
}

bool hitOutline(const std::vector<Pt>& pts, Pt p, float half) { return insidePolygon(pts, p) || nearPolyline(pts, p, half, true); }

// 下面每一組函式都是「每種物件一個多載」，由檔案後半的 std::visit 呼叫。加一種物件時每一組都要補，漏了編譯不過。

// ---- 外框 ----

Rc* rectPtr(FreehandShape& s) { return &s.rect; }
Rc* rectPtr(LineShape&) { return nullptr; }
Rc* rectPtr(RectShape& s) { return &s.rect; }
Rc* rectPtr(RoundRectShape& s) { return &s.rect; }
Rc* rectPtr(EllipseShape& s) { return &s.rect; }
Rc* rectPtr(BubbleShape& s) { return &s.rect; }
Rc* rectPtr(StarShape& s) { return &s.rect; }
Rc* rectPtr(PolygonShape& s) { return &s.rect; }
Rc* rectPtr(MagnifierShape&) { return nullptr; }
Rc* rectPtr(MosaicShape& s) { return &s.rect; }
Rc* rectPtr(TextShape& s) { return &s.rect; }

// ---- 範圍 ----

Rc boundsOf(const FreehandShape& s) { return s.rect; }
Rc boundsOf(const LineShape& s) { return Rc::fromPoints(s.p0, s.p1).unite(pointRect(s.mid)); }
Rc boundsOf(const RectShape& s) { return s.rect; }
Rc boundsOf(const RoundRectShape& s) { return s.rect; }
Rc boundsOf(const EllipseShape& s) { return s.rect; }
Rc boundsOf(const BubbleShape& s) { return s.rect.unite(pointRect(s.tailTip)); }
Rc boundsOf(const StarShape& s) { return s.rect; }
Rc boundsOf(const PolygonShape& s) { return s.rect; }
Rc boundsOf(const MagnifierShape& s) { return magnifierBox(s); }
Rc boundsOf(const MosaicShape& s) { return s.rect; }
Rc boundsOf(const TextShape& s) { return s.rect; }

// ---- 控制點 ----

std::vector<Handle> handlesOf(const FreehandShape& s) {
    auto all = rectHandles(s.rect);
    return {all[H_TL], all[H_TR], all[H_BR], all[H_BL]};
}
std::vector<Handle> handlesOf(const LineShape& s) {
    return {{LineShape::Start, s.p0, HandleKind::Blue}, {LineShape::End, s.p1, HandleKind::Blue}, {LineShape::Bend, s.mid, HandleKind::Green}};
}
std::vector<Handle> handlesOf(const RectShape& s) { return rectHandles(s.rect); }
std::vector<Handle> handlesOf(const RoundRectShape& s) { return rectHandles(s.rect); }
std::vector<Handle> handlesOf(const EllipseShape& s) { return rectHandles(s.rect); }
std::vector<Handle> handlesOf(const BubbleShape& s) {
    auto h = rectHandles(s.rect);
    h.push_back({BubbleShape::TailRoot, onEllipse(s.rect, s.tailAngle), HandleKind::Green});
    h.push_back({BubbleShape::TailTip, s.tailTip, HandleKind::Green});
    return h;
}
std::vector<Handle> handlesOf(const StarShape& s) {
    auto h = rectHandles(s.rect);
    float ang = countAngle(s.pointCount);
    h.push_back({StarShape::Count, onEllipse(s.rect, ang), HandleKind::Green});
    h.push_back({StarShape::Inner, onEllipse(s.rect, ang, std::clamp(s.innerRatio, kMinInnerRatio, 1.0f)), HandleKind::Green});
    return h;
}
std::vector<Handle> handlesOf(const PolygonShape& s) {
    auto h = rectHandles(s.rect);
    h.push_back({PolygonShape::Count, onEllipse(s.rect, countAngle(s.pointCount), kPolygonDialScale), HandleKind::Green});
    return h;
}
std::vector<Handle> handlesOf(const MagnifierShape& s) {
    Rc r = magnifierBox(s);
    return {{MagnifierShape::Radius, onEllipse(r, kPi / 4), HandleKind::Blue},
            {MagnifierShape::Zoom, onEllipse(r, zoomAngle(s.zoom)), HandleKind::Green}};
}
std::vector<Handle> handlesOf(const MosaicShape& s) {
    float t = static_cast<float>(s.blockSize - kMinBlock) / (kMaxBlock - kMinBlock);
    return {{MosaicShape::Corner, {s.rect.right, s.rect.bottom}, HandleKind::Blue},
            {MosaicShape::Block, {s.rect.left + s.rect.width() * t, s.rect.top}, HandleKind::Green}};
}
std::vector<Handle> handlesOf(const TextShape& s) {
    auto all = rectHandles(s.rect);
    return {all[H_L], all[H_R]};
}

// ---- 綠點的軌跡提示 ----

HandleGuide circleGuide(const Rc& r, float scale = 1.0f) {
    HandleGuide g;
    g.kind = GuideKind::Circle;
    g.ellipse = Rc::fromCenter(r.center(), r.width() * 0.5f * scale, r.height() * 0.5f * scale);
    return g;
}
HandleGuide lineGuide(Pt a, Pt b) {
    HandleGuide g;
    g.kind = GuideKind::Line;
    g.a = a;
    g.b = b;
    return g;
}

HandleGuide guideOf(const FreehandShape&, int) { return {}; }
HandleGuide guideOf(const LineShape&, int) { return {}; }
HandleGuide guideOf(const RectShape&, int) { return {}; }
HandleGuide guideOf(const RoundRectShape&, int) { return {}; }
HandleGuide guideOf(const EllipseShape&, int) { return {}; }
HandleGuide guideOf(const BubbleShape& s, int id) { return id == BubbleShape::TailRoot ? circleGuide(s.rect) : HandleGuide{}; }
HandleGuide guideOf(const StarShape& s, int id) {
    if (id == StarShape::Count) return circleGuide(s.rect);
    if (id == StarShape::Inner) return lineGuide(s.rect.center(), onEllipse(s.rect, countAngle(s.pointCount)));
    return {};
}
HandleGuide guideOf(const PolygonShape& s, int id) {
    return id == PolygonShape::Count ? circleGuide(s.rect, kPolygonDialScale) : HandleGuide{};
}
HandleGuide guideOf(const MagnifierShape& s, int id) { return id == MagnifierShape::Zoom ? circleGuide(magnifierBox(s)) : HandleGuide{}; }
HandleGuide guideOf(const MosaicShape& s, int id) {
    return id == MosaicShape::Block ? lineGuide({s.rect.left, s.rect.top}, {s.rect.right, s.rect.top}) : HandleGuide{};
}
HandleGuide guideOf(const TextShape&, int) { return {}; }

// ---- 命中。half 是線寬的一半加容差，tol 是容差本身。----

bool hit(const FreehandShape& s, const StrokeStyle& st, Pt p, float, float tol) {
    // 畫的時候筆跡連同線寬一起照外框非等比縮放，所以筆尖是橢圓：半軸是線寬一半乘上兩個方向的倍率。
    // 把座標各除以半軸（加容差），筆尖變成單位圓，再用一般的點到折線距離判斷。
    const float hx = std::max(1e-3f, st.width * 0.5f * s.rect.width() / s.origW + tol);
    const float hy = std::max(1e-3f, st.width * 0.5f * s.rect.height() / s.origH + tol);
    auto unit = [&](Pt q) { return Pt{q.x / hx, q.y / hy}; };
    std::vector<Pt> pts = freehandPoints(s);
    for (Pt& q : pts) q = unit(q);
    return nearPolyline(pts, unit(p), 1.0f, false);
}
bool hit(const LineShape& s, const StrokeStyle&, Pt p, float half, float) { return nearPolyline(curvePoints(s), p, half, false); }
bool hit(const RectShape& s, const StrokeStyle&, Pt p, float half, float) { return s.rect.inflate(half).contains(p); }
bool hit(const RoundRectShape& s, const StrokeStyle&, Pt p, float half, float) { return s.rect.inflate(half).contains(p); }
bool hit(const EllipseShape& s, const StrokeStyle&, Pt p, float half, float) {
    Pt c = s.rect.center();
    float rx = s.rect.width() * 0.5f + half, ry = s.rect.height() * 0.5f + half;
    if (rx <= 0 || ry <= 0) return false;
    float dx = (p.x - c.x) / rx, dy = (p.y - c.y) / ry;
    return dx * dx + dy * dy <= 1.0f;
}
bool hit(const BubbleShape& s, const StrokeStyle&, Pt p, float half, float) { return hitOutline(bubbleOutline(s), p, half); }
bool hit(const StarShape& s, const StrokeStyle&, Pt p, float half, float) { return hitOutline(starPoints(s), p, half); }
bool hit(const PolygonShape& s, const StrokeStyle&, Pt p, float half, float) { return hitOutline(starPoints(s), p, half); }
bool hit(const MagnifierShape& s, const StrokeStyle&, Pt p, float half, float) { return dist(s.center, p) <= s.radius + half; }
bool hit(const MosaicShape& s, const StrokeStyle&, Pt p, float half, float) { return s.rect.inflate(half).contains(p); }
bool hit(const TextShape& s, const StrokeStyle&, Pt p, float, float tol) { return s.rect.inflate(tol).contains(p); }

// ---- 搬移 ----

void shift(FreehandShape& s, Pt d) { s.rect = s.rect.offset(d.x, d.y); }
void shift(LineShape& s, Pt d) { s.p0 = s.p0 + d; s.p1 = s.p1 + d; s.mid = s.mid + d; }
void shift(RectShape& s, Pt d) { s.rect = s.rect.offset(d.x, d.y); }
void shift(RoundRectShape& s, Pt d) { s.rect = s.rect.offset(d.x, d.y); }
void shift(EllipseShape& s, Pt d) { s.rect = s.rect.offset(d.x, d.y); }
void shift(BubbleShape& s, Pt d) { s.rect = s.rect.offset(d.x, d.y); s.tailTip = s.tailTip + d; }
void shift(StarShape& s, Pt d) { s.rect = s.rect.offset(d.x, d.y); }
void shift(PolygonShape& s, Pt d) { s.rect = s.rect.offset(d.x, d.y); }
void shift(MagnifierShape& s, Pt d) { s.center = s.center + d; }
void shift(MosaicShape& s, Pt d) { s.rect = s.rect.offset(d.x, d.y); }
void shift(TextShape& s, Pt d) { s.rect = s.rect.offset(d.x, d.y); }

// ---- 拖控制點 ----

bool isRectHandle(int id) { return id >= H_TL && id <= H_L; }

bool dragBox(Rc& rect, int id, Pt p) {
    if (!isRectHandle(id)) return false;
    rect = dragRectHandle(rect, id, p);
    return true;
}

bool drag(FreehandShape& s, int id, Pt p) {
    if (id != H_TL && id != H_TR && id != H_BR && id != H_BL) return false;
    s.rect = dragRectHandle(s.rect, id, p);
    if (s.rect.width() < 1) s.rect.right = s.rect.left + 1;
    if (s.rect.height() < 1) s.rect.bottom = s.rect.top + 1;
    return true;
}
bool drag(LineShape& s, int id, Pt p) {
    if (id == LineShape::Bend) { s.mid = p; return true; }
    if (id != LineShape::Start && id != LineShape::End) return false;
    // 端點移動時，中點保持相對於兩端中點的偏移。
    Pt offset = s.mid - lerp(s.p0, s.p1, 0.5f);
    if (id == LineShape::Start) s.p0 = p; else s.p1 = p;
    s.mid = lerp(s.p0, s.p1, 0.5f) + offset;
    return true;
}
bool drag(RectShape& s, int id, Pt p) { return dragBox(s.rect, id, p); }
bool drag(RoundRectShape& s, int id, Pt p) { return dragBox(s.rect, id, p); }
bool drag(EllipseShape& s, int id, Pt p) { return dragBox(s.rect, id, p); }
bool drag(BubbleShape& s, int id, Pt p) {
    if (id == BubbleShape::TailRoot) { s.tailAngle = angleFromCenter(s.rect, p); return true; }
    if (id == BubbleShape::TailTip) { s.tailTip = p; return true; }
    return dragBox(s.rect, id, p);
}
bool drag(StarShape& s, int id, Pt p) {
    if (id == StarShape::Count) { s.pointCount = countFromAngle(angleFromCenter(s.rect, p), s.pointCount); return true; }
    if (id == StarShape::Inner) {
        // 沿「中心到外側綠點」的方向投影，距離 / 外半徑 = 內半徑比例。先把點換到單位圓座標。
        Pt c = s.rect.center();
        float ang = countAngle(s.pointCount);
        float rx = std::max(1.0f, s.rect.width() * 0.5f), ry = std::max(1.0f, s.rect.height() * 0.5f);
        float proj = (p.x - c.x) / rx * std::cos(ang) + (p.y - c.y) / ry * std::sin(ang);
        s.innerRatio = std::clamp(proj, kMinInnerRatio, 1.0f);
        return true;
    }
    return dragBox(s.rect, id, p);
}
bool drag(PolygonShape& s, int id, Pt p) {
    if (id == PolygonShape::Count) { s.pointCount = countFromAngle(angleFromCenter(s.rect, p), s.pointCount); return true; }
    return dragBox(s.rect, id, p);
}
bool drag(MagnifierShape& s, int id, Pt p) {
    if (id == MagnifierShape::Radius) { s.radius = std::max(kMinMagnifierRadius, dist(s.center, p)); return true; }
    if (id == MagnifierShape::Zoom) { s.zoom = zoomFromAngle(std::atan2(p.y - s.center.y, p.x - s.center.x)); return true; }
    return false;
}
bool drag(MosaicShape& s, int id, Pt p) {
    if (id == MosaicShape::Corner) {
        s.rect.right = std::max(s.rect.left + kMinMosaicSide, p.x);
        s.rect.bottom = std::max(s.rect.top + kMinMosaicSide, p.y);
        return true;
    }
    if (id == MosaicShape::Block) {
        float t = std::clamp((p.x - s.rect.left) / std::max(1.0f, s.rect.width()), 0.0f, 1.0f);
        s.blockSize = kMinBlock + static_cast<int>(std::lround(t * (kMaxBlock - kMinBlock)));
        return true;
    }
    return false;
}
bool drag(TextShape& s, int id, Pt p) {
    if (id == H_L) { s.rect.left = std::min(p.x, s.rect.right - kMinTextWidth); return true; }
    if (id == H_R) { s.rect.right = std::max(p.x, s.rect.left + kMinTextWidth); return true; }
    return false;
}

// ---- 整張圖逆時針轉 90 度：(x, y) -> (y, W - x) ----

struct Quarter {
    float imageWidth;
    Pt operator()(Pt p) const { return {p.y, imageWidth - p.x}; }
    Rc operator()(const Rc& r) const { return Rc::fromPoints((*this)(Pt{r.left, r.top}), (*this)(Pt{r.right, r.bottom})); }
};

void rotate(FreehandShape& s, const Quarter& q) {
    // 筆跡本身也要轉：相對座標 (x, y) -> (y, origW - x)，外框跟著換邊。
    for (auto& p : s.points) p = {p.y, s.origW - p.x};
    std::swap(s.origW, s.origH);
    s.rect = q(s.rect);
}
void rotate(LineShape& s, const Quarter& q) { s.p0 = q(s.p0); s.p1 = q(s.p1); s.mid = q(s.mid); }
void rotate(RectShape& s, const Quarter& q) { s.rect = q(s.rect); }
void rotate(RoundRectShape& s, const Quarter& q) { s.rect = q(s.rect); }
void rotate(EllipseShape& s, const Quarter& q) { s.rect = q(s.rect); }
void rotate(BubbleShape& s, const Quarter& q) {
    s.rect = q(s.rect);
    s.tailTip = q(s.tailTip);
    s.tailAngle -= kPi / 2;
}
void rotate(StarShape& s, const Quarter& q) { s.rect = q(s.rect); s.turns = (s.turns + 1) % 4; }
void rotate(PolygonShape& s, const Quarter& q) { s.rect = q(s.rect); s.turns = (s.turns + 1) % 4; }
void rotate(MagnifierShape& s, const Quarter& q) { s.center = q(s.center); }
void rotate(MosaicShape& s, const Quarter& q) { s.rect = q(s.rect); }
void rotate(TextShape& s, const Quarter& q) { s.rect = q(s.rect); } // 字不轉，只有方塊換位置（高度之後重排）

// ---- 整張圖重新取樣 ----

struct Scale {
    float sx, sy;
    Pt operator()(Pt p) const { return {p.x * sx, p.y * sy}; }
    Rc operator()(const Rc& r) const { return {r.left * sx, r.top * sy, r.right * sx, r.bottom * sy}; }
};

void scale(FreehandShape& s, const Scale& k) { s.rect = k(s.rect); }
void scale(LineShape& s, const Scale& k) { s.p0 = k(s.p0); s.p1 = k(s.p1); s.mid = k(s.mid); }
void scale(RectShape& s, const Scale& k) { s.rect = k(s.rect); }
void scale(RoundRectShape& s, const Scale& k) { s.rect = k(s.rect); }
void scale(EllipseShape& s, const Scale& k) { s.rect = k(s.rect); }
void scale(BubbleShape& s, const Scale& k) { s.rect = k(s.rect); s.tailTip = k(s.tailTip); }
void scale(StarShape& s, const Scale& k) { s.rect = k(s.rect); }
void scale(PolygonShape& s, const Scale& k) { s.rect = k(s.rect); }
void scale(MagnifierShape& s, const Scale& k) { s.center = k(s.center); s.radius *= std::min(k.sx, k.sy); }
void scale(MosaicShape& s, const Scale& k) { s.rect = k(s.rect); }
void scale(TextShape& s, const Scale& k) { s.rect = k(s.rect); }

} // namespace

float strokeWidthForLevel(int level) { return kStrokeWidths[std::clamp(level, 0, kStrokeLevels - 1)]; }

int strokeLevelForWidth(float width) {
    for (int i = 0; i < kStrokeLevels; ++i)
        if (kStrokeWidths[i] == width) return i;
    return -1;
}

const wchar_t* kindName(Kind kind) {
    switch (kind) {
    case Kind::Freehand: return L"Freehand";
    case Kind::Line: return L"Line";
    case Kind::Rect: return L"Rect";
    case Kind::RoundRect: return L"RoundRect";
    case Kind::Ellipse: return L"Ellipse";
    case Kind::Bubble: return L"Bubble";
    case Kind::Star: return L"Star";
    case Kind::Polygon: return L"Polygon";
    case Kind::Magnifier: return L"Magnifier";
    case Kind::Mosaic: return L"Mosaic";
    case Kind::Text: return L"Text";
    }
    return L"?";
}

// ---- 建立 ----

Annotation makeShape(Kind kind, Pt c, float size, const StrokeStyle& style) {
    Annotation a;
    a.stroke = style;
    const float half = std::max(8.0f, size * 0.15f);
    const Rc box = Rc::fromCenter(c, half, half);
    switch (kind) {
    case Kind::Freehand: a.shape = FreehandShape{box}; break;
    case Kind::Line: a.shape = LineShape{{c.x - half, c.y}, {c.x + half, c.y}, c}; break;
    case Kind::Rect: a.shape = RectShape{box}; break;
    case Kind::RoundRect: a.shape = RoundRectShape{box}; break;
    case Kind::Ellipse: a.shape = EllipseShape{box}; break;
    case Kind::Bubble: {
        BubbleShape s;
        s.rect = Rc::fromCenter(c, half * 1.3f, half);
        s.tailTip = {c.x - half * 1.3f, c.y + half * 1.8f};
        a.shape = s;
        break;
    }
    case Kind::Star: a.shape = StarShape{box}; break;
    case Kind::Polygon: a.shape = PolygonShape{box}; break;
    case Kind::Magnifier: a.shape = MagnifierShape{c, half}; break;
    case Kind::Mosaic: a.shape = MosaicShape{box}; break;
    case Kind::Text: a.shape = TextShape{box}; break;
    }
    return a;
}

Annotation makeText(Pt c, float size, const StrokeStyle& style, const TextStyle& textStyle, const std::wstring& text) {
    Annotation a;
    a.stroke = style;
    a.stroke.stroke = Color::noColor(); // 預設沒有外框
    float halfW = std::max(40.0f, size * 0.2f);
    a.shape = TextShape{{c.x - halfW, c.y - textStyle.size, c.x + halfW, c.y + textStyle.size}, textStyle, text};
    return a;
}

Annotation makeFreehand(const std::vector<Pt>& path, const StrokeStyle& style) {
    Annotation a;
    a.stroke = style;
    Rc b = pointRect(path[0]);
    for (auto p : path) b = b.unite(pointRect(p));
    FreehandShape s;
    s.origW = std::max(1.0f, b.width());
    s.origH = std::max(1.0f, b.height());
    s.rect = {b.left, b.top, b.left + s.origW, b.top + s.origH};
    s.points.reserve(path.size());
    for (auto p : path) s.points.push_back(p - b.topLeft());
    a.shape = std::move(s);
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

std::vector<Pt> freehandPoints(const FreehandShape& s) {
    std::vector<Pt> out;
    out.reserve(s.points.size());
    float sx = s.rect.width() / s.origW, sy = s.rect.height() / s.origH;
    for (auto p : s.points) out.push_back({s.rect.left + p.x * sx, s.rect.top + p.y * sy});
    return out;
}

Pt lineControlPoint(const LineShape& s) {
    // 曲線在 t=0.5 的點是 (p0 + 2c + p1) / 4，反推 c。
    return s.mid * 2.0f - lerp(s.p0, s.p1, 0.5f);
}

std::vector<Pt> starPoints(const PolygonShape& s) {
    std::vector<Pt> pts;
    const int n = std::clamp(s.pointCount, kMinPoints, kMaxPoints);
    const float first = firstVertexAngle(s.turns);
    for (int i = 0; i < n; ++i) pts.push_back(onEllipse(s.rect, first + i * 2 * kPi / n));
    return pts;
}

std::vector<Pt> starPoints(const StarShape& s) {
    std::vector<Pt> pts;
    const int n = std::clamp(s.pointCount, kMinPoints, kMaxPoints);
    const float inner = std::clamp(s.innerRatio, kMinInnerRatio, 1.0f);
    const float first = firstVertexAngle(s.turns);
    for (int i = 0; i < 2 * n; ++i) pts.push_back(onEllipse(s.rect, first + i * kPi / n, (i % 2 == 0) ? 1.0f : inner));
    return pts;
}

void bubbleTail(const BubbleShape& s, Pt& rootA, Pt& rootB, Pt& tip) {
    rootA = onEllipse(s.rect, s.tailAngle + kTailHalfAngle);
    rootB = onEllipse(s.rect, s.tailAngle - kTailHalfAngle);
    tip = s.tailTip;
}

std::vector<Pt> bubbleOutline(const BubbleShape& s) {
    // 從 rootA 順時針繞一圈到 rootB（跳過尾巴那一段），再到尖端。
    std::vector<Pt> pts;
    const int segs = 48;
    float start = s.tailAngle + kTailHalfAngle;
    float sweep = 2 * kPi - 2 * kTailHalfAngle;
    for (int i = 0; i <= segs; ++i) pts.push_back(onEllipse(s.rect, start + sweep * i / segs));
    pts.push_back(s.tailTip);
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

// ---- 依物件種類分派 ----

Rc* rectOf(Annotation& a) {
    return std::visit([](auto& s) { return rectPtr(s); }, a.shape);
}

const Rc* rectOf(const Annotation& a) { return rectOf(const_cast<Annotation&>(a)); }

Rc bounds(const Annotation& a) {
    return std::visit([](const auto& s) { return boundsOf(s); }, a.shape);
}

float paintedStrokeWidth(const Annotation& a) {
    if (const auto* f = a.as<FreehandShape>()) return a.stroke.width * std::max(f->rect.width() / f->origW, f->rect.height() / f->origH);
    return a.stroke.width;
}

std::vector<Handle> handles(const Annotation& a) {
    return std::visit([](const auto& s) { return handlesOf(s); }, a.shape);
}

HandleGuide handleGuide(const Annotation& a, int id) {
    return std::visit([id](const auto& s) { return guideOf(s, id); }, a.shape);
}

bool hitTest(const Annotation& a, Pt p, float tol) {
    const float half = a.stroke.width * 0.5f + tol;
    return std::visit([&](const auto& s) { return hit(s, a.stroke, p, half, tol); }, a.shape);
}

void moveBy(Annotation& a, float dx, float dy) {
    std::visit([&](auto& s) { shift(s, Pt{dx, dy}); }, a.shape);
}

bool dragHandle(Annotation& a, int id, Pt p) {
    return std::visit([&](auto& s) { return drag(s, id, p); }, a.shape);
}

void rotateCCW90(Annotation& a, int imageWidth) {
    const Quarter q{static_cast<float>(imageWidth)};
    std::visit([&](auto& s) { rotate(s, q); }, a.shape);
}

void scaleBy(Annotation& a, float sx, float sy) {
    const Scale k{sx, sy};
    std::visit([&](auto& s) { scale(s, k); }, a.shape);
}

} // namespace mp
