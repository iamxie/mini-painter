#pragma once
#include <cstdint>
#include <string>
#include <type_traits>
#include <variant>
#include <vector>

#include "core/Fonts.h"
#include "core/Geometry.h"

namespace mp {

// 物件的種類。順序要和下面 Shape 的型別清單一樣（Annotation::kind() 直接用 variant 的 index，有 static_assert 對照）。
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
// 物件種類的英文名（"Rect"、"Mosaic"…）。不翻譯：放在錯誤訊息裡給回報問題用，跟 HRESULT 一樣是技術資訊。
const wchar_t* kindName(Kind kind);
// 這個線寬是選單的第幾級；不是選單上的值回 -1。
int strokeLevelForWidth(float width);

struct StrokeStyle {
    Color stroke = Color::rgb(255, 59, 48);
    float width = 3.0f; // 圖片像素。選單給的是 strokeWidthForLevel 的七個值
    LineStyle line = LineStyle::Solid;
    ArrowMode arrows = ArrowMode::None;
    Color fill = Color::noColor();
};

struct TextStyle {
    std::wstring font = kTextFont;
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
// 有外框的物件共用這八個；每種物件另外的控制點在它自己的 struct 裡有名字（接在 8 之後，或自己從 0 編）。
constexpr int H_TL = 0, H_T = 1, H_TR = 2, H_R = 3, H_BR = 4, H_B = 5, H_BL = 6, H_L = 7;

constexpr int kMinPoints = 3, kMaxPoints = 20;
constexpr float kMinInnerRatio = 0.05f; // 星形內圈半徑佔外圈的比例下限
constexpr float kMinZoom = 1.5f, kMaxZoom = 6.0f;
constexpr int kMinBlock = 4, kMaxBlock = 64;

// ---- 每種物件自己的資料。座標都是圖片像素。----

// 塗鴉。控制點：外框四個角（H_TL、H_TR、H_BR、H_BL）。外框被拉大時，筆跡連同線寬一起縮放。
struct FreehandShape {
    Rc rect;                    // 目前的外框
    std::vector<Pt> points;     // 原始筆跡，座標相對於原始外框左上角
    float origW = 1, origH = 1; // 原始外框大小
};

// 直線 / 箭頭 / 曲線。
struct LineShape {
    enum HandleId { Start = 0, End = 1, Bend = 2 }; // 兩端點（藍）、曲線中點（綠）
    Pt p0, p1, mid;
};

// 矩形、圓角矩形、橢圓：只有外框，控制點是 H_* 八個。
struct RectShape { Rc rect; };
struct RoundRectShape { Rc rect; };
struct EllipseShape { Rc rect; };

// 對話框。
struct BubbleShape {
    enum HandleId { TailRoot = 8, TailTip = 9 }; // 尾巴根部（繞橢圓）、尾巴尖端（自由移動）
    Rc rect;
    float tailAngle = 0.75f * kPi; // 尾巴根部在橢圓上的角度（弧度，0 在右，順時針增加）
    Pt tailTip;
};

// 星形。
struct StarShape {
    enum HandleId { Count = 8, Inner = 9 }; // 角數（繞外圈）、內圈半徑（沿中心到角數綠點的線）
    Rc rect;
    int pointCount = 5;
    float innerRatio = 0.5f;
    int turns = 0; // 跟著整張圖逆時針轉了幾個 90 度（0..3）：第一個頂點從正上方往逆時針方向偏這麼多
};

// 正多邊形。
struct PolygonShape {
    enum HandleId { Count = 8 }; // 邊數（繞半徑一半的圓）
    Rc rect;
    int pointCount = 6;
    int turns = 0; // 同 StarShape::turns
};

// 放大鏡。
struct MagnifierShape {
    enum HandleId { Radius = 0, Zoom = 1 }; // 半徑（藍）、倍率（綠，繞圓周）
    Pt center;
    float radius = 50;
    float zoom = 2.0f;
};

// 馬賽克。
struct MosaicShape {
    enum HandleId { Corner = 0, Block = 1 }; // 右下角（藍）、格子大小（綠，沿上邊）
    Rc rect;
    int blockSize = 16;
};

// 文字方塊。控制點：H_L、H_R（只能改寬度）。rect 的 left / top / right 由使用者決定，bottom 由排版決定
// （要 DirectWrite 才量得出來：Editor 用 app 給的量測函式回填）。
struct TextShape {
    Rc rect;
    TextStyle style;
    std::wstring value;
};

using Shape = std::variant<FreehandShape, LineShape, RectShape, RoundRectShape, EllipseShape, BubbleShape, StarShape,
                           PolygonShape, MagnifierShape, MosaicShape, TextShape>;

namespace detail {
template <Kind K, class T>
constexpr bool kKindIs = std::is_same_v<std::variant_alternative_t<static_cast<size_t>(K), Shape>, T>;
}
static_assert(detail::kKindIs<Kind::Freehand, FreehandShape> && detail::kKindIs<Kind::Line, LineShape> &&
              detail::kKindIs<Kind::Rect, RectShape> && detail::kKindIs<Kind::RoundRect, RoundRectShape> &&
              detail::kKindIs<Kind::Ellipse, EllipseShape> && detail::kKindIs<Kind::Bubble, BubbleShape> &&
              detail::kKindIs<Kind::Star, StarShape> && detail::kKindIs<Kind::Polygon, PolygonShape> &&
              detail::kKindIs<Kind::Magnifier, MagnifierShape> && detail::kKindIs<Kind::Mosaic, MosaicShape> &&
              detail::kKindIs<Kind::Text, TextShape> && std::variant_size_v<Shape> == 11,
              "Kind 的順序要和 Shape 的型別清單一樣");

// 一個標註物件：每種物件共用的 id 與線條樣式，加上那一種物件自己的資料。
// 依物件種類做不同事的地方一律用 std::visit 加每種型別一個多載：加一種物件時，漏寫的地方編譯不過。
struct Annotation {
    uint32_t id = 0;
    StrokeStyle stroke;
    Shape shape = RectShape{};

