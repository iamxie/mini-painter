#pragma once
#include <windows.h>

#include <memory>

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

struct ColorPanelState;

// 「調整顏色」面板。主視窗擁有一個，面板的狀態由它握著：對話框關掉時狀態不刪自己，
// 下次開啟或這個物件解構時才換掉，所以面板自己的函式還沒回來時狀態一定還在。
class ColorAdjustPanel {
public:
    ColorAdjustPanel();
    ~ColorAdjustPanel();
    ColorAdjustPanel(const ColorAdjustPanel&) = delete;
    ColorAdjustPanel& operator=(const ColorAdjustPanel&) = delete;

    // 開啟。已經開著就把它帶到前面。開不起來時呼叫 host->cancelAdjust()。
    void show(HWND owner, ColorAdjustHost* host);
    bool isOpen() const { return hwnd() != nullptr; }
    // 面板的視窗，給訊息迴圈呼叫 IsDialogMessage 用。沒開時是 nullptr。
    HWND hwnd() const;
    // 主視窗在使用者點了圖片（取白點）後呼叫，更新面板的參數顯示。
    void setParams(const ColorAdjustParams& p);
    // 關掉，不通知 host（呼叫端自己決定要套用還是放棄）。
    void close();

private:
    std::unique_ptr<ColorPanelState> state_;
};

} // namespace mp
