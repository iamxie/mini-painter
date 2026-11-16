#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "core/Geometry.h"

using namespace mp;
using Catch::Matchers::WithinAbs;

TEST_CASE("Rc normalized 會翻正顛倒的矩形") {
    Rc r{10, 20, 0, 5};
    Rc n = r.normalized();
    CHECK(n.left == 0);
    CHECK(n.top == 5);
    CHECK(n.right == 10);
    CHECK(n.bottom == 20);
}

TEST_CASE("Rc contains 與 inflate") {
    Rc r{0, 0, 10, 10};
    CHECK(r.contains({5, 5}));
    CHECK_FALSE(r.contains({11, 5}));
    CHECK(r.inflate(2).contains({11, 5}));
    CHECK(r.fromCenter({5, 5}, 5, 5) == r);
}

TEST_CASE("distToSegment") {
    CHECK_THAT(distToSegment({5, 3}, {0, 0}, {10, 0}), WithinAbs(3.0f, 1e-5f));
    CHECK_THAT(distToSegment({-4, 3}, {0, 0}, {10, 0}), WithinAbs(5.0f, 1e-5f));
    CHECK_THAT(distToSegment({1, 1}, {2, 2}, {2, 2}), WithinAbs(std::sqrt(2.0f), 1e-5f));
}

TEST_CASE("quadBezier 端點與中點") {
    Pt p0{0, 0}, c{5, 10}, p1{10, 0};
    CHECK(quadBezier(p0, c, p1, 0) == p0);
    CHECK(quadBezier(p0, c, p1, 1) == p1);
    Pt m = quadBezier(p0, c, p1, 0.5f);
    CHECK_THAT(m.x, WithinAbs(5.0f, 1e-5f));
    CHECK_THAT(m.y, WithinAbs(5.0f, 1e-5f));
}

TEST_CASE("Pt 的運算、dist、lerp") {
    Pt a{1, 2}, b{4, 6};
    CHECK(a + b == Pt{5, 8});
    CHECK(b - a == Pt{3, 4});
    CHECK(a * 2.0f == Pt{2, 4});
    CHECK_THAT(dist(a, b), WithinAbs(5.0f, 1e-5f));
    CHECK(lerp(a, b, 0) == a);
    CHECK(lerp(a, b, 1) == b);
    CHECK(lerp(a, b, 0.5f) == Pt{2.5f, 4});
}

TEST_CASE("Rc 的寬高、中心、左上角、empty、offset") {
    Rc r{10, 20, 40, 60};
    CHECK(r.width() == 30);
    CHECK(r.height() == 40);
    CHECK(r.center() == Pt{25, 40});
    CHECK(r.topLeft() == Pt{10, 20});
    CHECK_FALSE(r.empty());
    CHECK(Rc{}.empty());
    CHECK(Rc{5, 5, 5, 9}.empty());  // 沒有寬
    CHECK(Rc{5, 9, 8, 5}.empty());  // 上下顛倒也算空的
    CHECK(r.offset(-10, 5) == Rc{0, 25, 30, 65});
    CHECK(r.contains(r.topLeft())); // 邊上算在裡面
}

TEST_CASE("Rc::unite：包住兩個矩形的最小矩形") {
    Rc a{0, 0, 10, 10}, b{5, -5, 20, 8};
    CHECK(a.unite(b) == Rc{0, -5, 20, 10});
    CHECK(b.unite(a) == a.unite(b));
    CHECK(a.unite(a) == a);
    CHECK(a.unite(Rc{2, 2, 3, 3}) == a); // 完全在裡面
    CHECK(a.unite(Rc{30, 40, 31, 41}) == Rc{0, 0, 31, 41}); // 不相交也是包住兩個
}

TEST_CASE("Rc::fromPoints：兩個對角，順序不拘") {
    const Rc want{1, 2, 7, 9};
    CHECK(Rc::fromPoints({1, 2}, {7, 9}) == want);
    CHECK(Rc::fromPoints({7, 9}, {1, 2}) == want);
    CHECK(Rc::fromPoints({7, 2}, {1, 9}) == want);
    CHECK(Rc::fromPoints({3, 3}, {3, 3}).empty());
}
