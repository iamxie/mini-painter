#pragma once
#include <windows.h>
#include <d2d1.h>

// 彈出選單關閉後送給主視窗：請把鍵盤焦點拿回去（編輯中就還給編輯框）。
#define WM_MP_POPUP_CLOSED (WM_APP + 3)

#include <functional>

#include "core/Annotation.h"

namespace mp {

class Renderer;

// 彈出選單都是 WS_POPUP 視窗，失去焦點就關閉。anchor 是螢幕座標的按鈕矩形，選單貼在它下方。
// 每次改動立刻呼叫 onChange。

void showLineStylePopup(HWND owner, Renderer& renderer, RECT anchor, float uiScale, StrokeStyle current,
                        std::function<void(const StrokeStyle&)> onChange);

// allowNone：第一格是「無色」。
void showColorPopup(HWND owner, Renderer& renderer, RECT anchor, float uiScale, Color current, bool allowNone,
                    std::function<void(Color)> onChange);

void showShapesPopup(HWND owner, Renderer& renderer, RECT anchor, float uiScale, std::function<void(Kind)> onPick);

void showFormatPopup(HWND owner, Renderer& renderer, RECT anchor, float uiScale, TextStyle current,
                     std::function<void(const TextStyle&)> onChange);

// 目前有沒有彈出選單開著。
bool isPopupOpen();
void closePopup();
// 最近一次彈出選單關閉的時刻（GetTickCount64）。
ULONGLONG lastPopupCloseTick();

// 用系統對話框選顏色。回傳 false 表示取消。
bool chooseColorDialog(HWND owner, Color& inout);

// 畫一個形狀的小圖示（給選單和工具列用）。
void drawKindIcon(Renderer& renderer, ID2D1RenderTarget* rt, Kind kind, D2D1_RECT_F box, D2D1_COLOR_F color);

} // namespace mp
