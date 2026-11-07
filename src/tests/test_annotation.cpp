#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "core/Annotation.h"

using namespace mp;
using Catch::Matchers::WithinAbs;

static StrokeStyle style() { return StrokeStyle{}; }

TEST_CASE("strokeWidthForLevel 單調遞增並夾在範圍內") {
    for (int i = 1; i < kStrokeLevels; ++i) CHECK(strokeWidthForLevel(i) > strokeWidthForLevel(i - 1));
    CHECK(strokeWidthForLevel(-5) == strokeWidthForLevel(0));
    CHECK(strokeWidthForLevel(99) == strokeWidthForLevel(kStrokeLevels - 1));
}

TEST_CASE("makeShape 以中心建立矩形") {
    auto a = makeShape(Kind::Rect, {100, 100}, 200, style());
    CHECK(a.kind == Kind::Rect);
    Pt c = a.rect.center();
    CHECK_THAT(c.x, WithinAbs(100.0f, 1e-4f));
    CHECK_THAT(c.y, WithinAbs(100.0f, 1e-4f));
    CHECK(a.rect.width() > 0);
    CHECK(handles(a).size() == 8);
}

TEST_CASE("矩形拖右下角控制點") {
    auto a = makeShape(Kind::Rect, {100, 100}, 200, style());
    REQUIRE(dragHandle(a, H_BR, {300, 250}));
    CHECK(a.rect.right == 300);
    CHECK(a.rect.bottom == 250);
    SECTION("拖過另一邊會翻正") {
        REQUIRE(dragHandle(a, H_L, {400, 0}));
        CHECK(a.rect.left == 300);
        CHECK(a.rect.right == 400);
    }
    SECTION("不存在的控制點回傳 false") { CHECK_FALSE(dragHandle(a, 8, {0, 0})); }
}

TEST_CASE("直線：三個控制點，拖綠點變曲線") {
    auto a = makeShape(Kind::Line, {100, 100}, 200, style());
    auto h = handles(a);
    REQUIRE(h.size() == 3);
    CHECK(h[2].kind == HandleKind::Green);
    CHECK(a.mid == lerp(a.p0, a.p1, 0.5f));
    REQUIRE(dragHandle(a, 2, {100, 50}));
    Pt c = lineControlPoint(a);
    Pt m = quadBezier(a.p0, c, a.p1, 0.5f);
    CHECK_THAT(m.x, WithinAbs(100.0f, 1e-3f));
    CHECK_THAT(m.y, WithinAbs(50.0f, 1e-3f));
    SECTION("移動端點時中點保持偏移") {
        Pt before = a.mid - lerp(a.p0, a.p1, 0.5f);
        REQUIRE(dragHandle(a, 1, {300, 300}));
        Pt after = a.mid - lerp(a.p0, a.p1, 0.5f);
        CHECK_THAT(after.x, WithinAbs(before.x, 1e-3f));
        CHECK_THAT(after.y, WithinAbs(before.y, 1e-3f));
    }
    SECTION("命中：曲線上有，遠處沒有") {
        CHECK(hitTest(a, {100, 50}, 2));
        CHECK_FALSE(hitTest(a, {100, 150}, 2));
    }
}

TEST_CASE("塗鴉：原始筆跡依外框縮放，線寬隨之改變由繪圖端處理") {
    std::vector<Pt> path{{10, 10}, {20, 30}, {50, 10}};
    auto a = makeFreehand(path, style());
    CHECK(a.rect.left == 10);
    CHECK(a.rect.top == 10);
    CHECK(a.origW == 40);
    CHECK(a.origH == 20);
    CHECK(handles(a).size() == 4);
    auto pts = freehandPoints(a);
    REQUIRE(pts.size() == 3);
    CHECK(pts[1] == Pt{20, 30});
    REQUIRE(dragHandle(a, H_BR, {90, 50})); // 寬 80、高 40，各放大 2 倍
    pts = freehandPoints(a);
    CHECK_THAT(pts[1].x, WithinAbs(30.0f, 1e-4f));
    CHECK_THAT(pts[1].y, WithinAbs(50.0f, 1e-4f));
    CHECK(hitTest(a, {30, 50}, 1));
    CHECK_FALSE(hitTest(a, {60, 45}, 1));
}

TEST_CASE("塗鴉只有一個點也能建立") {
    auto a = makeFreehand({{5, 5}}, style());
    CHECK(a.origW == 1);
    CHECK(hitTest(a, {5, 5}, 1));
}

