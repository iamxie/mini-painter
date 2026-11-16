#pragma once
#include <vector>

#include "core/Annotation.h"
#include "core/Image.h"
#include "render/BitmapCache.h"
#include "render/Surface.h"

namespace mp {

// 底圖與標註的繪製。沒有自己的狀態：要用的東西都由呼叫端在 PaintCtx 給。
// 座標都是圖片像素，畫在 surface 目前的 transform 下（畫面上是檢視的縮放與位移，存檔時是單位矩陣）。

// 文字方塊的字和邊框之間的留白（圖片像素）。文字編輯的輸入框要對齊同一個位置。
constexpr float kTextPad = 4.0f;

struct PaintCtx {
    Surface* surface = nullptr;
    ImagePtr image;                       // 底圖（放大鏡、馬賽克要從它取樣）
    const ImageBitmap* imageBmp = nullptr; // 底圖在 surface 的 target 上的點陣圖；沒有的話底圖與放大鏡的內容不畫
    MosaicCache* mosaic = nullptr;        // 沒有的話馬賽克不畫
};

void drawImage(const PaintCtx& ctx);
// skipId：這個 id 的物件不畫（文字編輯中的那個由 EDIT 控制項顯示）。
void drawAnnotations(const PaintCtx& ctx, const std::vector<Annotation>& items, uint32_t skipId = 0);
void drawAnnotation(const PaintCtx& ctx, const Annotation& a);

// 文字物件依目前寬度排版後的高度（圖片像素，含上下留白）。排不出來時用字級估一行的高度。
float textHeight(RenderDevice& device, const TextShape& t);

} // namespace mp
