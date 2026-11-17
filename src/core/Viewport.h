#pragma once
#include "core/Geometry.h"

namespace mp {

// 畫布與圖片之間的座標換算。不碰視窗：MainWindow 每個滑鼠動作、縮放、捲動都經過這裡。
//
// 名詞：
//   畫布座標：以畫布左上角為原點的像素。
//   圖片座標：虛擬畫面的像素（單頁就是那張圖；兩頁並排是並排後的整個畫面）。
//   offset：圖片原點在畫布裡的位置（畫布座標）。zoom：一個圖片像素畫成幾個畫布像素。

constexpr float kViewMinZoom = 0.02f, kViewMaxZoom = 64.0f;

struct Viewport {
    float zoom = 1.0f;
    Pt offset;
};

// 畫布座標 <-> 圖片座標。
Pt canvasToImage(const Viewport& v, Pt canvas);
Pt imageToCanvas(const Viewport& v, Pt image);

// 把 offset 夾進合理的範圍：畫面比畫布小的方向置中，大的方向不能露出畫布外的空白。結果對齊整數像素
// （高品質縮放的點陣圖才會 1:1 貼上，不被 bilinear 再糊一次）。viewW / viewH 是虛擬畫面的大小（圖片座標）。
Pt clampOffset(Pt offset, float zoom, float viewW, float viewH, float canvasW, float canvasH);

// 以畫布上的 anchor 為準把倍率改成 newZoom（先夾進 kViewMinZoom..kViewMaxZoom）：anchor 底下的那個圖片點留在原地。
// 回傳新的檢視；還沒夾 offset，呼叫端接著 clampOffset。
Viewport zoomAbout(const Viewport& v, float newZoom, Pt anchorCanvas);

// 兩頁並排的版面。一頁的像素大小：
struct PageSize {
    int width = 0, height = 0;
};
// 虛擬畫面的大小：單頁就是那一頁；兩頁是寬相加、高取大的。second 為 nullptr 表示這一屏只有一頁。
Pt spreadSize(PageSize first, const PageSize* second);
// 某一頁在虛擬畫面裡的位置：兩頁底對齊；閱讀順序左到右時第一頁在左，右到左（日漫）時第一頁在右。
// wantSecond 為 true 取第二頁（沒有第二頁時回空的矩形）。
Rc pageRectInSpread(PageSize first, const PageSize* second, bool wantSecond, bool rightToLeft);
// 兩頁並排的一屏裡有一頁讀不到時，那一頁的位置畫佔位框，大小用另一頁的（版面不跳）。
// first / second 是讀到的頁的大小，讀不到給 nullptr。兩頁都讀不到回 false（沒有可排的）。
bool spreadWithPlaceholder(const PageSize* first, const PageSize* second, PageSize& outFirst, PageSize& outSecond);

} // namespace mp