TEST_CASE("星形：角數上下限與方向") {
    auto a = makeShape(Kind::Star, {0, 0}, 200, style());
    CHECK(a.pointCount == 5);
    CHECK(starPoints(a).size() == 10);
    CHECK_THAT(starPoints(a)[0].y, WithinAbs(a.rect.top, 1e-3f)); // 頂點朝上
    CHECK(handles(a).size() == 10);
    auto h = handles(a);
    // 把外側綠點順時針多拖一格，角數 +1。
    Pt c = a.rect.center();
    float r = a.rect.width() / 2;
    float ang = -kPi / 2 + (5 - 3 + 1) * (2 * kPi / 18);
    REQUIRE(dragHandle(a, 8, {c.x + r * std::cos(ang), c.y + r * std::sin(ang)}));
    CHECK(a.pointCount == 6);
    // 繞回頂點附近不會跳到 3。
    REQUIRE(dragHandle(a, 8, {c.x, c.y - r}));
    CHECK(a.pointCount == 3);
    a.pointCount = 20;
    REQUIRE(dragHandle(a, 8, {c.x - 1, c.y - r}));
    CHECK(a.pointCount == 20);
    SECTION("內半徑拖到外框變成多邊形點集") {
        Pt outer = handles(a)[8].pos; // 外側綠點，內側綠點沿中心到它的方向移動
        REQUIRE(dragHandle(a, 9, c + (outer - c) * 2.0f));
        CHECK(a.innerRatio == 1.0f);
        REQUIRE(dragHandle(a, 9, c));
        CHECK(a.innerRatio == 0.05f);
    }
}

TEST_CASE("正多邊形：六邊形預設，頂點朝上") {
    auto a = makeShape(Kind::Polygon, {0, 0}, 200, style());
    CHECK(a.pointCount == 6);
    auto pts = starPoints(a);
    REQUIRE(pts.size() == 6);
    CHECK_THAT(pts[0].x, WithinAbs(0.0f, 1e-3f));
    CHECK_THAT(pts[0].y, WithinAbs(a.rect.top, 1e-3f));
    CHECK(handles(a).size() == 9);
    CHECK(hitTest(a, {0, 0}, 1));
}

TEST_CASE("對話框：尾巴根部角度與尖端") {
    auto a = makeShape(Kind::Bubble, {100, 100}, 200, style());
    auto h = handles(a);
    REQUIRE(h.size() == 10);
    REQUIRE(dragHandle(a, 8, {a.rect.right + 50, 100})); // 根部拖到右邊
    CHECK_THAT(a.tailAngle, WithinAbs(0.0f, 1e-3f));
    REQUIRE(dragHandle(a, 9, {500, 500}));
    CHECK(a.tailTip == Pt{500, 500});
    CHECK(bounds(a).right == 500);
    CHECK(hitTest(a, {100, 100}, 1));
    CHECK(hitTest(a, {499, 499}, 2));
    Pt ra, rb, tip;
    bubbleTail(a, ra, rb, tip);
    CHECK(tip == a.tailTip);
    CHECK(ra.x > a.rect.center().x);
}

TEST_CASE("放大鏡：藍點改半徑，綠點改倍率") {
    auto a = makeShape(Kind::Magnifier, {100, 100}, 200, style());
    REQUIRE(dragHandle(a, 0, {160, 100}));
    CHECK_THAT(a.radius, WithinAbs(60.0f, 1e-4f));
    REQUIRE(dragHandle(a, 1, {100 + std::cos(-0.75f * kPi) * 60, 100 + std::sin(-0.75f * kPi) * 60}));
    CHECK_THAT(a.zoom, WithinAbs(kMinZoom, 1e-3f));
    REQUIRE(dragHandle(a, 1, {100 + std::cos(0.15f * kPi) * 60, 100 + std::sin(0.15f * kPi) * 60}));
    CHECK_THAT(a.zoom, WithinAbs(kMaxZoom, 1e-3f));
    CHECK(hitTest(a, {100, 100}, 0));
    CHECK_FALSE(hitTest(a, {200, 200}, 0));
}

TEST_CASE("馬賽克：綠點沿上邊調格子大小") {
    auto a = makeShape(Kind::Mosaic, {100, 100}, 200, style());
    REQUIRE(dragHandle(a, 1, {a.rect.left, a.rect.top}));
    CHECK(a.blockSize == kMinBlock);
    REQUIRE(dragHandle(a, 1, {a.rect.right + 100, a.rect.top}));
    CHECK(a.blockSize == kMaxBlock);
    REQUIRE(dragHandle(a, 0, {a.rect.left + 2, a.rect.top + 2}));
    CHECK(a.rect.width() == 8);
}

TEST_CASE("文字：只有左右兩個控制點，預設無外框") {
    TextStyle ts;
    auto a = makeText({100, 100}, 200, style(), ts);
    CHECK(a.textValue == L"Text"); // 沒有語系時是英文 key
    CHECK(a.stroke.stroke.none());
    auto h = handles(a);
    REQUIRE(h.size() == 2);
    CHECK(h[0].id == H_L);
    CHECK(h[1].id == H_R);
    REQUIRE(dragHandle(a, H_R, {a.rect.left + 5, 0})); // 不能窄於 20
    CHECK(a.rect.width() == 20);
}

