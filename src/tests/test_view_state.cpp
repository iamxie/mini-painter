// core/ViewState：主視窗的檢視狀態（倍率、位移、跟不跟著檢視模式走、裁切模式存的檢視）。
#include <catch2/catch_test_macros.hpp>

#include <cmath>

#include "core/ViewState.h"

using namespace mp;

namespace {

bool near(float a, float b, float tol = 1e-3f) { return std::abs(a - b) <= tol; }
bool samePt(Pt a, Pt b) { return near(a.x, b.x) && near(a.y, b.y); }

// 畫布 400×300，圖 800×1200（直的大圖），符合視窗。
ViewFrame frame(ViewMode mode = ViewMode::FitWindow) {
    ViewFrame f;
    f.canvasW = 400;
    f.canvasH = 300;
    f.contentW = 800;
    f.contentH = 1200;
    f.mode = mode;
    return f;
}

} // namespace

TEST_CASE("ViewState：一開始是 1 倍、沒有位移、跟著檢視模式走") {
    ViewState v;
    CHECK(v.zoom() == 1.0f);
    CHECK(samePt(v.offset(), {0, 0}));
    CHECK(v.fitting());
    CHECK(v.atActualSize());
    CHECK_FALSE(v.cropFit());
    CHECK(v.viewport().zoom == 1.0f);
}

TEST_CASE("ViewState::fit：照檢視模式算倍率，放得下的方向置中") {
    ViewState v;
    v.fit(frame());
    CHECK(near(v.zoom(), 0.25f));            // 高 1200 放進 300
    CHECK(samePt(v.offset(), {100, 0}));     // 寬 200 在 400 裡置中
    CHECK(v.fitting());
    CHECK_FALSE(v.atActualSize());

    v.fit(frame(ViewMode::FitWidth));
    CHECK(near(v.zoom(), 0.5f));             // 寬 800 放進 400
    CHECK(samePt(v.offset(), {0, 0}));       // 高 600 超過畫布：從頂端開始

    v.fit(frame(ViewMode::Actual));
    CHECK(v.zoom() == 1.0f);
    CHECK(v.atActualSize());
    CHECK(samePt(v.offset(), {0, 0}));
}

TEST_CASE("ViewState::fit：小圖預設不放大，開了「小圖放大」才放大") {
    ViewFrame f = frame();
    f.contentW = 100;
    f.contentH = 50;
    ViewState v;
    v.fit(f);
    CHECK(v.zoom() == 1.0f);
    CHECK(samePt(v.offset(), {150, 125}));
    f.upscaleSmall = true;
    v.fit(f);
    CHECK(near(v.zoom(), 4.0f));
    CHECK(samePt(v.offset(), {0, 50}));
}

TEST_CASE("ViewState::fit(resetOffset = false)：留著原本的位移，只夾回範圍") {
    ViewState v;
    v.fit(frame(ViewMode::FitWidth)); // 0.5 倍，畫面 400×600
    REQUIRE(v.scrollBy(frame(ViewMode::FitWidth), 0, -200));
    CHECK(samePt(v.offset(), {0, -200}));
    // 畫布變高（視窗拉大）：位移留著
    ViewFrame taller = frame(ViewMode::FitWidth);
    taller.canvasH = 350;
    v.fit(taller, false);
    CHECK(near(v.zoom(), 0.5f));
    CHECK(samePt(v.offset(), {0, -200}));
    // 預設會歸零
    v.fit(taller);
    CHECK(samePt(v.offset(), {0, 0}));
}

TEST_CASE("ViewState::zoomTo：錨點底下的圖片點不動，之後不跟著模式走") {
    ViewState v;
    const ViewFrame f = frame(ViewMode::Actual);
    v.fit(f); // 1 倍，位移 0
    const Pt anchor{200, 150};
    const Pt before = canvasToImage(v.viewport(), anchor);
    v.zoomTo(f, 2.0f, anchor);
    CHECK(v.zoom() == 2.0f);
    CHECK_FALSE(v.fitting());
    CHECK(samePt(canvasToImage(v.viewport(), anchor), before));
    // 倍率夾在上下限
    v.zoomTo(f, 1e6f, anchor);
    CHECK(v.zoom() == kViewMaxZoom);
    v.zoomTo(f, 0.0f, anchor);
    CHECK(v.zoom() == kViewMinZoom);
    // 縮到比畫布小：置中（位移夾回範圍）
    CHECK(samePt(v.offset(), {std::round((400 - 800 * kViewMinZoom) / 2), std::round((300 - 1200 * kViewMinZoom) / 2)}));
}

TEST_CASE("ViewState::reflow：跟著模式走的重新算倍率，手動縮放過的只夾位移") {
    ViewFrame f = frame();
    ViewState v;
    v.fit(f);
    f.canvasH = 600; // 視窗變高
    v.reflow(f, false);
    CHECK(near(v.zoom(), 0.5f)); // 跟著變

    v.zoomTo(f, 1.0f, {0, 0});
    REQUIRE_FALSE(v.fitting());
    v.scrollBy(f, -400, -600); // 捲到右下角
    CHECK(samePt(v.offset(), {-400, -600}));
    f.canvasW = 600; // 畫布變寬：倍率不變，右邊不能露白
    v.reflow(f, true);
    CHECK(v.zoom() == 1.0f);
    CHECK(samePt(v.offset(), {-200, -600}));
}

