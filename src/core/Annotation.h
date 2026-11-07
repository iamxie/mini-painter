#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "core/Geometry.h"

namespace mp {

enum class Kind { Freehand, Line, Rect, RoundRect, Ellipse, Bubble, Star, Polygon, Magnifier, Mosaic, Text };
enum class LineStyle { Solid, Dashed, Fuzzy };
enum class ArrowMode { None, Start, End, Both };
enum class TextAlign { Left, Center, Right, Justify };

struct Color {
    uint8_t r = 0, g = 0, b = 0, a = 255;
    bool none() const { return a == 0; }
    static Color rgb(uint8_t r, uint8_t g, uint8_t b) { return {r, g, b, 255}; }
    static Color noColor() { return {0, 0, 0, 0}; }
};
inline bool operator==(Color a, Color b) { return a.r == b.r && a.g == b.g && a.b == b.b && a.a == b.a; }
inline bool operator!=(Color a, Color b) { return !(a == b); }

// 線型選單的七級粗細對應的像素寬度。
constexpr int kStrokeLevels = 7;
float strokeWidthForLevel(int level);

struct StrokeStyle {
    Color stroke = Color::rgb(255, 59, 48);
    int widthLevel = 2; // 0..6
    LineStyle line = LineStyle::Solid;
    ArrowMode arrows = ArrowMode::None;
    Color fill = Color::noColor();

    float width() const { return strokeWidthForLevel(widthLevel); }
};

struct TextStyle {
    std::wstring font = L"Microsoft JhengHei";
    float size = 24.0f;
    Color color = Color::rgb(0, 0, 0);
    bool bold = false, italic = false, underline = false;
    TextAlign align = TextAlign::Left;
};

enum class HandleKind { Blue, Green };

struct Handle {
    int id;
    Pt pos;
    HandleKind kind;
};

// 矩形類控制點的編號：0 左上、1 上、2 右上、3 右、4 右下、5 下、6 左下、7 左。
constexpr int H_TL = 0, H_T = 1, H_TR = 2, H_R = 3, H_BR = 4, H_B = 5, H_BL = 6, H_L = 7;

constexpr int kMinPoints = 3, kMaxPoints = 20;
constexpr float kMinZoom = 1.5f, kMaxZoom = 6.0f;
constexpr int kMinBlock = 4, kMaxBlock = 64;

// 所有標註物件共用一個 struct，依 kind 決定哪些欄位有效。座標都是圖片像素。
struct Annotation {
    uint32_t id = 0;
    Kind kind = Kind::Rect;
    StrokeStyle stroke;
    TextStyle text;

    // Rect、RoundRect、Ellipse、Bubble、Star、Polygon、Mosaic：外框。
    // Freehand：目前的外框。Text：left/top/right 有效，bottom 由排版決定（由 UI 回填）。
    Rc rect;

    // Freehand：原始筆跡，座標相對於原始外框左上角；origW/origH 是原始外框大小。
    std::vector<Pt> strokePoints;
    float origW = 1, origH = 1;

    // Line：兩端點，mid 是曲線中點（綠點）。
    Pt p0, p1, mid;

    // Bubble：尾巴根部在橢圓上的角度（弧度，0 在右，順時針增加）與尾巴尖端。
    float tailAngle = 0.75f * kPi;
    Pt tailTip;

    // Star、Polygon。
    int pointCount = 5;
    float innerRatio = 0.5f;

    // Magnifier：中心與半徑。
    Pt center;
    float radius = 50;
    float zoom = 2.0f;

    // Mosaic。
    int blockSize = 16;

    // Text。
    std::wstring textValue;
};

// ---- 建立 ----

// 在指定中心建立預設形狀。size 是可見區域短邊，決定預設大小。
Annotation makeShape(Kind kind, Pt center, float size, const StrokeStyle& style);
Annotation makeText(Pt center, float size, const StrokeStyle& style, const TextStyle& textStyle);
// 從滑鼠軌跡建立塗鴉（軌跡至少一點）。
Annotation makeFreehand(const std::vector<Pt>& path, const StrokeStyle& style);

// ---- 幾何 ----

Rc bounds(const Annotation& a);
std::vector<Handle> handles(const Annotation& a);
// tol 是命中容差（圖片像素）。
bool hitTest(const Annotation& a, Pt p, float tol);
void moveBy(Annotation& a, float dx, float dy);
// 把控制點拖到 p。回傳 false 表示這個 id 不存在。
bool dragHandle(Annotation& a, int handleId, Pt p);
// 整張圖逆時針轉 90 度時，物件跟著轉。imageWidth 是轉之前的寬度。
void rotateCCW90(Annotation& a, int imageWidth);
// 整張圖重新取樣時，物件座標跟著縮放（文字字級不變）。
void scaleBy(Annotation& a, float sx, float sy);

// 塗鴉目前的筆跡（套用外框縮放後的絕對座標）。
std::vector<Pt> freehandPoints(const Annotation& a);
// 直線的貝茲控制點（由 mid 反推）。
Pt lineControlPoint(const Annotation& a);
// 星形 / 正多邊形的頂點。
std::vector<Pt> starPoints(const Annotation& a);
// 對話框的輪廓折線（給測試與命中判斷；繪圖用真正的弧線）。
std::vector<Pt> bubbleOutline(const Annotation& a);
// 對話框尾巴根部的兩個點與尖端。
void bubbleTail(const Annotation& a, Pt& rootA, Pt& rootB, Pt& tip);
// 箭頭三角形的三個頂點。tip 是箭頭尖端，dir 是指向尖端的方向。
std::vector<Pt> arrowHead(Pt tip, Pt dir, float strokeWidth);

// 綠點按下時要畫的軌跡提示。
enum class GuideKind { None, Circle, Line };
struct HandleGuide {
    GuideKind kind = GuideKind::None;
    Rc ellipse;  // Circle：橢圓外框（沿 rect 比例）
    Pt a, b;     // Line：線段兩端
};
// 這個控制點的可動軌跡。藍點和自由移動的綠點回傳 None。
HandleGuide handleGuide(const Annotation& a, int handleId);

// 矩形類控制點：rect 的 8 個點。
std::vector<Handle> rectHandles(const Rc& r);
// 拖矩形的第 id 個控制點到 p，回傳新矩形（已翻正）。
Rc dragRectHandle(const Rc& r, int id, Pt p);

} // namespace mp
