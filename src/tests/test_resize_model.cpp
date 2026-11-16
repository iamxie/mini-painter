#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "core/ResizeModel.h"

using namespace mp;
using Catch::Matchers::WithinAbs;

// 「調整尺寸」對話框的算法。

TEST_CASE("pixelsToUnit / unitToPixels：每個單位互為反函數") {
    const double dpi = 300;
    const int total = 2000;
    for (LengthUnit u : {LengthUnit::Pixels, LengthUnit::Percent, LengthUnit::Inches, LengthUnit::Cm, LengthUnit::Mm, LengthUnit::Points}) {
        INFO("unit " << static_cast<int>(u));
        CHECK_THAT(unitToPixels(pixelsToUnit(1234, u, dpi, total), u, dpi, total), WithinAbs(1234.0, 1e-9));
    }
    CHECK(pixelsToUnit(500, LengthUnit::Pixels, dpi, total) == 500);
    CHECK(pixelsToUnit(500, LengthUnit::Percent, dpi, total) == 25);
    CHECK(pixelsToUnit(600, LengthUnit::Inches, dpi, total) == 2);
    CHECK_THAT(pixelsToUnit(600, LengthUnit::Cm, dpi, total), WithinAbs(5.08, 1e-9));
    CHECK_THAT(pixelsToUnit(600, LengthUnit::Mm, dpi, total), WithinAbs(50.8, 1e-9));
    CHECK(pixelsToUnit(600, LengthUnit::Points, dpi, total) == 144);
}

TEST_CASE("ResizeModel：起始狀態是原圖；解析度不合理時當 72") {
    ResizeModel m = ResizeModel::forImage(4000, 3000, 300);
    CHECK(m.w == 4000);
    CHECK(m.h == 3000);
    CHECK(m.dpi == 300);
    CHECK(m.lock);
    CHECK(m.resample);
    CHECK(m.resultWidth() == 4000);
    CHECK(m.resultHeight() == 3000);
    CHECK(ResizeModel::forImage(10, 10, 0).dpi == 72);
    CHECK(ResizeModel::forImage(10, 10, -5).dpi == 72);
}

TEST_CASE("ResizeModel：預設尺寸等比例放進正方形") {
    ResizeModel wide = ResizeModel::forImage(4000, 3000, 72);
    wide.fitInto(800);
    CHECK(wide.resultWidth() == 800);
    CHECK(wide.resultHeight() == 600);
    ResizeModel tall = ResizeModel::forImage(1000, 4000, 72);
    tall.fitInto(800);
    CHECK(tall.resultWidth() == 200);
    CHECK(tall.resultHeight() == 800);
    ResizeModel small = ResizeModel::forImage(100, 50, 72); // 比預設尺寸小：放大
    small.fitInto(320);
    CHECK(small.resultWidth() == 320);
    CHECK(small.resultHeight() == 160);
    CHECK(kResizePresets[0] == 320);
}

TEST_CASE("ResizeModel：鎖比例時改一邊，另一邊照原圖的比例跟著；不鎖就各改各的") {
    ResizeModel m = ResizeModel::forImage(4000, 3000, 72);
    m.setWidth(2000);
    CHECK(m.w == 2000);
    CHECK(m.h == 1500);
    m.setHeight(300);
    CHECK(m.w == 400);
    CHECK(m.h == 300);
    m.lock = false;
    m.setWidth(1000);
    CHECK(m.w == 1000);
    CHECK(m.h == 300);
    m.setHeight(50);
    CHECK(m.w == 1000);
    CHECK(m.h == 50);
    // 不是正數的輸入（打到一半、空的）不動
    m.setWidth(0);
    m.setHeight(-3);
    CHECK(m.w == 1000);
    CHECK(m.h == 50);
}

TEST_CASE("ResizeModel：欄位的數字照目前的單位") {
    ResizeModel m = ResizeModel::forImage(3000, 1500, 300);
    m.unit = LengthUnit::Percent;
    CHECK(m.widthInUnit() == 100);
    m.setWidth(50); // 50%
    CHECK(m.w == 1500);
    CHECK(m.h == 750);
    CHECK(m.heightInUnit() == 50);
    m.unit = LengthUnit::Inches;
    CHECK(m.widthInUnit() == 5);
    m.setHeight(5); // 5 英寸 × 300
    CHECK(m.h == 1500);
    CHECK(m.w == 3000);
    m.unit = LengthUnit::Cm;
    CHECK_THAT(m.widthInUnit(), WithinAbs(25.4, 1e-9));
}

TEST_CASE("ResizeModel：改解析度。實體單位加重新取樣時像素跟著變，其他情況像素不動") {
    ResizeModel m = ResizeModel::forImage(3000, 1500, 300);
    m.setDpi(150); // 單位是像素：只改解析度
    CHECK(m.dpi == 150);
    CHECK(m.w == 3000);

    m.unit = LengthUnit::Inches; // 20 × 10 英寸
    m.setDpi(300);               // 實體尺寸不變：像素加倍
    CHECK(m.dpi == 300);
    CHECK(m.w == 6000);
    CHECK(m.h == 3000);
    CHECK(m.widthInUnit() == 20);

    m.setResample(false); // 不重新取樣：尺寸回到原圖，之後改解析度不動像素
    CHECK(m.w == 3000);
    CHECK(m.h == 1500);
    m.setDpi(600);
    CHECK(m.w == 3000);
    CHECK(m.dpi == 600);
    CHECK(m.widthInUnit() == 5); // 同樣的像素，印出來變小

    m.dpiUnit = DpiUnit::PerCm;
    CHECK_THAT(m.dpiInUnit(), WithinAbs(600 / 2.54, 1e-9));
    m.setDpi(100); // 每公分 100 像素
    CHECK_THAT(m.dpi, WithinAbs(254.0, 1e-9));
    m.setDpi(0); // 不合理的輸入不動
    CHECK_THAT(m.dpi, WithinAbs(254.0, 1e-9));
}

TEST_CASE("ResizeModel：結果四捨五入，至少 1，最多 kMaxResizeSide；預覽的數字不夾上限") {
    ResizeModel m = ResizeModel::forImage(1000, 3, 72);
    m.setWidth(100); // 高度 0.3
    CHECK(m.previewHeight() == 1);
    CHECK(m.resultHeight() == 1);
    m.lock = false;
    m.setWidth(99999.6);
    CHECK(m.previewWidth() == 100000);
    CHECK(m.resultWidth() == kMaxResizeSide);
    m.setWidth(10.5);
    CHECK(m.resultWidth() == 11);
}
