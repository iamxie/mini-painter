#pragma once
#include <vector>

#include "core/Annotation.h"
#include "core/Geometry.h"

namespace mp {

// 裁切框：圖片像素座標，整數；right、bottom 不含（寬 = right - left）。
struct CropBox {
    int left = 0, top = 0, right = 0, bottom = 0;
    int width() const { return right - left; }
    int height() const { return bottom - top; }
};
inline bool operator==(const CropBox& a, const CropBox& b) {
    return a.left == b.left && a.top == b.top && a.right == b.right && a.bottom == b.bottom;
}
inline bool operator!=(const CropBox& a, const CropBox& b) { return !(a == b); }

// 滑鼠落在裁切框的哪個部位。
enum class CropPart { None, Inside, Left, Top, Right, Bottom, TopLeft, TopRight, BottomLeft, BottomRight };

// p 是圖片座標。tol 是邊的命中容差，corner 是角的範圍（沿邊從角算起多長算角，>= tol），都是圖片像素。
// 角優先於邊，邊優先於框內；兩個角都在範圍內時取近的。框外超過 tol 回 None。
CropPart cropHitTest(const CropBox& box, Pt p, float tol, float corner);

// 從 start 拖 part，滑鼠移了 (dx, dy) 圖片像素後的框。
// 角、邊：動到的邊四捨五入到整數，夾在圖內，不越過對邊（寬高至少 1）。
// Inside：整個框搬移，大小不變，碰到圖邊就停。None：原樣。
CropBox dragCropBox(const CropBox& start, CropPart part, float dx, float dy, int imageW, int imageH);

// 裁切後的標註物件：全部平移 (-left, -top)；外框（往外加一個線寬）和裁切框完全沒有交集的刪掉。
std::vector<Annotation> cropAnnotations(const std::vector<Annotation>& items, const CropBox& box);

} // namespace mp
