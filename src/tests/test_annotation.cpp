#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "core/Annotation.h"

using namespace mp;
using Catch::Matchers::WithinAbs;

static StrokeStyle style() { return StrokeStyle{}; }

// 這個物件自己的資料；種類不對就讓測試停在這裡。
template <class T>
static T& shapeOf(Annotation& a) {
    REQUIRE(a.as<T>() != nullptr);
    return *a.as<T>();
}

static const Kind kAllKinds[] = {Kind::Freehand, Kind::Line,    Kind::Rect,      Kind::RoundRect, Kind::Ellipse, Kind::Bubble,
                                 Kind::Star,     Kind::Polygon, Kind::Magnifier, Kind::Mosaic,    Kind::Text};

// 物件上幾個代表的點（控制點加外框中心），用來看整個物件有沒有一起動。
static std::vector<Pt> landmarks(const Annotation& a) {
    std::vector<Pt> pts{bounds(a).center()};
    for (const Handle& h : handles(a)) pts.push_back(h.pos);
    return pts;
}

TEST_CASE("strokeWidthForLevel 單調遞增並夾在範圍內") {
    for (int i = 1; i < kStrokeLevels; ++i) CHECK(strokeWidthForLevel(i) > strokeWidthForLevel(i - 1));
    CHECK(strokeWidthForLevel(-5) == strokeWidthForLevel(0));
    CHECK(strokeWidthForLevel(99) == strokeWidthForLevel(kStrokeLevels - 1));
}

TEST_CASE("strokeLevelForWidth：選單上的值回那一級，其他回 -1") {
    for (int i = 0; i < kStrokeLevels; ++i) CHECK(strokeLevelForWidth(strokeWidthForLevel(i)) == i);
    CHECK(strokeLevelForWidth(4.0f) == -1);
    CHECK(strokeLevelForWidth(0.0f) == -1);
    CHECK(strokeLevelForWidth(StrokeStyle{}.width) == 2); // 預設線寬是選單的第三級
}

TEST_CASE("makeShape：每一種都建得出來，kind() 是要的那一種") {
    for (Kind k : kAllKinds) {
        auto a = makeShape(k, {100, 100}, 200, style());
        CHECK(a.kind() == k);
        CHECK(bounds(a).contains({100, 100}));
    }
    CHECK(Annotation{}.kind() == Kind::Rect); // 沒指定時是矩形
}

TEST_CASE("rectOf：直線與放大鏡沒有外框，其他都有") {
    for (Kind k : kAllKinds) {
        auto a = makeShape(k, {100, 100}, 200, style());
        const bool boxed = k != Kind::Line && k != Kind::Magnifier;
        CHECK((rectOf(a) != nullptr) == boxed);
        const Annotation& c = a;
        CHECK((rectOf(c) != nullptr) == boxed);
    }
}

TEST_CASE("makeShape 以中心建立矩形") {
    auto a = makeShape(Kind::Rect, {100, 100}, 200, style());
    Pt c = rectOf(a)->center();
    CHECK_THAT(c.x, WithinAbs(100.0f, 1e-4f));
    CHECK_THAT(c.y, WithinAbs(100.0f, 1e-4f));
    CHECK(rectOf(a)->width() > 0);
    CHECK(handles(a).size() == 8);
}

TEST_CASE("矩形類：八個控制點，拖右下角") {
    for (Kind k : {Kind::Rect, Kind::RoundRect, Kind::Ellipse}) {
        auto a = makeShape(k, {100, 100}, 200, style());
        CHECK(handles(a).size() == 8);
        REQUIRE(dragHandle(a, H_BR, {300, 250}));
        CHECK(rectOf(a)->right == 300);
        CHECK(rectOf(a)->bottom == 250);
        // 拖過另一邊會翻正
        REQUIRE(dragHandle(a, H_L, {400, 0}));
        CHECK(rectOf(a)->left == 300);
        CHECK(rectOf(a)->right == 400);
        // 不存在的控制點回傳 false
        CHECK_FALSE(dragHandle(a, 8, {0, 0}));
        CHECK_FALSE(dragHandle(a, -1, {0, 0}));
    }
}

