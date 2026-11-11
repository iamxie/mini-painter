#include <catch2/catch_test_macros.hpp>

#include "core/Crop.h"

using namespace mp;

namespace {
const CropBox kBox{100, 50, 300, 250}; // 200 x 200
constexpr float kTol = 8, kCorner = 20;
} // namespace

TEST_CASE("cropHitTest：角、邊、框內、框外") {
    CHECK(cropHitTest(kBox, {100, 50}, kTol, kCorner) == CropPart::TopLeft);
    CHECK(cropHitTest(kBox, {300, 50}, kTol, kCorner) == CropPart::TopRight);
    CHECK(cropHitTest(kBox, {100, 250}, kTol, kCorner) == CropPart::BottomLeft);
    CHECK(cropHitTest(kBox, {300, 250}, kTol, kCorner) == CropPart::BottomRight);
    CHECK(cropHitTest(kBox, {200, 50}, kTol, kCorner) == CropPart::Top);
    CHECK(cropHitTest(kBox, {200, 250}, kTol, kCorner) == CropPart::Bottom);
    CHECK(cropHitTest(kBox, {100, 150}, kTol, kCorner) == CropPart::Left);
    CHECK(cropHitTest(kBox, {300, 150}, kTol, kCorner) == CropPart::Right);
    CHECK(cropHitTest(kBox, {200, 150}, kTol, kCorner) == CropPart::Inside);
    CHECK(cropHitTest(kBox, {50, 150}, kTol, kCorner) == CropPart::None);
    CHECK(cropHitTest(kBox, {200, 400}, kTol, kCorner) == CropPart::None);
}

TEST_CASE("cropHitTest：容差在框外也算") {
    CHECK(cropHitTest(kBox, {93, 150}, kTol, kCorner) == CropPart::Left);
    CHECK(cropHitTest(kBox, {91, 150}, kTol, kCorner) == CropPart::None);
    CHECK(cropHitTest(kBox, {95, 45}, kTol, kCorner) == CropPart::TopLeft);
}

TEST_CASE("cropHitTest：L 形角沿邊延伸 corner 長，超過才算邊") {
    CHECK(cropHitTest(kBox, {118, 52}, kTol, kCorner) == CropPart::TopLeft);  // 沿上邊離角 18
    CHECK(cropHitTest(kBox, {125, 52}, kTol, kCorner) == CropPart::Top);      // 離角 25
    CHECK(cropHitTest(kBox, {298, 232}, kTol, kCorner) == CropPart::BottomRight); // 沿右邊離角 18
    CHECK(cropHitTest(kBox, {298, 220}, kTol, kCorner) == CropPart::Right);
}

TEST_CASE("cropHitTest：框很小時取最近的角") {
    CropBox tiny{100, 100, 110, 110};
    CHECK(cropHitTest(tiny, {101, 101}, kTol, kCorner) == CropPart::TopLeft);
    CHECK(cropHitTest(tiny, {109, 109}, kTol, kCorner) == CropPart::BottomRight);
    CHECK(cropHitTest(tiny, {109, 101}, kTol, kCorner) == CropPart::TopRight);
}

TEST_CASE("dragCropBox：拖角動兩個邊，四捨五入到整數") {
    CropBox b = dragCropBox(kBox, CropPart::TopLeft, 10.4f, 20.6f, 1000, 800);
    CHECK(b == CropBox{110, 71, 300, 250});
    b = dragCropBox(kBox, CropPart::BottomRight, -50, 30, 1000, 800);
    CHECK(b == CropBox{100, 50, 250, 280});
    b = dragCropBox(kBox, CropPart::TopRight, 5, -5, 1000, 800);
    CHECK(b == CropBox{100, 45, 305, 250});
    b = dragCropBox(kBox, CropPart::BottomLeft, -5, 5, 1000, 800);
    CHECK(b == CropBox{95, 50, 300, 255});
}

TEST_CASE("dragCropBox：拖邊只動那一邊") {
    CHECK(dragCropBox(kBox, CropPart::Left, 30, 99, 1000, 800) == CropBox{130, 50, 300, 250});
    CHECK(dragCropBox(kBox, CropPart::Right, -30, 99, 1000, 800) == CropBox{100, 50, 270, 250});
    CHECK(dragCropBox(kBox, CropPart::Top, 99, 30, 1000, 800) == CropBox{100, 80, 300, 250});
    CHECK(dragCropBox(kBox, CropPart::Bottom, 99, -30, 1000, 800) == CropBox{100, 50, 300, 220});
}

