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
