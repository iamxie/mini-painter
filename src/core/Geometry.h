#pragma once
#include <algorithm>
#include <cmath>

namespace mp {

constexpr float kPi = 3.14159265358979f;

struct Pt {
    float x = 0, y = 0;
};

inline Pt operator+(Pt a, Pt b) { return {a.x + b.x, a.y + b.y}; }
inline Pt operator-(Pt a, Pt b) { return {a.x - b.x, a.y - b.y}; }
inline Pt operator*(Pt a, float s) { return {a.x * s, a.y * s}; }
inline bool operator==(Pt a, Pt b) { return a.x == b.x && a.y == b.y; }
inline float dist(Pt a, Pt b) { return std::hypot(a.x - b.x, a.y - b.y); }
inline Pt lerp(Pt a, Pt b, float t) { return {a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t}; }

struct Rc {
    float left = 0, top = 0, right = 0, bottom = 0;

    float width() const { return right - left; }
    float height() const { return bottom - top; }
    Pt center() const { return {(left + right) * 0.5f, (top + bottom) * 0.5f}; }
    Pt topLeft() const { return {left, top}; }
    bool contains(Pt p) const { return p.x >= left && p.x <= right && p.y >= top && p.y <= bottom; }
    bool empty() const { return right <= left || bottom <= top; }

    // 左右、上下顛倒時翻正。
    Rc normalized() const {
        return {std::min(left, right), std::min(top, bottom), std::max(left, right), std::max(top, bottom)};
    }
    Rc inflate(float d) const { return {left - d, top - d, right + d, bottom + d}; }
    Rc offset(float dx, float dy) const { return {left + dx, top + dy, right + dx, bottom + dy}; }
    Rc unite(const Rc& o) const {
        return {std::min(left, o.left), std::min(top, o.top), std::max(right, o.right), std::max(bottom, o.bottom)};
    }
    static Rc fromCenter(Pt c, float halfW, float halfH) {
        return {c.x - halfW, c.y - halfH, c.x + halfW, c.y + halfH};
    }
    static Rc fromPoints(Pt a, Pt b) { return Rc{a.x, a.y, b.x, b.y}.normalized(); }
};

inline bool operator==(const Rc& a, const Rc& b) {
    return a.left == b.left && a.top == b.top && a.right == b.right && a.bottom == b.bottom;
}

// 點到線段的距離。
inline float distToSegment(Pt p, Pt a, Pt b) {
    Pt ab = b - a;
    float len2 = ab.x * ab.x + ab.y * ab.y;
    if (len2 <= 0.0f) return dist(p, a);
    float t = ((p.x - a.x) * ab.x + (p.y - a.y) * ab.y) / len2;
    t = std::clamp(t, 0.0f, 1.0f);
    return dist(p, a + ab * t);
}

// 二次貝茲曲線上的點。
inline Pt quadBezier(Pt p0, Pt c, Pt p1, float t) {
    float u = 1 - t;
    return p0 * (u * u) + c * (2 * u * t) + p1 * (t * t);
}

inline float clampf(float v, float lo, float hi) { return std::clamp(v, lo, hi); }

} // namespace mp