    Kind kind() const { return static_cast<Kind>(shape.index()); }
    // 是 T 這種物件就回它的資料，不是回 nullptr。
    template <class T> T* as() { return std::get_if<T>(&shape); }
    template <class T> const T* as() const { return std::get_if<T>(&shape); }
};

// ---- 建立 ----

// 在指定中心建立預設形狀。size 是可見區域短邊，決定預設大小。
Annotation makeShape(Kind kind, Pt center, float size, const StrokeStyle& style);
// text：方塊裡預設的字（app 給翻譯好的「文字」）。
Annotation makeText(Pt center, float size, const StrokeStyle& style, const TextStyle& textStyle,
                    const std::wstring& text = L"Text");
// 從滑鼠軌跡建立塗鴉（軌跡至少一點）。
Annotation makeFreehand(const std::vector<Pt>& path, const StrokeStyle& style);

// ---- 幾何 ----

// 有外框的物件（直線與放大鏡以外的）回它的外框，沒有回 nullptr。
Rc* rectOf(Annotation& a);
const Rc* rectOf(const Annotation& a);

Rc bounds(const Annotation& a);
// 畫出來的線有多寬（圖片像素）：塗鴉被拉大時線寬跟著放大，其他物件就是 stroke.width。
float paintedStrokeWidth(const Annotation& a);
std::vector<Handle> handles(const Annotation& a);
// tol 是命中容差（圖片像素）。
bool hitTest(const Annotation& a, Pt p, float tol);
void moveBy(Annotation& a, float dx, float dy);
// 把控制點拖到 p。回傳 false 表示這個 id 不存在。
bool dragHandle(Annotation& a, int handleId, Pt p);
// 整張圖逆時針轉 90 度時，物件跟著轉。imageWidth 是轉之前的寬度。
void rotateCCW90(Annotation& a, int imageWidth);
// 整張圖重新取樣時，物件座標跟著縮放（文字字級、線寬不變；塗鴉的線寬跟著外框變）。
void scaleBy(Annotation& a, float sx, float sy);

// 塗鴉目前的筆跡（套用外框縮放後的絕對座標）。
std::vector<Pt> freehandPoints(const FreehandShape& s);
// 直線的貝茲控制點（由 mid 反推）。
Pt lineControlPoint(const LineShape& s);
// 星形 / 正多邊形的頂點。
std::vector<Pt> starPoints(const StarShape& s);
std::vector<Pt> starPoints(const PolygonShape& s);
// 對話框的輪廓折線（命中判斷與選單圖示用；存檔與畫面用真正的弧線）。
std::vector<Pt> bubbleOutline(const BubbleShape& s);
// 對話框尾巴根部的兩個點與尖端。
void bubbleTail(const BubbleShape& s, Pt& rootA, Pt& rootB, Pt& tip);
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
