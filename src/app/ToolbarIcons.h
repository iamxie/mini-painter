#pragma once
#include <d2d1.h>

#include <vector>

#include "app/Theme.h"
#include "core/Annotation.h"

namespace mp {

class Surface;

// 工具列按鈕。
enum ButtonId {
    BTN_NONE = 0,
    BTN_ZOOMOUT, BTN_ZOOM100, BTN_ZOOMIN, BTN_ROTATE, BTN_MARKUP, BTN_INFO,
    BTN_FREEHAND, BTN_SHAPES, BTN_TEXT, BTN_ADJCOLOR, BTN_ADJSIZE, BTN_LINESTYLE, BTN_LINECOLOR, BTN_FILL, BTN_FORMAT,
    BTN_CROP,       // 標示工具列：進入 / 取消裁切模式
    BTN_CROP_APPLY, // 裁切模式中才有：標示工具列最右邊的「裁切」文字按鈕
};

// 圖示按鈕的大小（邏輯像素）。
constexpr float kBtnW = 36, kBtnH = 30;
// 圖示預設的線寬（邏輯像素）。
constexpr float kIconStroke = 1.6f;

// 圖示是資料：一個圖示由幾筆組成，座標是邏輯像素、原點在按鈕中心（x 往右、y 往下）。
// 改圖示就改 ToolbarIcons.cpp 裡那張表，不必動繪製的程式。

// 路徑的一段。
struct IconSeg {
    enum class Kind {
        Move,   // x y：開始新的一筆
        Line,   // x y
        Bezier, // x1 y1 x2 y2 x y（三次貝茲）
        Arc,    // x y r：逆時針繞大弧到 (x, y)
        Close,  // 封閉目前這一筆
    };
    Kind kind;
    float v[6];
};

struct IconPart {
    enum class Kind {
        Line,    // v = x0 y0 x1 y1
        Rect,    // v = 左 上 右 下（只有框）
        Ellipse, // v = cx cy rx ry（只有框）
        Dot,     // v = cx cy r（實心圓）
        Path,    // path：一筆或多筆的路徑（只有線）
        Text,    // text：置中的字；v[0] 是字級，v[1] 是文字區右邊往內縮多少
    };
    Kind kind;
    float v[4];
    float width = kIconStroke; // 線寬（Dot、Text 不用）
    std::vector<IconSeg> path;
    const wchar_t* text = nullptr;
    bool bold = false;
};

// 這顆按鈕的圖示。沒有固定圖示的回 nullptr：線條顏色與填色（跟著目前的顏色變，見下面兩個函式）、文字按鈕。
const std::vector<IconPart>* toolbarIcon(int buttonId);

// 把圖示畫在 button（視窗座標）的中央。
void drawIcon(Surface& surface, const std::vector<IconPart>& icon, const D2D1_RECT_F& button, UiScale px, D2D1_COLOR_F color);
// 「線條顏色」按鈕：一圈目前的線條顏色；無色時是空心圓加紅斜線。
void drawLineColorIcon(Surface& surface, const D2D1_RECT_F& button, UiScale px, Color stroke, D2D1_COLOR_F iconColor);
// 「填色」按鈕：一塊目前的填色（外面一圈細框）；無色時是空心方塊加紅斜線。
void drawFillIcon(Surface& surface, const D2D1_RECT_F& button, UiScale px, Color fill, D2D1_COLOR_F iconColor);

} // namespace mp