TEST_CASE("ViewState::scrollBy：回報畫面有沒有動；到邊就不動") {
    ViewState v;
    const ViewFrame f = frame(ViewMode::FitWidth); // 畫面 400×600，畫布 400×300
    v.fit(f);
    CHECK_FALSE(v.scrollBy(f, 0, 50));   // 已經在頂端，往上捲不動
    CHECK(v.scrollBy(f, 0, -75));
    CHECK(samePt(v.offset(), {0, -75}));
    CHECK_FALSE(v.scrollBy(f, 30, 0));   // 寬剛好，橫向捲不動
    CHECK(v.scrollBy(f, 0, -1000));      // 超過就停在底
    CHECK(samePt(v.offset(), {0, -300}));
    CHECK_FALSE(v.scrollBy(f, 0, -75));  // 已經在底
    CHECK(v.fitting());                  // 捲動不算手動縮放
}

TEST_CASE("ViewState::panTo / scrollToBottom / clamp") {
    ViewState v;
    const ViewFrame f = frame(ViewMode::FitWidth);
    v.fit(f);
    v.panTo(f, {-50, -120});
    CHECK(samePt(v.offset(), {0, -120})); // 橫向放得下：不能拖
    v.panTo(f, {0, 500});
    CHECK(samePt(v.offset(), {0, 0}));
    v.scrollToBottom(f);
    CHECK(samePt(v.offset(), {0, -300}));
    // 畫布變大之後 clamp：底下不能露白
    ViewFrame bigger = f;
    bigger.canvasH = 500;
    v.clamp(bigger);
    CHECK(samePt(v.offset(), {0, -100}));
}

TEST_CASE("ViewState：沒有圖的時候什麼都不改") {
    ViewFrame none;
    none.canvasW = 400;
    none.canvasH = 300;
    REQUIRE_FALSE(none.hasContent());
    ViewState v;
    v.zoomTo(frame(ViewMode::Actual), 2.0f, {0, 0});
    const Viewport before = v.viewport();
    v.fit(none);
    v.zoomTo(none, 5.0f, {10, 10});
    CHECK_FALSE(v.scrollBy(none, 100, 100));
    v.panTo(none, {7, 7});
    v.scrollToBottom(none);
    v.clamp(none);
    CHECK(v.zoom() == before.zoom);
    CHECK(samePt(v.offset(), before.offset));
    CHECK_FALSE(v.fitting()); // fit 也沒有把它改回 true
}

TEST_CASE("ViewState 裁切：進入時整張圖放進畫布，取消回到原本的檢視") {
    ViewState v;
    ViewFrame f = frame(ViewMode::FitWidth);
    v.fit(f);
    v.zoomTo(f, 1.0f, {0, 0});
    v.scrollBy(f, -100, -200);
    const Viewport manual = v.viewport();
    REQUIRE_FALSE(v.fitting());

    f.cropping = true;
    v.enterCrop(f);
    CHECK(v.cropFit());
    CHECK(near(v.zoom(), 0.25f)); // 不管檢視模式（符合寬度是 0.5），整張放進畫布
    CHECK(samePt(v.offset(), {100, 0}));
    // 裁切中視窗改大小：還是整張放進畫布
    ViewFrame resized = f;
    resized.canvasH = 600;
    v.reflow(resized, false);
    CHECK(near(v.zoom(), 0.5f));
    CHECK(v.cropFit());

    f.cropping = false;
    v.leaveCrop(f);
    CHECK_FALSE(v.cropFit());
    CHECK_FALSE(v.fitting());
    CHECK(v.zoom() == manual.zoom);
    CHECK(samePt(v.offset(), manual.offset));
}

TEST_CASE("ViewState 裁切：進入前跟著模式走的，取消後照模式重新算（視窗可能改過大小）") {
    ViewState v;
    ViewFrame f = frame(ViewMode::FitWidth);
    v.fit(f);
    f.cropping = true;
    v.enterCrop(f);
    f.canvasW = 800; // 裁切中把視窗拉寬
    f.cropping = false;
    v.leaveCrop(f);
    CHECK(v.fitting());
    CHECK(near(v.zoom(), 1.0f)); // 符合寬度：800 放進 800
}

TEST_CASE("ViewState 裁切：手動縮放或 endCropFit 之後，「整張放進畫布」失效") {
    ViewFrame f = frame(ViewMode::FitWidth);
    f.cropping = true;
    ViewState v;
    v.fit(f);
    v.enterCrop(f);
    v.zoomTo(f, 2.0f, {0, 0});
    CHECK_FALSE(v.cropFit());
    v.fit(f); // 之後再 fit 就照檢視模式
    CHECK(near(v.zoom(), 0.5f));

    ViewState w;
    w.enterCrop(f);
    REQUIRE(near(w.zoom(), 0.25f));
    w.endCropFit(); // 改了檢視模式、裁切套用了
    w.fit(f);
    CHECK(near(w.zoom(), 0.5f));
}

TEST_CASE("ViewState：cropFit 只在裁切模式中有用（frame.cropping 是 false 時照檢視模式）") {
    ViewFrame f = frame(ViewMode::FitWidth);
    f.cropping = true;
    ViewState v;
    v.enterCrop(f);
    REQUIRE(v.cropFit());
    f.cropping = false; // 裁切模式從別的路離開了
    v.fit(f);
    CHECK(near(v.zoom(), 0.5f));
}