TEST_CASE("直線：三個控制點，拖綠點變曲線") {
    auto a = makeShape(Kind::Line, {100, 100}, 200, style());
    LineShape& l = shapeOf<LineShape>(a);
    auto h = handles(a);
    REQUIRE(h.size() == 3);
    CHECK(h[LineShape::Bend].kind == HandleKind::Green);
    CHECK(l.mid == lerp(l.p0, l.p1, 0.5f));
    REQUIRE(dragHandle(a, LineShape::Bend, {100, 50}));
    Pt c = lineControlPoint(l);
    Pt m = quadBezier(l.p0, c, l.p1, 0.5f);
    CHECK_THAT(m.x, WithinAbs(100.0f, 1e-3f));
    CHECK_THAT(m.y, WithinAbs(50.0f, 1e-3f));
    CHECK_FALSE(dragHandle(a, 3, {0, 0}));
    SECTION("移動端點時中點保持偏移") {
        Pt before = l.mid - lerp(l.p0, l.p1, 0.5f);
        REQUIRE(dragHandle(a, LineShape::End, {300, 300}));
        CHECK(l.p1 == Pt{300, 300});
        Pt after = l.mid - lerp(l.p0, l.p1, 0.5f);
        CHECK_THAT(after.x, WithinAbs(before.x, 1e-3f));
        CHECK_THAT(after.y, WithinAbs(before.y, 1e-3f));
        REQUIRE(dragHandle(a, LineShape::Start, {0, 0}));
        CHECK(l.p0 == Pt{0, 0});
    }
    SECTION("命中：曲線上有，遠處沒有") {
        CHECK(hitTest(a, {100, 50}, 2));
        CHECK_FALSE(hitTest(a, {100, 150}, 2));
    }
    SECTION("範圍包含曲線中點") { CHECK(bounds(a).top == 50); }
}

TEST_CASE("塗鴉：原始筆跡依外框縮放，線寬隨之改變由繪圖端處理") {
    std::vector<Pt> path{{10, 10}, {20, 30}, {50, 10}};
    auto a = makeFreehand(path, style());
    FreehandShape& f = shapeOf<FreehandShape>(a);
    CHECK(f.rect.left == 10);
    CHECK(f.rect.top == 10);
    CHECK(f.origW == 40);
    CHECK(f.origH == 20);
    CHECK(handles(a).size() == 4);
    auto pts = freehandPoints(f);
    REQUIRE(pts.size() == 3);
    CHECK(pts[1] == Pt{20, 30});
    CHECK(paintedStrokeWidth(a) == a.stroke.width);
    REQUIRE(dragHandle(a, H_BR, {90, 50})); // 寬 80、高 40，各放大 2 倍
    pts = freehandPoints(f);
    CHECK_THAT(pts[1].x, WithinAbs(30.0f, 1e-4f));
    CHECK_THAT(pts[1].y, WithinAbs(50.0f, 1e-4f));
    CHECK(hitTest(a, {30, 50}, 1));
    CHECK_FALSE(hitTest(a, {60, 45}, 1));
    CHECK(paintedStrokeWidth(a) == a.stroke.width * 2);
    CHECK_FALSE(dragHandle(a, H_T, {0, 0})); // 只有四個角
    SECTION("外框不會被拖成零") {
        REQUIRE(dragHandle(a, H_BR, {10, 10}));
        CHECK(f.rect.width() == 1);
        CHECK(f.rect.height() == 1);
    }
}

TEST_CASE("塗鴉只有一個點也能建立") {
    auto a = makeFreehand({{5, 5}}, style());
    CHECK(shapeOf<FreehandShape>(a).origW == 1);
    CHECK(hitTest(a, {5, 5}, 1));
}

