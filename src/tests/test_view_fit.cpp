#include <catch2/catch_test_macros.hpp>

#include "core/ViewFit.h"

using namespace mp;

TEST_CASE("fitZoom：各檢視模式的倍率") {
    // 畫布 1000×500，畫面 2000×2000
    CHECK(fitZoom(ViewMode::FitWindow, 1000, 500, 2000, 2000, false) == 0.25f);
    CHECK(fitZoom(ViewMode::TwoPage, 1000, 500, 2000, 2000, false) == 0.25f);
    CHECK(fitZoom(ViewMode::FitWidth, 1000, 500, 2000, 2000, false) == 0.5f);
    CHECK(fitZoom(ViewMode::FitHeight, 1000, 500, 2000, 2000, false) == 0.25f);
    CHECK(fitZoom(ViewMode::Actual, 1000, 500, 2000, 2000, false) == 1.0f);
}

TEST_CASE("fitZoom：小圖放大只在符合類模式、開了選項時；最小 0.01") {
    CHECK(fitZoom(ViewMode::FitWindow, 1000, 500, 100, 100, false) == 1.0f);
    CHECK(fitZoom(ViewMode::FitWindow, 1000, 500, 100, 100, true) == 5.0f);
    CHECK(fitZoom(ViewMode::FitWidth, 1000, 500, 100, 100, true) == 10.0f);
    CHECK(fitZoom(ViewMode::Actual, 1000, 500, 100, 100, true) == 1.0f);
    CHECK(fitZoom(ViewMode::FitWindow, 10, 10, 100000, 100000, false) == 0.01f);
}

TEST_CASE("hqTargetSize：倍率 1 不縮；四捨五入；超過邊長上限或 4000 萬像素不縮") {
    int tw = 0, th = 0;
    CHECK_FALSE(hqTargetSize(3000, 6000, 1.0f, 16384, tw, th));
    REQUIRE(hqTargetSize(3000, 6000, 0.5f, 16384, tw, th));
    CHECK(tw == 1500);
    CHECK(th == 3000);
    REQUIRE(hqTargetSize(333, 3, 0.5f, 16384, tw, th));
    CHECK(tw == 167);
    CHECK(th == 2);
    CHECK_FALSE(hqTargetSize(3000, 6000, 0.5f, 2000, tw, th));
    CHECK_FALSE(hqTargetSize(4000, 4000, 2.0f, 16384, tw, th)); // 8000×8000 = 6400 萬
    REQUIRE(hqTargetSize(1, 1, 0.01f, 16384, tw, th));
    CHECK(tw == 1);
    CHECK(th == 1);
}

TEST_CASE("screenHqTargets：一屏各頁的縮圖尺寸，和 spreadSize → fitZoom → hqTargetSize 逐步算的一樣") {
    const unsigned maxSide = 16384;
    // 單頁 2000×3000 放進 1000×1000 的畫布（符合視窗）：倍率 1/3
    auto one = screenHqTargets(ViewMode::FitWindow, 1000, 1000, false, maxSide, {{2000, 3000}});
    REQUIRE(one.size() == 1);
    CHECK(one[0].first == 667);
    CHECK(one[0].second == 1000);
    // 兩頁並排：寬相加、高取大的，兩頁用同一個倍率
    auto two = screenHqTargets(ViewMode::FitWindow, 1000, 1000, false, maxSide, {{1000, 1500}, {1000, 1000}});
    REQUIRE(two.size() == 2);
    const float z = fitZoom(ViewMode::FitWindow, 1000, 1000, 2000, 1500, false);
    int tw = 0, th = 0;
    REQUIRE(hqTargetSize(1000, 1500, z, maxSide, tw, th));
    CHECK(two[0] == std::pair<int, int>{tw, th});
    REQUIRE(hqTargetSize(1000, 1000, z, maxSide, tw, th));
    CHECK(two[1] == std::pair<int, int>{tw, th});
    CHECK(two[0].first == 500);
    // 實際大小：倍率 1，不縮
    auto actual = screenHqTargets(ViewMode::Actual, 1000, 1000, false, maxSide, {{2000, 3000}});
    CHECK(actual[0] == std::pair<int, int>{0, 0});
    // 小圖不放大：倍率 1，不縮；開了「小圖放大」就放大
    CHECK(screenHqTargets(ViewMode::FitWindow, 1000, 1000, false, maxSide, {{100, 100}})[0] == std::pair<int, int>{0, 0});
    CHECK(screenHqTargets(ViewMode::FitWindow, 1000, 1000, true, maxSide, {{100, 100}})[0] == std::pair<int, int>{1000, 1000});
    CHECK(screenHqTargets(ViewMode::FitWindow, 1000, 1000, false, maxSide, {}).empty());
}
