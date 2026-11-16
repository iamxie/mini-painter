#pragma once
#include <windows.h>
#include <d2d1.h>

#include <functional>
#include <memory>

#include "app/AppIds.h"
#include "app/Theme.h"
#include "core/Annotation.h"

namespace mp {

class RenderDevice;
class Surface;
struct PopupBase;

// 工具列的彈出選單：線型、顏色、形狀、格式。都是 WS_POPUP 視窗，失去焦點就關閉，同時最多開一個。
// 主視窗擁有一個 PopupMenus，選單物件由它握著。選單的視窗關掉時物件不刪自己：等主視窗收到
// WM_MP_POPUP_CLOSED 呼叫 reap()、或開下一個選單、或這個物件解構時才放掉，
// 所以選單自己的函式還沒回來時（例如選色對話框開著）物件一定還在。
class PopupMenus {
public:
    PopupMenus();
    ~PopupMenus();
    PopupMenus(const PopupMenus&) = delete;
    PopupMenus& operator=(const PopupMenus&) = delete;

    // anchor 是螢幕座標的按鈕矩形，選單貼在它下方。每次改動立刻呼叫 onChange。
    void showLineStyle(HWND owner, RenderDevice& device, RECT anchor, UiScale px, StrokeStyle current,
                       std::function<void(const StrokeStyle&)> onChange);
    void showColor(HWND owner, RenderDevice& device, RECT anchor, UiScale px, Color current, std::function<void(Color)> onChange);
    void showShapes(HWND owner, RenderDevice& device, RECT anchor, UiScale px, std::function<void(Kind)> onPick);
    void showFormat(HWND owner, RenderDevice& device, RECT anchor, UiScale px, TextStyle current,
                    std::function<void(const TextStyle&)> onChange);

    // 目前有沒有選單開著。
    bool isOpen() const;
    void close();
    // 最近一次選單關閉的時刻（GetTickCount64）。
    ULONGLONG lastCloseTick() const { return lastClose_; }
    // 視窗已經關掉的選單物件放掉。主視窗收到 WM_MP_POPUP_CLOSED 時呼叫。
    void reap();

private:
    void show(std::unique_ptr<PopupBase> popup, HWND owner, RenderDevice& device, RECT anchor, UiScale px);

    std::unique_ptr<PopupBase> current_;
    ULONGLONG lastClose_ = 0;
};

// 用系統對話框選顏色。回傳 false 表示取消。
bool chooseColorDialog(HWND owner, Color& inout);

// 畫一個形狀的小圖示（形狀選單用）。只認形狀選單裡有的那幾種。
void drawKindIcon(Surface& surface, Kind kind, D2D1_RECT_F box, D2D1_COLOR_F color);

} // namespace mp