TEST_CASE("B-18：塗鴉拉大後，畫得到的線寬範圍都點得到") {
    StrokeStyle s;
    s.width = 18;
    auto a = makeFreehand({{100, 100}, {200, 100}}, s);
    REQUIRE(dragHandle(a, H_BR, {500, 105})); // 橫向 4 倍、縱向 5 倍：畫出來的線上下各 45 像素
    CHECK(paintedStrokeWidth(a) == 18 * 5);
    CHECK(hitTest(a, {300, 130}, 2));
    CHECK(hitTest(a, {300, 144}, 2));
    CHECK_FALSE(hitTest(a, {300, 150}, 2));
    // 線的兩端是橢圓的筆尖：橫向多出 9 × 4 = 36 像素
    CHECK(hitTest(a, {530, 100}, 2));
    CHECK_FALSE(hitTest(a, {545, 100}, 2));
    // 沒拉大時和原本一樣：線寬一半加容差
    auto b = makeFreehand({{100, 100}, {200, 100}}, s);
    CHECK(hitTest(b, {150, 110}, 2));
    CHECK_FALSE(hitTest(b, {150, 112}, 2));
}

TEST_CASE("星形：角數上下限與方向") {
    auto a = makeShape(Kind::Star, {0, 0}, 200, style());
    StarShape& star = shapeOf<StarShape>(a);
    CHECK(star.pointCount == 5);
    CHECK(starPoints(star).size() == 10);
    CHECK_THAT(starPoints(star)[0].y, WithinAbs(star.rect.top, 1e-3f)); // 頂點朝上
    CHECK(handles(a).size() == 10);
    // 把外側綠點順時針多拖一格，角數 +1。
    Pt c = star.rect.center();
    float r = star.rect.width() / 2;
    const float step = 2 * kPi / (kMaxPoints - kMinPoints + 1);
    float ang = -kPi / 2 + (5 - kMinPoints + 1) * step;
    REQUIRE(dragHandle(a, StarShape::Count, {c.x + r * std::cos(ang), c.y + r * std::sin(ang)}));
    CHECK(star.pointCount == 6);
    REQUIRE(dragHandle(a, StarShape::Count, {c.x, c.y - r}));
    CHECK(star.pointCount == 3);
    // 從最大的角數再往前拖過頂點：繞一圈回來不會跳到 3。
    star.pointCount = 20;
    REQUIRE(dragHandle(a, StarShape::Count, {c.x - 1, c.y - r}));
    CHECK(star.pointCount == 20);
    SECTION("內半徑拖到外框變成多邊形點集") {
        Pt outer = handles(a)[StarShape::Count].pos; // 外側綠點，內側綠點沿中心到它的方向移動
        REQUIRE(dragHandle(a, StarShape::Inner, c + (outer - c) * 2.0f));
        CHECK(star.innerRatio == 1.0f);
        REQUIRE(dragHandle(a, StarShape::Inner, c));
        CHECK(star.innerRatio == kMinInnerRatio);
    }
    SECTION("命中：尖端裡面有，兩個尖端之間的凹處沒有") {
        star.pointCount = 5;
        CHECK(hitTest(a, {0, 0}, 1));
        CHECK(hitTest(a, {0, star.rect.top + 4}, 1));
        CHECK_FALSE(hitTest(a, {star.rect.left + 2, star.rect.top + 2}, 1));
    }
}

