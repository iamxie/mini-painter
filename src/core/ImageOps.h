#pragma once
#include "core/Image.h"

namespace mp {

// 逆時針轉 90 度。
Image rotateCCW90(const Image& src);

// 依 EXIF orientation（1..8）把像素轉正。1 或不認得的值就原樣複製。
Image applyExifOrientation(const Image& src, int orientation);

// 重新取樣到指定大小：Lanczos3，放大縮小都用。縮小時濾波器寬度隨倍率放大（不會有摩爾紋）。
// 多執行緒。顯示的高品質縮放和「調整尺寸」都走這裡。
Image resample(const Image& src, int newW, int newH);

// 舊的快速版：縮小面積平均、放大雙線性。給 GPU 上限縮圖等不講究品質的地方用。
Image resampleFast(const Image& src, int newW, int newH);

// 馬賽克：取 src 裡 [x0, y0, x1, y1) 區域，每 block 像素一格取平均，
// 回傳縮小後的小圖（每格一個像素）。區域會先夾到圖內。
// 區域為空或 block <= 0 時回傳空圖。
Image mosaicBlocks(const Image& src, int x0, int y0, int x1, int y1, int block);

} // namespace mp
