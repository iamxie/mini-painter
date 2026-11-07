#pragma once
#include <array>
#include <cstdint>

#include "core/Image.h"

namespace mp {

// 「調整顏色」面板的全部參數。預設值等於不改變圖片。
struct ColorAdjustParams {
    // 色階：黑點、白點是輸入亮度（0..1），黑點 < 白點。中間調 0..1，0.5 是不變（gamma）。
    float blackPoint = 0.0f;
    float midtone = 0.5f;
    float whitePoint = 1.0f;

    float exposure = 0.0f;   // -2..2 EV
    float contrast = 0.0f;   // -1..1
    float highlights = 0.0f; // -1..1，負值壓暗亮部
    float shadows = 0.0f;    // -1..1，正值提亮陰影

    float saturation = 0.0f;  // -1..1，-1 變灰階
    float temperature = 0.0f; // -1..1，正值偏暖（偏黃）
    float tint = 0.0f;        // -1..1，正值偏洋紅，負值偏綠
    float sepia = 0.0f;       // 0..1

    float clarity = 0.0f; // 0..1，局部對比

    // 白平衡增益，由滴管設定。1,1,1 是不變。
    float wbGainR = 1.0f;
    float wbGainG = 1.0f;
    float wbGainB = 1.0f;

    bool isIdentity() const;
};

// 套用全部參數，回傳新圖。alpha 不變。輸入是 premultiplied，內部要先還原再處理。
Image applyColorAdjust(const Image& src, const ColorAdjustParams& p);

// 亮度直方圖（Rec.601 luma，256 格）。
std::array<uint32_t, 256> luminanceHistogram(const Image& img);

// 自動色階：依直方圖把黑點、白點設到 0.1% / 99.9% 的位置，其他參數不動。
ColorAdjustParams autoLevels(const Image& img, ColorAdjustParams base);

// 滴管：以 (x, y) 附近 5x5 的平均色為白色基準，設定 wbGain*。座標超出範圍時夾回圖內。
void setWhiteBalanceFrom(ColorAdjustParams& p, const Image& img, int x, int y);

} // namespace mp