TEST_CASE("正多邊形：六邊形預設，頂點朝上") {
    auto a = makeShape(Kind::Polygon, {0, 0}, 200, style());
    PolygonShape& poly = shapeOf<PolygonShape>(a);
    CHECK(poly.pointCount == 6);
    auto pts = starPoints(poly);
    REQUIRE(pts.size() == 6);
    CHECK_THAT(pts[0].x, WithinAbs(0.0f, 1e-3f));
    CHECK_THAT(pts[0].y, WithinAbs(poly.rect.top, 1e-3f));
    CHECK(handles(a).size() == 9);
    CHECK(hitTest(a, {0, 0}, 1));
    CHECK_FALSE(hitTest(a, {poly.rect.left + 1, poly.rect.top + 1}, 1)); // 外框的角落在六邊形外
    // 邊數綠點在半徑一半的圓上：拖到正上方是最少的邊數
    Pt c = poly.rect.center();
    REQUIRE(dragHandle(a, PolygonShape::Count, {c.x, c.y - 10}));
    CHECK(poly.pointCount == kMinPoints);
    CHECK_FALSE(dragHandle(a, 9, {0, 0})); // 多邊形沒有內圈綠點
}

TEST_CASE("B-17：整圖旋轉後，星形與多邊形的每個頂點都跟著轉") {
    const int imageW = 400;
    auto sameSet = [](const std::vector<Pt>& got, const std::vector<Pt>& want) {
        if (got.size() != want.size()) return false;
        for (Pt w : want) {
            bool found = false;
            for (Pt g : got) found = found || dist(g, w) < 1e-2f;
            if (!found) return false;
        }
        return true;
    };
    auto points = [](const Annotation& a) {
        if (const auto* s = a.as<StarShape>()) return starPoints(*s);
        return starPoints(*a.as<PolygonShape>());
    };
    for (Kind kind : {Kind::Polygon, Kind::Star}) {
        for (int count : {3, 5}) {
            auto a = makeShape(kind, {100, 150}, 400, style());
            *rectOf(a) = {60, 100, 180, 260}; // 不是正方形：轉過去外框的寬高要對調
            if (auto* s = a.as<StarShape>()) s->pointCount = count;
            if (auto* p = a.as<PolygonShape>()) p->pointCount = count;
            const auto original = points(a);
            auto want = original;
            int width = imageW, height = 300;
            for (int turn = 1; turn <= 4; ++turn) {
                for (Pt& p : want) p = Pt{p.y, width - p.x};
                rotateCCW90(a, width);
                std::swap(width, height);
                INFO("kind " << static_cast<int>(kind) << " count " << count << " turn " << turn);
                CHECK(sameSet(points(a), want));
            }
            CHECK(sameSet(points(a), original)); // 轉四次回到原樣
        }
    }
    // 三角形轉一次：尖端從朝上變成朝左
    auto tri = makeShape(Kind::Polygon, {100, 100}, 200, style());
    PolygonShape& p = shapeOf<PolygonShape>(tri);
    p.pointCount = 3;
    rotateCCW90(tri, 400);
    CHECK_THAT(starPoints(p)[0].x, WithinAbs(p.rect.left, 1e-3f));
    CHECK(p.turns == 1);
}

