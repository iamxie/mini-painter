#pragma once

namespace mp {

// 檢視模式（看漫畫）。ini 裡的字串見 MainWindow::viewModeFromString。
enum class ViewMode { FitWindow, FitWidth, FitHeight, Actual, TwoPage };

// 高品質縮放（Lanczos3）的點陣圖最多幾個像素；放大到超過就畫原圖，記憶體和時間都不值得。
constexpr long long kHqMaxPixels = 40'000'000LL;

// 依檢視模式算倍率。canvas 是畫布大小；view 是要放進去的畫面（單頁是那張圖，兩頁是並排後）的寬高，圖片像素。
// 翻頁時的倍率與預先縮圖用的倍率都用它算，兩邊才會一致。最小 0.01。
float fitZoom(ViewMode mode, float canvasW, float canvasH, float viewW, float viewH, bool upscaleSmall);

// w×h 的圖以 zoom 顯示時，高品質縮圖的尺寸。倍率是 1、超過 maxSide 或 kHqMaxPixels 時回 false（畫原圖）。
bool hqTargetSize(int w, int h, float zoom, unsigned maxSide, int& tw, int& th);

} // namespace mp