TEST_CASE("moveBy 移動所有座標") {
    auto a = makeShape(Kind::Line, {0, 0}, 100, style());
    moveBy(a, 10, 20);
    CHECK(a.mid == Pt{10, 20});
    auto b = makeShape(Kind::Magnifier, {0, 0}, 100, style());
    moveBy(b, 10, 20);
    CHECK(b.center == Pt{10, 20});
}

TEST_CASE("rotateCCW90 把矩形轉到新座標") {
    Annotation a = makeShape(Kind::Rect, {0, 0}, 100, style());
    a.rect = {10, 20, 30, 50}; // 圖寬 100
    rotateCCW90(a, 100);
    // (x, y) -> (y, 100 - x)：左上 (10,20)->(20,90)，右下 (30,50)->(50,70)
    CHECK(a.rect == Rc{20, 70, 50, 90});
}

TEST_CASE("rotateCCW90 塗鴉筆跡跟著轉") {
    auto a = makeFreehand({{0, 0}, {40, 0}}, style()); // 水平線，origW 40，origH 1
    rotateCCW90(a, 100);
    CHECK(a.origW == 1);
    CHECK(a.origH == 40);
    auto pts = freehandPoints(a);
    CHECK_THAT(pts[0].x, WithinAbs(pts[1].x, 1e-4f)); // 變垂直線
}

TEST_CASE("arrowHead 三角形指向尖端") {
    auto tri = arrowHead({100, 0}, {1, 0}, 2);
    REQUIRE(tri.size() == 3);
    CHECK(tri[0] == Pt{100, 0});
    CHECK(tri[1].x < 100);
    CHECK(tri[2].x < 100);
    CHECK_THAT(tri[1].y, WithinAbs(-tri[2].y, 1e-4f));
}

TEST_CASE("scaleBy 縮放所有座標") {
    auto a = makeShape(Kind::Magnifier, {100, 50}, 200, style());
    float r = a.radius;
    scaleBy(a, 2, 0.5f);
    CHECK(a.center == Pt{200, 25});
    CHECK(a.radius == r * 0.5f);
    auto b = makeShape(Kind::Rect, {0, 0}, 100, style());
    b.rect = {10, 10, 20, 20};
    scaleBy(b, 2, 2);
    CHECK(b.rect == Rc{20, 20, 40, 40});
}

TEST_CASE("handleGuide：綠點的軌跡提示") {
    // 藍點沒有
    auto r = makeShape(Kind::Rect, {100, 100}, 200, style());
    CHECK(handleGuide(r, H_TL).kind == GuideKind::None);
    // 直線的綠點自由移動，沒有
    auto l = makeShape(Kind::Line, {100, 100}, 200, style());
    CHECK(handleGuide(l, 2).kind == GuideKind::None);
    // 多邊形：半徑一半的圓
    auto p = makeShape(Kind::Polygon, {100, 100}, 200, style());
    auto g = handleGuide(p, 8);
    REQUIRE(g.kind == GuideKind::Circle);
    CHECK_THAT(g.ellipse.width(), WithinAbs(p.rect.width() * 0.5f, 1e-3f));
    CHECK(g.ellipse.center() == p.rect.center());
    // 星形：角數綠點繞外圓，內縮綠點是中心到外側綠點的線
    auto s = makeShape(Kind::Star, {100, 100}, 200, style());
    CHECK(handleGuide(s, 8).kind == GuideKind::Circle);
    auto line = handleGuide(s, 9);
    REQUIRE(line.kind == GuideKind::Line);
    CHECK(line.a == s.rect.center());
    Pt outer;
    for (const auto& h : handles(s)) if (h.id == 8) outer = h.pos;
    CHECK_THAT(dist(line.b, outer), WithinAbs(0.0f, 1e-3f));
    // 放大鏡倍率繞圓周
    auto m = makeShape(Kind::Magnifier, {100, 100}, 200, style());
    auto mg = handleGuide(m, 1);
    REQUIRE(mg.kind == GuideKind::Circle);
    CHECK_THAT(mg.ellipse.width(), WithinAbs(m.radius * 2, 1e-3f));
    CHECK(handleGuide(m, 0).kind == GuideKind::None);
    // 馬賽克沿上邊
    auto mo = makeShape(Kind::Mosaic, {100, 100}, 200, style());
    auto ml = handleGuide(mo, 1);
    REQUIRE(ml.kind == GuideKind::Line);
    CHECK(ml.a.y == mo.rect.top);
    CHECK(ml.b.x == mo.rect.right);
    // 對話框尾巴根部繞橢圓
    auto b = makeShape(Kind::Bubble, {100, 100}, 200, style());
    CHECK(handleGuide(b, 8).kind == GuideKind::Circle);
    CHECK(handleGuide(b, 9).kind == GuideKind::None);
}