TEST_CASE("對話框：尾巴根部角度與尖端") {
    auto a = makeShape(Kind::Bubble, {100, 100}, 200, style());
    BubbleShape& b = shapeOf<BubbleShape>(a);
    REQUIRE(handles(a).size() == 10);
    REQUIRE(dragHandle(a, BubbleShape::TailRoot, {b.rect.right + 50, 100})); // 根部拖到右邊
    CHECK_THAT(b.tailAngle, WithinAbs(0.0f, 1e-3f));
    REQUIRE(dragHandle(a, BubbleShape::TailTip, {500, 500}));
    CHECK(b.tailTip == Pt{500, 500});
    CHECK(bounds(a).right == 500);
    CHECK(hitTest(a, {100, 100}, 1));
    CHECK(hitTest(a, {499, 499}, 2));
    CHECK_FALSE(hitTest(a, {b.rect.left + 1, b.rect.top + 1}, 1)); // 橢圓外的角落
    Pt ra, rb, tip;
    bubbleTail(b, ra, rb, tip);
    CHECK(tip == b.tailTip);
    CHECK(ra.x > b.rect.center().x);
    SECTION("輪廓：繞橢圓一圈（跳過尾巴根部那一段），最後一點是尖端") {
        auto pts = bubbleOutline(b);
        REQUIRE(pts.size() > 10);
        CHECK(pts.back() == b.tailTip);
        CHECK(dist(pts.front(), ra) < 1e-3f);
        CHECK(dist(pts[pts.size() - 2], rb) < 1e-3f);
        Pt c = b.rect.center();
        for (size_t i = 0; i + 1 < pts.size(); ++i) { // 都在橢圓上
            float dx = (pts[i].x - c.x) / (b.rect.width() / 2), dy = (pts[i].y - c.y) / (b.rect.height() / 2);
            CHECK_THAT(dx * dx + dy * dy, WithinAbs(1.0f, 1e-3f));
        }
    }
    SECTION("外框的八個點照樣能拖") {
        REQUIRE(dragHandle(a, H_TL, {0, 0}));
        CHECK(b.rect.left == 0);
        CHECK_FALSE(dragHandle(a, 10, {0, 0}));
    }
}

TEST_CASE("放大鏡：藍點改半徑，綠點改倍率") {
    auto a = makeShape(Kind::Magnifier, {100, 100}, 200, style());
    MagnifierShape& m = shapeOf<MagnifierShape>(a);
    REQUIRE(dragHandle(a, MagnifierShape::Radius, {160, 100}));
    CHECK_THAT(m.radius, WithinAbs(60.0f, 1e-4f));
    REQUIRE(dragHandle(a, MagnifierShape::Zoom, {100 + std::cos(-0.75f * kPi) * 60, 100 + std::sin(-0.75f * kPi) * 60}));
    CHECK_THAT(m.zoom, WithinAbs(kMinZoom, 1e-3f));
    REQUIRE(dragHandle(a, MagnifierShape::Zoom, {100 + std::cos(0.15f * kPi) * 60, 100 + std::sin(0.15f * kPi) * 60}));
    CHECK_THAT(m.zoom, WithinAbs(kMaxZoom, 1e-3f));
    CHECK(hitTest(a, {100, 100}, 0));
    CHECK_FALSE(hitTest(a, {200, 200}, 0));
    REQUIRE(dragHandle(a, MagnifierShape::Radius, {101, 100})); // 半徑有下限
    CHECK(m.radius == 8);
    CHECK_FALSE(dragHandle(a, 2, {0, 0}));
}

TEST_CASE("馬賽克：綠點沿上邊調格子大小") {
    auto a = makeShape(Kind::Mosaic, {100, 100}, 200, style());
    MosaicShape& m = shapeOf<MosaicShape>(a);
    REQUIRE(dragHandle(a, MosaicShape::Block, {m.rect.left, m.rect.top}));
    CHECK(m.blockSize == kMinBlock);
    REQUIRE(dragHandle(a, MosaicShape::Block, {m.rect.right + 100, m.rect.top}));
    CHECK(m.blockSize == kMaxBlock);
    CHECK(hitTest(a, m.rect.center(), 0));
    CHECK_FALSE(hitTest(a, {m.rect.right + 20, m.rect.top}, 1));
    REQUIRE(dragHandle(a, MosaicShape::Corner, {m.rect.left + 2, m.rect.top + 2}));
    CHECK(m.rect.width() == 8);
    CHECK_FALSE(dragHandle(a, 2, {0, 0}));
}

