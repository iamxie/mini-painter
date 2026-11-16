#pragma once
#include <windows.h>

#include "core/ColorAdjust.h"
#include "core/Image.h"

namespace mp {

// 調整尺寸對話框的結果。
struct ResizeResult {
    int width = 0, height = 0;   // 新的像素尺寸（resample 為 false 時與原尺寸相同）
    double dpi = 72;             // 像素/英寸
    bool resample = true;
};

// 顯示「調整尺寸」對話框（modal）。回傳 false 表示取消。
bool showResizeDialog(HWND owner, int curW, int curH, double curDpi, ResizeResult& out);

// 「調整顏色」是 modeless 面板，透過這組回呼和主視窗互動。
struct ColorAdjustHost {
    virtual ~ColorAdjustHost() = default;
    virtual const Image& baseImage() = 0;                       // 調整前的底圖
    virtual void previewChanged(const ColorAdjustParams& p) = 0; // 參數變了，主視窗更新預覽
    virtual void commitAdjust(const ColorAdjustParams& p) = 0;   // 按「好」
    virtual void cancelAdjust() = 0;                             // 按取消或關閉
    virtual void beginPickWhite() = 0;                           // 按滴管，等使用者點圖片
};

// 開啟調整顏色面板。同時只會有一個。
void showColorAdjustPanel(HWND owner, ColorAdjustHost* host);
bool isColorAdjustPanelOpen();
// 面板的視窗，給訊息迴圈呼叫 IsDialogMessage 用。沒開時是 nullptr。
HWND colorAdjustPanelHwnd();
// 主視窗在使用者點了圖片後呼叫，更新面板的參數顯示。
void colorAdjustPanelSetParams(const ColorAdjustParams& p);
void closeColorAdjustPanel();

} // namespace mp
