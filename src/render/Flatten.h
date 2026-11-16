#pragma once
#include "core/Document.h"
#include "core/Image.h"
#include "render/RenderDevice.h"

namespace mp {

// flatten 畫得出正確像素的最大邊長（2^19 − 1）。再大的話 Direct2D 的軟體算繪每個呼叫都成功，
// 但畫出來的位置會錯（2026-11-16 在 Win10 VM 量的：524287 正確，524288 起有錯的像素）。
constexpr int kFlattenMaxSide = 524287;

// 把標註畫到底圖上，壓成一張圖（存檔用）。標註以外的像素和底圖一模一樣。
// 沒有標註時 out 是底圖的複本；呼叫端有底圖可以直接用的話不必呼叫。
// 畫不出正確結果（邊長超過 kFlattenMaxSide、記憶體不夠、繪製失敗）回 false，這時 out 不能用。
// 記憶體：過程中除了底圖，另有一張畫布與 out（各一張圖的大小）；有放大鏡時再多一張底圖的點陣圖。
bool flatten(RenderDevice& device, const DocState& state, Image& out);

} // namespace mp