TEST_CASE("文字：只有左右兩個控制點，預設無外框") {
    TextStyle ts;
    auto a = makeText({100, 100}, 200, style(), ts);
    TextShape& t = shapeOf<TextShape>(a);
    CHECK(t.value == L"Text"); // 沒有語系時是英文 key
    CHECK(a.stroke.stroke.none());
    auto h = handles(a);
    REQUIRE(h.size() == 2);
    CHECK(h[0].id == H_L);
    CHECK(h[1].id == H_R);
    CHECK(hitTest(a, t.rect.center(), 0));
    CHECK(hitTest(a, {t.rect.left - 2, t.rect.top}, 3));       // 只看容差
    CHECK_FALSE(hitTest(a, {t.rect.left - 2, t.rect.top}, 1)); // 線寬不算
    REQUIRE(dragHandle(a, H_R, {t.rect.left + 5, 0})); // 不能窄於 20
    CHECK(t.rect.width() == 20);
    REQUIRE(dragHandle(a, H_L, {t.rect.right - 5, 0}));
    CHECK(t.rect.width() == 20);
    CHECK_FALSE(dragHandle(a, H_T, {0, 0}));
}

TEST_CASE("命中：矩形類看外框加線寬，橢圓看橢圓") {
    StrokeStyle s;
    s.width = 8;
    for (Kind k : {Kind::Rect, Kind::RoundRect, Kind::Mosaic}) {
        auto a = makeShape(k, {100, 100}, 200, s); // 外框 70..130
        CHECK(hitTest(a, {100, 100}, 0));
        CHECK(hitTest(a, {133, 100}, 0));       // 線寬的一半以內
        CHECK_FALSE(hitTest(a, {136, 100}, 0)); // 超過
        CHECK(hitTest(a, {136, 100}, 3));       // 容差補上
    }
    auto e = makeShape(Kind::Ellipse, {100, 100}, 200, s);
    CHECK(hitTest(e, {100, 100}, 0));
    CHECK(hitTest(e, {133, 100}, 0));
    CHECK_FALSE(hitTest(e, {128, 128}, 0)); // 外框的角落不在橢圓裡
}

TEST_CASE("moveBy：每一種物件整個一起搬") {
    for (Kind k : kAllKinds) {
        auto a = k == Kind::Freehand ? makeFreehand({{80, 90}, {120, 110}}, style()) : makeShape(k, {100, 100}, 200, style());
        const auto before = landmarks(a);
        moveBy(a, 10, 20);
        const auto after = landmarks(a);
        REQUIRE(after.size() == before.size());
        INFO("kind " << static_cast<int>(k));
        for (size_t i = 0; i < before.size(); ++i) CHECK(dist(after[i], before[i] + Pt{10, 20}) < 1e-3f);
    }
    auto b = makeShape(Kind::Bubble, {0, 0}, 100, style());
    Pt tip = shapeOf<BubbleShape>(b).tailTip;
    moveBy(b, 10, 20);
    CHECK(shapeOf<BubbleShape>(b).tailTip == tip + Pt{10, 20});
}

TEST_CASE("rotateCCW90 把矩形轉到新座標") {
    for (Kind k : {Kind::Rect, Kind::RoundRect, Kind::Ellipse, Kind::Mosaic, Kind::Text, Kind::Star, Kind::Polygon}) {
        Annotation a = makeShape(k, {0, 0}, 100, style());
        *rectOf(a) = {10, 20, 30, 50}; // 圖寬 100
        rotateCCW90(a, 100);
        // (x, y) -> (y, 100 - x)：左上 (10,20)->(20,90)，右下 (30,50)->(50,70)
        CHECK(*rectOf(a) == Rc{20, 70, 50, 90});
    }
}