TEST_CASE("dragCropBox：不能拉出圖外") {
    CHECK(dragCropBox(kBox, CropPart::TopLeft, -500, -500, 1000, 800) == CropBox{0, 0, 300, 250});
    CHECK(dragCropBox(kBox, CropPart::BottomRight, 5000, 5000, 1000, 800) == CropBox{100, 50, 1000, 800});
}

TEST_CASE("dragCropBox：不越過對邊，寬高至少 1") {
    CHECK(dragCropBox(kBox, CropPart::Left, 1000, 0, 1000, 800) == CropBox{299, 50, 300, 250});
    CHECK(dragCropBox(kBox, CropPart::Right, -1000, 0, 1000, 800) == CropBox{100, 50, 101, 250});
    CHECK(dragCropBox(kBox, CropPart::Top, 0, 1000, 1000, 800) == CropBox{100, 249, 300, 250});
    CHECK(dragCropBox(kBox, CropPart::Bottom, 0, -1000, 1000, 800) == CropBox{100, 50, 300, 51});
}

TEST_CASE("dragCropBox：框內拖曳整個搬，大小不變，碰圖邊就停") {
    CHECK(dragCropBox(kBox, CropPart::Inside, 40, -20, 1000, 800) == CropBox{140, 30, 340, 230});
    CHECK(dragCropBox(kBox, CropPart::Inside, -500, -500, 1000, 800) == CropBox{0, 0, 200, 200});
    CHECK(dragCropBox(kBox, CropPart::Inside, 5000, 5000, 1000, 800) == CropBox{800, 600, 1000, 800});
}

TEST_CASE("dragCropBox：None 原樣") {
    CHECK(dragCropBox(kBox, CropPart::None, 40, 40, 1000, 800) == kBox);
}

TEST_CASE("cropAnnotations：平移到裁切後的座標") {
    StrokeStyle s;
    Annotation rect = makeShape(Kind::Rect, {200, 150}, 100, s);
    Rc before = rect.rect;
    Annotation line;
    line.kind = Kind::Line;
    line.p0 = {150, 100};
    line.p1 = {250, 200};
    line.mid = {200, 150};
    auto out = cropAnnotations({rect, line}, kBox);
    REQUIRE(out.size() == 2);
    CHECK(out[0].rect == before.offset(-100, -50));
    CHECK(out[1].p0 == Pt{50, 50});
    CHECK(out[1].p1 == Pt{150, 150});
    CHECK(out[1].mid == Pt{100, 100});
}

TEST_CASE("cropAnnotations：完全在框外的刪掉，跨邊界的留下") {
    Annotation inside, crossing, outside, nearEdge;
    for (Annotation* a : {&inside, &crossing, &outside, &nearEdge}) a->kind = Kind::Rect;
    inside.id = 1;
    inside.rect = {150, 100, 200, 150};
    crossing.id = 2;
    crossing.rect = {250, 200, 400, 400}; // 跨右下角
    outside.id = 3;
    outside.rect = {500, 500, 600, 600};
    nearEdge.id = 4;
    nearEdge.rect = {50, 100, 99, 150}; // 外框在框外 1 像素，但線寬碰得到
    auto out = cropAnnotations({inside, crossing, outside, nearEdge}, kBox);
    REQUIRE(out.size() == 3);
    CHECK(out[0].id == 1);
    CHECK(out[1].id == 2);
    CHECK(out[2].id == 4);
}

TEST_CASE("cropAnnotations：放大鏡用圓心與半徑判斷") {
    Annotation mag;
    mag.kind = Kind::Magnifier;
    mag.center = {320, 270}; // 圓心在框外，半徑伸進框內
    mag.radius = 50;
    auto out = cropAnnotations({mag}, kBox);
    REQUIRE(out.size() == 1);
    CHECK(out[0].center == Pt{220, 220});
    mag.center = {500, 500};
    CHECK(cropAnnotations({mag}, kBox).empty());
}
