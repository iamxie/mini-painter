#pragma once
#include <windows.h>
#include <d2d1.h>

#include <vector>

#include "app/Theme.h"
#include "app/ToolbarIcons.h"
#include "core/Annotation.h"
#include "core/Commands.h"

namespace mp {

class RenderDevice;
class Surface;

// 工具列上的一顆按鈕（哪幾顆見 app/ToolbarIcons.h 的 ButtonId）。
struct Button {
    int id = BTN_NONE;
    D2D1_RECT_F rect{};    // 視窗座標
    bool enabled = true;
    bool pressed = false;  // 切換按鈕亮起
    bool dropdown = false; // 右下角畫小三角
};

// 工具列的文字（檔名列、文字按鈕）的字級，邏輯像素。
constexpr float kUiTextSize = 13;

// 排按鈕時要看的狀態，主視窗每次重排時給。
struct ToolbarState {
    bool showMarkup = false, showInfo = false; // 標示工具列、資訊面板開著
    bool freehand = false, adjusting = false, cropping = false;
    bool hasImage = false;
    bool zoomIsActual = false; // 倍率就是 1：「1:1」那一顆不能按
    CommandState commands;     // 對應到指令的按鈕用指令的可用條件（和選單、快捷鍵同一份）
    Color stroke, fill;        // 「線條顏色」「填色」兩顆顯示的顏色
};

// 主工具列與標示工具列的按鈕：排版、繪製、命中。不碰視窗；哪顆按下去做什麼是主視窗的事。
class Toolbar {
public:
    // 重排所有按鈕並算出每一顆能不能按、有沒有亮起。toolbar / markup 是兩列在視窗裡的位置
    // （state.showMarkup 是 false 時 markup 不用）。狀態一變就要再呼叫一次。
    void layout(const D2D1_RECT_F& toolbar, const D2D1_RECT_F& markup, const ToolbarState& state, UiScale px, RenderDevice& device);
    // 畫所有按鈕（底色、圖示、下拉三角）。
    void draw(Surface& surface, UiScale px) const;

    // pt（視窗座標）上的按鈕；沒有回 BTN_NONE。
    int hit(POINT pt, const Button** out = nullptr) const;
    const Button* find(int id) const;
    const std::vector<Button>& buttons() const { return buttons_; }

    // 提示用的按鈕名稱（翻譯好的）。文字按鈕本身就寫了名字，回空字串。
    static const wchar_t* name(int id);

    int hover = BTN_NONE;   // 滑鼠在哪一顆上面
    int pressed = BTN_NONE; // 滑鼠按著哪一顆（放開時還在同一顆才算點到）

private:
    void drawButton(Surface& surface, UiScale px, const Button& b) const;

    std::vector<Button> buttons_;
    Color stroke_, fill_;
};

} // namespace mp