TEST_CASE("rotateCCW90：直線、對話框、放大鏡的每個點都轉過去") {
    auto l = makeShape(Kind::Line, {0, 0}, 100, style());
    shapeOf<LineShape>(l) = {{10, 20}, {30, 50}, {25, 30}};
    rotateCCW90(l, 100);
    CHECK(shapeOf<LineShape>(l).p0 == Pt{20, 90});
    CHECK(shapeOf<LineShape>(l).p1 == Pt{50, 70});
    CHECK(shapeOf<LineShape>(l).mid == Pt{30, 75});

    auto b = makeShape(Kind::Bubble, {50, 50}, 100, style());
    BubbleShape& bubble = shapeOf<BubbleShape>(b);
    bubble.tailTip = {10, 90};
    Pt ra, rb, tip;
    bubbleTail(bubble, ra, rb, tip);
    rotateCCW90(b, 100);
    Pt ra2, rb2, tip2;
    bubbleTail(bubble, ra2, rb2, tip2);
    CHECK(tip2 == Pt{90, 90});
    CHECK(dist(ra2, Pt{ra.y, 100 - ra.x}) < 1e-3f); // 尾巴根部跟著轉
    CHECK(dist(rb2, Pt{rb.y, 100 - rb.x}) < 1e-3f);

    auto m = makeShape(Kind::Magnifier, {30, 40}, 100, style());
    float radius = shapeOf<MagnifierShape>(m).radius;
    rotateCCW90(m, 100);
    CHECK(shapeOf<MagnifierShape>(m).center == Pt{40, 70});
    CHECK(shapeOf<MagnifierShape>(m).radius == radius);
}

TEST_CASE("rotateCCW90 塗鴉筆跡跟著轉") {
    auto a = makeFreehand({{0, 0}, {40, 0}}, style()); // 水平線，origW 40，origH 1
    rotateCCW90(a, 100);
    FreehandShape& f = shapeOf<FreehandShape>(a);
    CHECK(f.origW == 1);
    CHECK(f.origH == 40);
    auto pts = freehandPoints(f);
    CHECK_THAT(pts[0].x, WithinAbs(pts[1].x, 1e-4f)); // 變垂直線
}

TEST_CASE("arrowHead 三角形指向尖端") {
    auto tri = arrowHead({100, 0}, {1, 0}, 2);
    REQUIRE(tri.size() == 3);
    CHECK(tri[0] == Pt{100, 0});
    CHECK(tri[1].x < 100);
    CHECK(tri[2].x < 100);
    CHECK_THAT(tri[1].y, WithinAbs(-tri[2].y, 1e-4f));
    // 方向是零向量時當成朝右，不會除以零
    auto flat = arrowHead({100, 0}, {0, 0}, 2);
    CHECK(flat[1].x < 100);
}

TEST_CASE("scaleBy：每一種物件的座標都照比例縮放") {
    for (Kind k : kAllKinds) {
        auto a = k == Kind::Freehand ? makeFreehand({{80, 90}, {120, 110}}, style()) : makeShape(k, {100, 100}, 200, style());
        const Rc before = bounds(a);
        scaleBy(a, 2, 2);
        INFO("kind " << static_cast<int>(k));
        const Rc after = bounds(a);
        CHECK_THAT(after.left, WithinAbs(before.left * 2, 1e-3f));
        CHECK_THAT(after.top, WithinAbs(before.top * 2, 1e-3f));
        CHECK_THAT(after.right, WithinAbs(before.right * 2, 1e-3f));
        CHECK_THAT(after.bottom, WithinAbs(before.bottom * 2, 1e-3f));
        CHECK(a.stroke.width == StrokeStyle{}.width); // 線寬的數值不變
    }
    auto a = makeShape(Kind::Magnifier, {100, 50}, 200, style());
    MagnifierShape& m = shapeOf<MagnifierShape>(a);
    float r = m.radius;
    scaleBy(a, 2, 0.5f);
    CHECK(m.center == Pt{200, 25});
    CHECK(m.radius == r * 0.5f); // 不等比時半徑跟著小的那一邊
    auto b = makeShape(Kind::Rect, {0, 0}, 100, style());
    *rectOf(b) = {10, 10, 20, 20};
    scaleBy(b, 2, 3);
    CHECK(*rectOf(b) == Rc{20, 30, 40, 60});
    auto l = makeShape(Kind::Line, {0, 0}, 100, style());
    shapeOf<LineShape>(l) = {{10, 20}, {30, 50}, {25, 30}};
    scaleBy(l, 2, 3);
    CHECK(shapeOf<LineShape>(l).mid == Pt{50, 90});
    auto bub = makeShape(Kind::Bubble, {0, 0}, 100, style());
    shapeOf<BubbleShape>(bub).tailTip = {10, 20};
    scaleBy(bub, 2, 3);
    CHECK(shapeOf<BubbleShape>(bub).tailTip == Pt{20, 60});
    // 塗鴉：外框變大，畫出來的線寬跟著變
    auto f = makeFreehand({{0, 0}, {40, 20}}, style());
    scaleBy(f, 3, 3);
    CHECK(paintedStrokeWidth(f) == f.stroke.width * 3);
}

