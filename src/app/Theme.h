#pragma once
#include <d2d1.h>

#include <cmath>

namespace mp {

// 邏輯像素（96 DPI 時的一個像素）換成實際像素。主視窗、彈出選單、資訊面板量尺寸都用這一個：
// 成員叫 px，寫 px(10) 就是「10 個邏輯像素」。
struct UiScale {
    float factor = 1.0f;
    float operator()(float v) const { return v * factor; }
    int round(float v) const { return static_cast<int>(std::lround(v * factor)); }
    static UiScale forDpi(unsigned dpi) { return {dpi / 96.0f}; }
};

// 介面的顏色只寫在這裡。同一個意思的顏色只有一個名字（要做深色模式就從這裡換）。
namespace theme {

constexpr D2D1_COLOR_F gray(float v, float a = 1.0f) { return {v, v, v, a}; }

// ---- 共用 ----
inline constexpr D2D1_COLOR_F kAccent{0.04f, 0.52f, 1.0f, 1.0f};      // 選取藍：控制點、選單裡選到的那一格的外框
inline constexpr D2D1_COLOR_F kAccentGuide{0.04f, 0.52f, 1.0f, 0.40f}; // 綠點按下時的軌跡提示
inline constexpr D2D1_COLOR_F kSelectedBg{0.78f, 0.85f, 0.95f, 1.0f}; // 亮起的工具列按鈕、選到的分頁
inline constexpr D2D1_COLOR_F kNoColorSlash{0.85f, 0.1f, 0.1f, 1.0f}; // 「無色」的紅斜線
inline constexpr D2D1_COLOR_F kWhite = gray(1.0f);
inline constexpr D2D1_COLOR_F kHairline = gray(0.0f, 0.25f);          // 圖片外緣的細框、捲軸

// ---- 主視窗 ----
inline constexpr D2D1_COLOR_F kChromeBg = gray(0.953f);   // 工具列、檔名列
inline constexpr D2D1_COLOR_F kMarkupBg = gray(0.93f);    // 標示工具列
inline constexpr D2D1_COLOR_F kChromeLine = gray(0.80f);  // 各列之間的分隔線
inline constexpr D2D1_COLOR_F kCanvasBg = gray(0.90f);
inline constexpr D2D1_COLOR_F kIcon = gray(0.20f);        // 圖示、檔名、畫布上的訊息
inline constexpr D2D1_COLOR_F kIconDisabled = gray(0.70f);
inline constexpr D2D1_COLOR_F kBtnHover = gray(0.87f);
inline constexpr D2D1_COLOR_F kTextBtn = gray(0.85f);     // 「裁切」文字按鈕
inline constexpr D2D1_COLOR_F kTextBtnHot = gray(0.78f);
inline constexpr D2D1_COLOR_F kHandleGreen{0.20f, 0.78f, 0.35f, 1.0f};
inline constexpr D2D1_COLOR_F kToastBg = gray(0.12f, 0.88f);
inline constexpr D2D1_COLOR_F kCropShade = gray(0.0f, 0.5f); // 裁切框外蓋的深色
inline constexpr D2D1_COLOR_F kCropFrame = gray(1.0f, 0.9f);        // 裁切框的細線
inline constexpr D2D1_COLOR_F kCropHandleShadow = gray(0.0f, 0.55f); // 裁切把手白線底下的暗邊
inline constexpr D2D1_COLOR_F kCropSizeBg = gray(0.0f, 0.6f);        // 拖裁切框時中央的尺寸標籤

// ---- 資訊面板 ----
inline constexpr D2D1_COLOR_F kPanelBg = gray(0.97f);
inline constexpr D2D1_COLOR_F kPanelLine = gray(0.82f);
inline constexpr D2D1_COLOR_F kPanelLabel = gray(0.45f);
inline constexpr D2D1_COLOR_F kPanelText = gray(0.12f);

// ---- 彈出選單 ----
inline constexpr D2D1_COLOR_F kPopupBg = gray(0.98f);
inline constexpr D2D1_COLOR_F kPopupHover = gray(0.85f);
inline constexpr D2D1_COLOR_F kPopupInk = gray(0.15f);         // 選單裡的線條、文字、形狀圖示
inline constexpr D2D1_COLOR_F kPopupBtnLine = gray(0.6f);      // 「顯示顏色…」的外框
inline constexpr D2D1_COLOR_F kSwatchNoneLine = gray(0.5f);    // 「無色」那一格的外框
inline constexpr D2D1_COLOR_F kSwatchLine = gray(0.0f, 0.15f); // 色塊的外框

} // namespace theme

} // namespace mp
