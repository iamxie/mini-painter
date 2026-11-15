// core/Viewport：畫布與圖片的座標換算、位移夾限、縮放錨點、兩頁並排的版面。
#include <catch2/catch_test_macros.hpp>

#include <cmath>

#include "core/Viewport.h"

using namespace mp;

namespace {
bool near(float a, float b, float tol = 1e-3f) { return std::abs(a - b) <= tol; }
bool samePt(Pt a, Pt b) { return near(a.x, b.x) && near(a.y, b.y); }
bool sameRc(Rc a, Rc b) { return near(a.left, b.left) && near(a.top, b.top) && near(a.right, b.right) && near(a.bottom, b.bottom); }
} // namespace

TEST_CASE("canvasToImage / imageToCanvas：互為反函數") {
    Viewport v{2.0f, {30, -40}};
    CHECK(samePt(canvasToImage(v, {30, -40}), {0, 0}));    // 圖片原點
    CHECK(samePt(canvasToImage(v, {130, 60}), {50, 50}));
    CHECK(samePt(imageToCanvas(v, {50, 50}), {130, 60}));
    for (Pt p : {Pt{0, 0}, Pt{12.5f, 7.25f}, Pt{-3, 400}}) CHECK(samePt(canvasToImage(v, imageToCanvas(v, p)), p));
    Viewport one; // 預設：1 倍、沒有位移
    CHECK(samePt(canvasToImage(one, {7, 9}), {7, 9}));
}

TEST_CASE("clampOffset：比畫布小的方向置中，比畫布大的方向不露出空白，結果是整數") {
    // 圖 100×50、1 倍，畫布 300×200：兩個方向都置中
    CHECK(samePt(clampOffset({-999, 999}, 1.0f, 100, 50, 300, 200), {100, 75}));
    // 4 倍變成 400×200：寬超過畫布，高剛好
    CHECK(samePt(clampOffset({50, 30}, 4.0f, 100, 50, 300, 200), {0, 0}));       // 往右拖過頭：左邊不能露白
    CHECK(samePt(clampOffset({-500, 0}, 4.0f, 100, 50, 300, 200), {-100, 0}));   // 往左拖過頭：右邊不能露白
    CHECK(samePt(clampOffset({-40, 0}, 4.0f, 100, 50, 300, 200), {-40, 0}));     // 範圍內不動
    // 高超過畫布
    CHECK(samePt(clampOffset({0, -1e9f}, 1.0f, 100, 500, 300, 200), {100, -300})); // 捲到底
    CHECK(samePt(clampOffset({0, 1e9f}, 1.0f, 100, 500, 300, 200), {100, 0}));     // 捲到頂
    // 對齊整數像素
    Pt p = clampOffset({-10.4f, -20.6f}, 3.0f, 200, 200, 300, 200);
    CHECK(p.x == std::round(p.x));
    CHECK(p.y == std::round(p.y));
    CHECK(samePt(p, {-10, -21}));
    Pt centered = clampOffset({0, 0}, 1.0f, 101, 51, 300, 200); // 置中算出 .5 也要取整
    CHECK(centered.x == std::round(centered.x));
    CHECK(centered.y == std::round(centered.y));
}

TEST_CASE("zoomAbout：錨點底下的圖片點不動，倍率夾在範圍內") {
    Viewport v{1.0f, {10, 20}};
    Pt anchor{110, 70};
    Pt before = canvasToImage(v, anchor);
    Viewport z = zoomAbout(v, 2.5f, anchor);
    CHECK(near(z.zoom, 2.5f));
    CHECK(samePt(canvasToImage(z, anchor), before));
    CHECK(samePt(imageToCanvas(z, before), anchor));
    // 縮小也一樣
    Viewport s = zoomAbout(z, 0.4f, anchor);
    CHECK(samePt(canvasToImage(s, anchor), before));
    // 夾範圍
    CHECK(near(zoomAbout(v, 1000.0f, anchor).zoom, kViewMaxZoom));
    CHECK(near(zoomAbout(v, 0.0001f, anchor).zoom, kViewMinZoom));
    CHECK(samePt(canvasToImage(zoomAbout(v, 1000.0f, anchor), anchor), before));
    // 倍率沒變：檢視不變
    Viewport same = zoomAbout(v, 1.0f, anchor);
    CHECK(samePt(same.offset, v.offset));
}

TEST_CASE("spreadSize / pageRectInSpread：單頁、兩頁、左右順序、底對齊") {
    PageSize a{600, 800}, b{500, 700};
    // 單頁
    CHECK(samePt(spreadSize(a, nullptr), {600, 800}));
    CHECK(sameRc(pageRectInSpread(a, nullptr, false, false), {0, 0, 600, 800}));
    CHECK(sameRc(pageRectInSpread(a, nullptr, false, true), {0, 0, 600, 800}));
    CHECK(sameRc(pageRectInSpread(a, nullptr, true, false), {0, 0, 0, 0})); // 沒有第二頁
    // 兩頁：寬相加、高取大的
    CHECK(samePt(spreadSize(a, &b), {1100, 800}));
    CHECK(samePt(spreadSize(b, &a), {1100, 800}));
    // 左到右：第一頁在左；比較矮的那頁往下靠（底對齊）
    CHECK(sameRc(pageRectInSpread(a, &b, false, false), {0, 0, 600, 800}));
    CHECK(sameRc(pageRectInSpread(a, &b, true, false), {600, 100, 1100, 800}));
    // 右到左（日漫）：第一頁在右
    CHECK(sameRc(pageRectInSpread(a, &b, false, true), {500, 0, 1100, 800}));
    CHECK(sameRc(pageRectInSpread(a, &b, true, true), {0, 100, 500, 800}));
    // 第一頁比較矮
    CHECK(sameRc(pageRectInSpread(b, &a, false, false), {0, 100, 500, 800}));
    CHECK(sameRc(pageRectInSpread(b, &a, true, false), {500, 0, 1100, 800}));
    // 兩頁不重疊、合起來剛好是整個畫面的寬
    for (bool rtl : {false, true}) {
        Rc first = pageRectInSpread(a, &b, false, rtl), second = pageRectInSpread(a, &b, true, rtl);
        CHECK(near(std::min(first.left, second.left), 0));
        CHECK(near(std::max(first.right, second.right), 1100));
        CHECK((near(first.right, second.left) || near(second.right, first.left)));
    }
}
