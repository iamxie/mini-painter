#pragma once
#include <string>

namespace mp {

// 檢視模式（看漫畫）。每個模式的 ini 名稱與顯示名只寫在 ViewFit.cpp 的那張表。
enum class ViewMode { FitWindow, FitWidth, FitHeight, Actual, TwoPage };
constexpr int kViewModeCount = 5;

// ini 裡的名字：fitwindow / fitwidth / fitheight / actual / twopage。
const wchar_t* viewModeKey(ViewMode mode);
// ini 的名字 → 模式。不認得的（含空字串）回 FitWindow。
ViewMode viewModeFromKey(const std::wstring& key);
// 顯示名（英文翻譯 key），例如 L"Fit to Window"。設定面板的下拉選單用。
const wchar_t* viewModeName(ViewMode mode);

// 高品質縮放（Lanczos3）的點陣圖最多幾個像素；放大到超過就畫原圖，記憶體和時間都不值得。
constexpr long long kHqMaxPixels = 40'000'000LL;

// 依檢視模式算倍率。canvas 是畫布大小；view 是要放進去的畫面（單頁是那張圖，兩頁是並排後）的寬高，圖片像素。
// 翻頁時的倍率與預先縮圖用的倍率都用它算，兩邊才會一致。最小 0.01。
float fitZoom(ViewMode mode, float canvasW, float canvasH, float viewW, float viewH, bool upscaleSmall);

// w×h 的圖以 zoom 顯示時，高品質縮圖的尺寸。倍率是 1、超過 maxSide 或 kHqMaxPixels 時回 false（畫原圖）。
bool hqTargetSize(int w, int h, float zoom, unsigned maxSide, int& tw, int& th);

} // namespace mp