TEST_CASE("handleGuide：綠點的軌跡提示") {
    // 藍點沒有
    auto r = makeShape(Kind::Rect, {100, 100}, 200, style());
    CHECK(handleGuide(r, H_TL).kind == GuideKind::None);
    // 直線的綠點自由移動，沒有
    auto l = makeShape(Kind::Line, {100, 100}, 200, style());
    CHECK(handleGuide(l, LineShape::Bend).kind == GuideKind::None);
    // 多邊形：半徑一半的圓
    auto p = makeShape(Kind::Polygon, {100, 100}, 200, style());
    auto g = handleGuide(p, PolygonShape::Count);
    REQUIRE(g.kind == GuideKind::Circle);
    CHECK_THAT(g.ellipse.width(), WithinAbs(rectOf(p)->width() * 0.5f, 1e-3f));
    CHECK(g.ellipse.center() == rectOf(p)->center());
    // 星形：角數綠點繞外圓，內縮綠點是中心到外側綠點的線
    auto s = makeShape(Kind::Star, {100, 100}, 200, style());
    CHECK(handleGuide(s, StarShape::Count).kind == GuideKind::Circle);
    auto line = handleGuide(s, StarShape::Inner);
    REQUIRE(line.kind == GuideKind::Line);
    CHECK(line.a == rectOf(s)->center());
    Pt outer;
    for (const auto& h : handles(s)) if (h.id == StarShape::Count) outer = h.pos;
    CHECK_THAT(dist(line.b, outer), WithinAbs(0.0f, 1e-3f));
    // 放大鏡倍率繞圓周
    auto m = makeShape(Kind::Magnifier, {100, 100}, 200, style());
    auto mg = handleGuide(m, MagnifierShape::Zoom);
    REQUIRE(mg.kind == GuideKind::Circle);
    CHECK_THAT(mg.ellipse.width(), WithinAbs(shapeOf<MagnifierShape>(m).radius * 2, 1e-3f));
    CHECK(handleGuide(m, MagnifierShape::Radius).kind == GuideKind::None);
    // 馬賽克沿上邊
    auto mo = makeShape(Kind::Mosaic, {100, 100}, 200, style());
    auto ml = handleGuide(mo, MosaicShape::Block);
    REQUIRE(ml.kind == GuideKind::Line);
    CHECK(ml.a.y == rectOf(mo)->top);
    CHECK(ml.b.x == rectOf(mo)->right);
    CHECK(handleGuide(mo, MosaicShape::Corner).kind == GuideKind::None);
    // 對話框尾巴根部繞橢圓
    auto b = makeShape(Kind::Bubble, {100, 100}, 200, style());
    CHECK(handleGuide(b, BubbleShape::TailRoot).kind == GuideKind::Circle);
    CHECK(handleGuide(b, BubbleShape::TailTip).kind == GuideKind::None);
    // 塗鴉與文字沒有綠點
    CHECK(handleGuide(makeFreehand({{0, 0}, {5, 5}}, style()), H_TL).kind == GuideKind::None);
    CHECK(handleGuide(makeText({0, 0}, 100, style(), TextStyle{}), H_L).kind == GuideKind::None);
}
