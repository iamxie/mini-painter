#include <catch2/catch_test_macros.hpp>

#include <windows.h>

#include <cmath>

#include "render/Renderer.h"
#include "render_fixture.h"
#include "test_util.h"

using namespace mp;
using namespace mp::test;

// 存檔寫進使用者檔案的像素（flatten）與每種標註的繪製。
// 檢查的是「該有顏色的地方有、不該有的地方沒有」，不逐像素比對整張圖（反鋸齒的邊緣不穩定）。

namespace {

constexpr Color kRed = {255, 0, 0, 255};
constexpr Color kBlue = {0, 0, 255, 255};
constexpr Color kWhite = {255, 255, 255, 255};

// 線寬等級 4 = 8 像素：線的中心那個像素一定整個蓋滿。
StrokeStyle redStroke(int level = 4) {
    StrokeStyle s;
    s.stroke = kRed;
    s.width = strokeWidthForLevel(level);
    return s;
}

// 預設形狀：makeShape 的 half = size * 0.15，size 400 → 中心上下左右各 60。
// 0.15 不是剛好的浮點數（半徑會是 60.000004），座標取整數，像素位置才說得準。
Annotation shape(Kind k, Pt c, const StrokeStyle& s) {
    Annotation a = makeShape(k, c, 400, s);
    a.id = 1;
    auto snap = [](Pt& p) { p = {std::round(p.x), std::round(p.y)}; };
    if (Rc* r = rectOf(a)) *r = {std::round(r->left), std::round(r->top), std::round(r->right), std::round(r->bottom)};
    if (auto* m = a.as<MagnifierShape>()) m->radius = std::round(m->radius);
    if (auto* l = a.as<LineShape>()) { snap(l->p0); snap(l->p1); snap(l->mid); }
    if (auto* bubble = a.as<BubbleShape>()) snap(bubble->tailTip);
    return a;
}

} // namespace

TEST_CASE("flatten：矩形的框線、填色與框外") {
    RenderFixture f;
    Annotation a = shape(Kind::Rect, {100, 100}, redStroke());
    Image out = f.flat(whiteImage(200, 200), {a});
    CHECK(isNear(pixel(out, 40, 100), kRed));    // 左邊
    CHECK(isNear(pixel(out, 160, 100), kRed));   // 右邊
    CHECK(isNear(pixel(out, 100, 40), kRed));    // 上邊
    CHECK(isNear(pixel(out, 100, 160), kRed));   // 下邊
    CHECK(isNear(pixel(out, 41, 41), kRed));     // 直角的角落有線
    CHECK(isNear(pixel(out, 100, 100), kWhite)); // 沒有填色：裡面是底圖
    CHECK(isNear(pixel(out, 20, 100), kWhite));  // 框外
    CHECK(isNear(pixel(out, 180, 180), kWhite));

    a.stroke.fill = kBlue;
    out = f.flat(whiteImage(200, 200), {a});
    CHECK(isNear(pixel(out, 100, 100), kBlue));
    CHECK(isNear(pixel(out, 60, 60), kBlue));
    CHECK(isNear(pixel(out, 40, 100), kRed)); // 框線畫在填色上面
    CHECK(isNear(pixel(out, 20, 100), kWhite));
}

TEST_CASE("flatten：沒有框線顏色時只有填色") {
    RenderFixture f;
    Annotation a = shape(Kind::Rect, {100, 100}, redStroke());
    a.stroke.stroke = Color::noColor();
    a.stroke.fill = kBlue;
    Image out = f.flat(whiteImage(200, 200), {a});
    CHECK(isNear(pixel(out, 42, 100), kBlue)); // 邊上是填色，不是紅線
    CHECK(isNear(pixel(out, 100, 100), kBlue));
    CHECK(isNear(pixel(out, 34, 100), kWhite)); // 原本線寬會蓋到的框外
}

TEST_CASE("flatten：圓角矩形的角落是空的，邊的中段有線") {
    RenderFixture f;
    Annotation a = shape(Kind::RoundRect, {100, 100}, redStroke());
    a.stroke.fill = kBlue;
    Image out = f.flat(whiteImage(200, 200), {a});
    CHECK(isNear(pixel(out, 100, 40), kRed));
    CHECK(isNear(pixel(out, 40, 100), kRed));
    CHECK(isNear(pixel(out, 41, 41), kWhite)); // 直角矩形在這裡有線（見上一個測試）
    CHECK(isNear(pixel(out, 159, 159), kWhite));
    CHECK(isNear(pixel(out, 100, 100), kBlue));
}

TEST_CASE("flatten：橢圓") {
    RenderFixture f;
    Annotation a = shape(Kind::Ellipse, {100, 100}, redStroke());
    (*rectOf(a)) = {20, 60, 180, 140}; // 寬 160、高 80
    a.stroke.fill = kBlue;
    Image out = f.flat(whiteImage(200, 200), {a});
    CHECK(isNear(pixel(out, 20, 100), kRed));
    CHECK(isNear(pixel(out, 180, 100), kRed));
    CHECK(isNear(pixel(out, 100, 60), kRed));
    CHECK(isNear(pixel(out, 100, 140), kRed));
    CHECK(isNear(pixel(out, 100, 100), kBlue));
    CHECK(isNear(pixel(out, 26, 66), kWhite)); // 外框的角落在橢圓外
    CHECK(isNear(pixel(out, 100, 40), kWhite));
}

TEST_CASE("flatten：直線、彎曲的線") {
    RenderFixture f;
    Annotation a = shape(Kind::Line, {100, 100}, redStroke()); // (40,100) 到 (160,100)
    Image out = f.flat(whiteImage(200, 200), {a});
    CHECK(isNear(pixel(out, 100, 100), kRed));
    CHECK(isNear(pixel(out, 45, 100), kRed));
    CHECK(isNear(pixel(out, 155, 100), kRed));
    CHECK(isNear(pixel(out, 100, 90), kWhite));
    CHECK(isNear(pixel(out, 100, 110), kWhite));
    CHECK(isNear(pixel(out, 20, 100), kWhite));

    a.as<LineShape>()->mid = {100, 60}; // 曲線通過 mid
    out = f.flat(whiteImage(200, 200), {a});
    CHECK(isNear(pixel(out, 100, 60), kRed));
    CHECK(isNear(pixel(out, 100, 100), kWhite));
    CHECK(isNear(pixel(out, 45, 97), kRed)); // 端點附近仍在線上
}

TEST_CASE("flatten：箭頭畫在指定的那一端") {
    RenderFixture f;
    // 箭頭大小 = 線寬 * 3 + 6 = 30：尖端往回 10 像素處，箭頭半寬 5，比線寬的一半（4）寬不了多少；
    // 往回 25 像素處半寬 12.5，量那一欄的紅色像素數。
    auto redInColumn = [](const Image& im, int x) {
        int n = 0;
        for (int y = 0; y < im.height; ++y)
            if (isNear(pixel(im, x, y), kRed)) ++n;
        return n;
    };
    Annotation a = shape(Kind::Line, {100, 100}, redStroke());
    Image none = f.flat(whiteImage(200, 200), {a});
    int plainStart = redInColumn(none, 65), plainEnd = redInColumn(none, 135);
    CHECK(plainStart >= 7);
    CHECK(plainStart <= 9);
    CHECK(plainEnd == plainStart);

    a.stroke.arrows = ArrowMode::End;
    Image end = f.flat(whiteImage(200, 200), {a});
    CHECK(redInColumn(end, 135) > plainEnd + 8);
    CHECK(redInColumn(end, 65) == plainStart);

    a.stroke.arrows = ArrowMode::Start;
    Image start = f.flat(whiteImage(200, 200), {a});
    CHECK(redInColumn(start, 65) > plainStart + 8);
    CHECK(redInColumn(start, 135) == plainEnd);

    a.stroke.arrows = ArrowMode::Both;
    Image both = f.flat(whiteImage(200, 200), {a});
    CHECK(redInColumn(both, 65) > plainStart + 8);
    CHECK(redInColumn(both, 135) > plainEnd + 8);
}

TEST_CASE("flatten：虛線有空隙，霧線比實線寬而且外圈比較淡") {
    RenderFixture f;
    Annotation a = shape(Kind::Line, {100, 100}, redStroke());
    a.stroke.line = LineStyle::Dashed;
    Image out = f.flat(whiteImage(200, 200), {a});
    int red = 0, white = 0;
    for (int x = 50; x < 150; ++x) {
        if (isNear(pixel(out, x, 100), kRed)) ++red;
        if (isNear(pixel(out, x, 100), kWhite)) ++white;
    }
    CHECK(red > 10);
    CHECK(white > 10);

    a.stroke.line = LineStyle::Fuzzy;
    out = f.flat(whiteImage(200, 200), {a});
    Color core = pixel(out, 100, 100), halo = pixel(out, 100, 110), outer = pixel(out, 100, 125);
    CHECK(core.r > 240);
    CHECK(core.g < 80); // 中心接近線條顏色
    CHECK(halo.g > core.g + 60); // 實線在 y=110 是白的；霧線在這裡有淡淡的顏色
    CHECK(halo.g < 250);
    CHECK(isNear(outer, kWhite));
}

TEST_CASE("flatten：半透明的線和底圖混合") {
    RenderFixture f;
    Annotation a = shape(Kind::Line, {100, 100}, redStroke());
    a.stroke.stroke = {255, 0, 0, 128};
    Image out = f.flat(whiteImage(200, 200), {a});
    Color c = pixel(out, 100, 100);
    CHECK(c.r > 250);
    CHECK(std::abs(c.g - 127) <= 3);
    CHECK(std::abs(c.b - 127) <= 3);
    CHECK(c.a == 255);
}

TEST_CASE("flatten：塗鴉照筆跡畫；外框改了大小，筆跡跟著縮放；只有一點時是圓點") {
    RenderFixture f;
    Annotation a = makeFreehand({{50, 50}, {150, 50}, {150, 150}}, redStroke());
    a.id = 1;
    Image out = f.flat(whiteImage(200, 200), {a});
    CHECK(isNear(pixel(out, 100, 50), kRed));
    CHECK(isNear(pixel(out, 150, 100), kRed));
    CHECK(isNear(pixel(out, 100, 100), kWhite));
    CHECK(isNear(pixel(out, 50, 150), kWhite));

    (*rectOf(a)) = {50, 50, 100, 100}; // 縮成一半
    out = f.flat(whiteImage(200, 200), {a});
    CHECK(isNear(pixel(out, 75, 50), kRed));
    CHECK(isNear(pixel(out, 100, 75), kRed));
    CHECK(isNear(pixel(out, 150, 100), kWhite)); // 原本的位置沒有線了

    Annotation dot = makeFreehand({{100, 100}}, redStroke(6)); // 18 像素
    dot.id = 2;
    out = f.flat(whiteImage(200, 200), {dot});
    CHECK(isNear(pixel(out, 100, 100), kRed));
    CHECK(isNear(pixel(out, 105, 100), kRed));
    CHECK(isNear(pixel(out, 115, 100), kWhite));
}

TEST_CASE("flatten：對話框的本體與尾巴") {
    RenderFixture f;
    Annotation a = shape(Kind::Bubble, {150, 150}, redStroke(2));
    a.stroke.fill = kBlue;
    Image out = f.flat(whiteImage(300, 300), {a});
    CHECK(isNear(pixel(out, 150, 150), kBlue));
    Pt ra, rb, tip;
    bubbleTail(*a.as<BubbleShape>(), ra, rb, tip);
    Pt root = lerp(ra, rb, 0.5f);
    Pt inTail = lerp(root, tip, 0.5f); // 尾巴三角形的中間
    CHECK(isNear(pixel(out, static_cast<int>(inTail.x), static_cast<int>(inTail.y)), kBlue));
    Pt nearTip = lerp(root, tip, 0.9f);
    CHECK_FALSE(isNear(pixel(out, static_cast<int>(nearTip.x), static_cast<int>(nearTip.y)), kWhite));
    CHECK(isNear(pixel(out, 250, 260), kWhite)); // 另一側沒有尾巴
    CHECK(isNear(pixel(out, 150, 90), kRed));    // 橢圓的上緣是框線
}

TEST_CASE("flatten：星形的凹處是空的，正多邊形同一個位置在形狀裡") {
    RenderFixture f;
    Annotation star = shape(Kind::Star, {100, 100}, redStroke(2));
    star.stroke.fill = kBlue;
    Image out = f.flat(whiteImage(200, 200), {star});
    CHECK(isNear(pixel(out, 100, 100), kBlue));
    auto pts = starPoints(*star.as<StarShape>());
    REQUIRE(pts.size() == 10);
    Pt c = rectOf(star)->center();
    for (size_t i = 0; i < pts.size(); i += 2) { // 外側的五個頂點：往中心縮一點就在形狀裡
        Pt p = lerp(pts[i], c, 0.25f);
        CAPTURE(i);
        CHECK(isNear(pixel(out, static_cast<int>(p.x), static_cast<int>(p.y)), kBlue));
    }
    Pt notch = lerp(pts[0], pts[2], 0.5f); // 兩個外側頂點的中間：星形的凹處，在形狀外
    CHECK(isNear(pixel(out, static_cast<int>(notch.x), static_cast<int>(notch.y)), kWhite));

    Annotation poly = shape(Kind::Polygon, {100, 100}, redStroke(2));
    poly.as<PolygonShape>()->pointCount = 5;
    poly.stroke.fill = kBlue;
    out = f.flat(whiteImage(200, 200), {poly});
    Pt inside = lerp(notch, c, 0.2f);
    CHECK(isNear(pixel(out, static_cast<int>(inside.x), static_cast<int>(inside.y)), kBlue));
    CHECK(isNear(pixel(out, 100, 100), kBlue));
    CHECK(isNear(pixel(out, 45, 45), kWhite)); // 外框的角落在五邊形外
}

TEST_CASE("flatten：放大鏡把圓內的底圖放大，圓外不動") {
    RenderFixture f;
    // 灰底，x 在 [100, 110) 是一條綠色直條。放大 2 倍後直條在 [100, 120)。
    auto base = std::make_shared<Image>(200, 200);
    const Color gray = {128, 128, 128, 255}, green = {0, 200, 0, 255};
    for (int y = 0; y < 200; ++y)
        for (int x = 0; x < 200; ++x) setPixel(*base, x, y, x >= 100 && x < 110 ? green : gray);
    Annotation a = shape(Kind::Magnifier, {100, 100}, redStroke(2)); // 半徑 60、zoom 2
    REQUIRE(a.as<MagnifierShape>()->radius == 60.0f);
    REQUIRE(a.as<MagnifierShape>()->zoom == 2.0f);
    Image out = f.flat(base, {a});
    CHECK(isNear(pixel(out, 105, 100), green));
    CHECK(isNear(pixel(out, 115, 100), green)); // 底圖這裡是灰的
    CHECK(isNear(pixel(out, 125, 100), gray));
    CHECK(isNear(pixel(out, 95, 100), gray));
    CHECK(isNear(pixel(out, 115, 130), green)); // 圓內的其他列也一樣
    CHECK(isNear(pixel(out, 160, 100), kRed));  // 圓框
    CHECK(isNear(pixel(out, 115, 170), gray));  // 圓外：直條回到原本的位置
    CHECK(isNear(pixel(out, 105, 170), green));

    a.stroke.stroke = Color::noColor(); // 沒有顏色時用預設的深灰框
    out = f.flat(base, {a});
    CHECK(isNear(pixel(out, 160, 100), Color::rgb(80, 80, 80)));
}

TEST_CASE("flatten：馬賽克把矩形裡的底圖變成一格一格，矩形外不動") {
    RenderFixture f;
    auto base = gradientImage(200, 200);
    Annotation a = shape(Kind::Mosaic, {100, 100}, StrokeStyle{}); // 40..160
    a.as<MosaicShape>()->blockSize = 16;
    Image out = f.flat(base, {a});
    // 同一格裡顏色一樣
    CHECK(pixel(out, 41, 41) == pixel(out, 55, 55));
    CHECK(pixel(out, 57, 41) == pixel(out, 71, 55));
    // 相鄰的格子顏色不同（底圖是漸層）
    CHECK(pixel(out, 41, 41) != pixel(out, 57, 41));
    CHECK(pixel(out, 41, 41) != pixel(out, 41, 57));
    // 格子的顏色是那一格底圖的平均：介於那一格的兩個角之間
    Color lo = pixel(*base, 40, 40), hi = pixel(*base, 55, 55), got = pixel(out, 48, 48);
    CHECK(got.r >= lo.r);
    CHECK(got.r <= hi.r);
    CHECK(got.g >= lo.g);
    CHECK(got.g <= hi.g);
    // 矩形外逐像素和底圖相同
    int changedOutside = 0;
    for (int y = 0; y < 200; ++y)
        for (int x = 0; x < 200; ++x)
            if ((x < 40 || x >= 160 || y < 40 || y >= 160) && pixel(out, x, y) != pixel(*base, x, y)) ++changedOutside;
    CHECK(changedOutside == 0);

    // 格子大小不同，結果不同
    a.as<MosaicShape>()->blockSize = 32;
    Image out32 = f.flat(base, {a});
    CHECK(pixel(out32, 41, 41) == pixel(out32, 70, 70));
    CHECK(pixel(out, 41, 41) != pixel(out, 70, 70));
}

TEST_CASE("flatten：馬賽克跨在圖的邊界上只處理圖內的部分") {
    RenderFixture f;
    auto base = gradientImage(100, 100);
    Annotation a = shape(Kind::Mosaic, {0, 0}, StrokeStyle{}); // -60..60
    Image out = f.flat(base, {a});
    CHECK(out.width == 100);
    CHECK(pixel(out, 1, 1) == pixel(out, 14, 14));
    CHECK(pixel(out, 80, 80) == pixel(*base, 80, 80));
    // 整個在圖外：不畫，也不出錯
    (*rectOf(a)) = {300, 300, 400, 400};
    out = f.flat(base, {a});
    CHECK(out.pixels == base->pixels);
}

TEST_CASE("flatten：文字畫在方塊裡，方塊外不動；背景色與外框") {
    RenderFixture f;
    TextStyle ts;
    ts.size = 32;
    Annotation a = makeText({150, 100}, 600, StrokeStyle{}, ts, L"Hello");
    a.id = 1;
    rectOf(a)->bottom = rectOf(a)->top + f.textHeight(a);
    Image out = f.flat(whiteImage(300, 200), {a});
    int inkInside = 0, changedOutside = 0;
    for (int y = 0; y < 200; ++y)
        for (int x = 0; x < 300; ++x) {
            bool inside = x >= rectOf(a)->left && x < rectOf(a)->right && y >= rectOf(a)->top && y < rectOf(a)->bottom;
            Color c = pixel(out, x, y);
            if (inside && c.r < 80 && c.g < 80 && c.b < 80) ++inkInside;
            if (!inside && c != kWhite) ++changedOutside;
        }
    CHECK(inkInside > 100); // 有黑色的字
    CHECK(changedOutside == 0);

    a.stroke.fill = kBlue;
    a.as<TextShape>()->style.color = kWhite;
    out = f.flat(whiteImage(300, 200), {a});
    CHECK(isNear(pixel(out, static_cast<int>(rectOf(a)->left) + 1, static_cast<int>(rectOf(a)->top) + 1), kBlue));
    CHECK(countNear(out, kWhite, (*rectOf(a)), kInkTol) > 100); // 白色的字畫在藍底上

    a.stroke.stroke = kRed;
    a.stroke.width = strokeWidthForLevel(4);
    out = f.flat(whiteImage(300, 200), {a});
    CHECK(isNear(pixel(out, static_cast<int>(rectOf(a)->left), 100), kRed));

    // 空字串：只有背景
    a.as<TextShape>()->value.clear();
    a.stroke.stroke = Color::noColor();
    out = f.flat(whiteImage(300, 200), {a});
    CHECK(countNear(out, kWhite, (*rectOf(a)), kInkTol) == 0);
}

static std::wstring kindOf(const FlattenResult& r) { return r.kind ? r.kind : L""; }

// 一個有文字的文字方塊（有外框、有填色），高度照內容量好。
static Annotation boxedText(RenderFixture& f, const StrokeStyle& st, uint32_t id) {
    TextStyle ts;
    ts.size = 28;
    Annotation tx = makeText({150, 100}, 400, st, ts, L"Hi");
    tx.stroke.stroke = st.stroke; // makeText 預設不帶外框
    tx.id = id;
    rectOf(tx)->bottom = rectOf(tx)->top + f.textHeight(tx);
    return tx;
}

TEST_CASE("flatten：故障開關打開的那一類資源建不出來 → 用到它的標註回報失敗；沒用到的照常畫，像素和沒開關時一樣") {
    // v0.9.9：要的資源建不出來時那個標註直接略過，flatten 仍回報成功，存檔就把少了標註的圖寫進原檔。
    RenderFixture f;
    StrokeStyle st = redStroke();
    st.fill = kBlue;
    st.arrows = ArrowMode::Both;
    std::vector<Annotation> items;
    for (Kind k : {Kind::Line, Kind::Rect, Kind::RoundRect, Kind::Ellipse, Kind::Bubble, Kind::Star, Kind::Polygon, Kind::Magnifier,
                   Kind::Mosaic})
        items.push_back(shape(k, {150, 100}, st));
    Annotation fh = makeFreehand({{100, 80}, {150, 120}, {200, 90}}, st);
    fh.id = 1;
    items.push_back(fh);
    items.push_back(boxedText(f, st, 1));
    REQUIRE(items.size() == 11); // 每一種都有

    using Flag = bool RenderDevice::TestFaults::*;
    struct Fault {
        const char* name;
        Flag flag;
    };
    const Fault faults[] = {{"geometry", &RenderDevice::TestFaults::geometry},
                            {"brush", &RenderDevice::TestFaults::brush},
                            {"text", &RenderDevice::TestFaults::text},
                            {"bitmap", &RenderDevice::TestFaults::bitmap}};
    // 每種物件用到哪些資源：馬賽克只用點陣圖；文字排版只有文字用；其他都要幾何與畫刷（文字的邊框也是）。
    auto uses = [](Kind k, Flag flag) {
        if (flag == &RenderDevice::TestFaults::bitmap) return k == Kind::Mosaic;
        if (flag == &RenderDevice::TestFaults::text) return k == Kind::Text;
        return k != Kind::Mosaic;
    };
    auto base = gradientImage(300, 200);
    RenderDevice& dev = f.r.device();
    for (const Annotation& a : items) {
        const Kind k = a.kind();
        DocState doc;
        doc.image = base;
        doc.items = {a};
        dev.testFaults = {};
        Image clean;
        REQUIRE(flatten(dev, doc, clean));
        REQUIRE(clean.pixels != base->pixels); // 每一個都真的有畫東西
        for (const Fault& fault : faults) {
            CAPTURE(static_cast<int>(k), fault.name);
            dev.testFaults = {};
            dev.testFaults.*fault.flag = true;
            Image out;
            FlattenResult r = flatten(dev, doc, out);
            if (uses(k, fault.flag)) {
                CHECK_FALSE(r.ok);
                CHECK(kindOf(r) == kindName(k));
                CHECK(r.hr == E_OUTOFMEMORY);
                CHECK(out.empty());
            } else {
                CHECK(r.ok);
                CHECK(out.pixels == clean.pixels);
            }
        }
    }
    dev.testFaults = {};
}

TEST_CASE("flatten：好幾個標註裡有一個畫不出來 → 整個失敗，回報的是那一個的種類") {
    RenderFixture f;
    Annotation rect = shape(Kind::Rect, {150, 100}, redStroke());
    rect.id = 1;
    Annotation mosaic = shape(Kind::Mosaic, {150, 100}, StrokeStyle{});
    mosaic.id = 2;
    DocState doc;
    doc.image = gradientImage(300, 200);
    doc.items = {rect, mosaic, boxedText(f, redStroke(), 3)};
    f.r.device().testFaults.bitmap = true;
    Image out;
    FlattenResult r = flatten(f.r.device(), doc, out);
    f.r.device().testFaults = {};
    CHECK_FALSE(r.ok);
    CHECK(kindOf(r) == L"Mosaic");
    CHECK(r.hr == E_OUTOFMEMORY);
    CHECK(out.empty());
    CHECK(flatten(f.r.device(), doc, out)); // 開關關掉就好了
}

TEST_CASE("flatten：馬賽克整個在圖外不算失敗（本來就沒東西可畫）") {
    RenderFixture f;
    Annotation m = shape(Kind::Mosaic, {150, 100}, StrokeStyle{});
    *rectOf(m) = {400, 300, 460, 360};
    auto base = gradientImage(300, 200);
    DocState doc;
    doc.image = base;
    doc.items = {m};
    Image out;
    FlattenResult r = flatten(f.r.device(), doc, out);
    CHECK(r.ok);
    CHECK(r.hr == S_OK);
    CHECK(out.pixels == base->pixels);
}

TEST_CASE("drawAnnotations：有畫不出來的照樣畫完其他的，回第一個失敗與它的 id") {
    RenderFixture f;
    auto base = whiteImage(300, 200);
    Annotation rect = shape(Kind::Rect, {150, 100}, redStroke());
    rect.id = 8;
    const std::vector<Annotation> items = {boxedText(f, StrokeStyle{}, 7), rect, boxedText(f, StrokeStyle{}, 9)};
    HRESULT hr = S_OK;
    uint32_t failedId = 0;
    f.r.device().testFaults.text = true;
    Image out = f.frameOn(base, [&](ID2D1RenderTarget*) {
        PaintCtx ctx = f.r.paintCtx(base, f.r.imageBitmap(base));
        drawImage(ctx);
        hr = drawAnnotations(ctx, items, 0, &failedId);
    });
    f.r.device().testFaults = {};
    CHECK(hr == E_OUTOFMEMORY);
    CHECK(failedId == 7);
    CHECK(countNear(out, kRed, *rectOf(rect)) > 100); // 失敗的之後那個照樣畫了

    failedId = 0;
    f.frameOn(base, [&](ID2D1RenderTarget*) {
        PaintCtx ctx = f.r.paintCtx(base, f.r.imageBitmap(base));
        hr = drawAnnotations(ctx, items, 0, &failedId);
    });
    CHECK(hr == S_OK);
    CHECK(failedId == 0);
}

TEST_CASE("flatten：文字的粗體、底線、對齊") {
    RenderFixture f;
    TextStyle ts;
    ts.size = 28;
    Annotation a = makeText({200, 100}, 1500, StrokeStyle{}, ts, L"Hill"); // 方塊寬 600，字只佔左邊一小段
    a.id = 1;
    (*rectOf(a)) = {50, 60, 350, 0};
    rectOf(a)->bottom = rectOf(a)->top + f.textHeight(a);
    const Color black = {0, 0, 0, 255};
    auto ink = [&](const Annotation& t, Rc area) { return countNear(f.flat(whiteImage(400, 200), {t}), black, area, kInkTol); };
    Rc leftHalf = {50, 60, 200, rectOf(a)->bottom}, rightHalf = {200, 60, 350, rectOf(a)->bottom};

    int plain = ink(a, (*rectOf(a)));
    REQUIRE(plain > 50);
    CHECK(ink(a, leftHalf) == plain); // 靠左：字都在左半
    CHECK(ink(a, rightHalf) == 0);

    Annotation right = a;
    right.as<TextShape>()->style.align = TextAlign::Right;
    CHECK(ink(right, leftHalf) == 0);
    CHECK(ink(right, rightHalf) > 50);

    Annotation center = a;
    center.as<TextShape>()->style.align = TextAlign::Center;
    CHECK(ink(center, {50, 60, 150, rectOf(a)->bottom}) == 0);
    CHECK(ink(center, {150, 60, 250, rectOf(a)->bottom}) > 50);
    CHECK(ink(center, {250, 60, 350, rectOf(a)->bottom}) == 0);

    Annotation bold = a;
    bold.as<TextShape>()->style.bold = true;
    CHECK(ink(bold, (*rectOf(a))) > plain);

    Annotation under = a;
    under.as<TextShape>()->style.underline = true;
    CHECK(ink(under, (*rectOf(a))) > plain);

    Annotation big = a;
    big.as<TextShape>()->style.size = 56;
    rectOf(big)->bottom = rectOf(big)->top + f.textHeight(big);
    CHECK(ink(big, (*rectOf(big))) > plain * 2);
}

TEST_CASE("textHeight：行數、換行寬度、字級") {
    RenderFixture f;
    TextStyle ts;
    ts.size = 24;
    Annotation a = makeText({200, 100}, 1500, StrokeStyle{}, ts, L"one");
    (*rectOf(a)) = {0, 0, 400, 0};
    float one = f.textHeight(a);
    CHECK(one > 24.0f);
    CHECK(one < 24.0f * 2.5f);

    Annotation two = a;
    two.as<TextShape>()->value = L"one\ntwo";
    float h2 = f.textHeight(two);
    CHECK(h2 > one * 1.5f);

    Annotation narrow = a; // 一行放不下就換行
    narrow.as<TextShape>()->value = L"aaaa bbbb cccc dddd eeee ffff gggg hhhh";
    float wide = f.textHeight(narrow);
    rectOf(narrow)->right = 120;
    CHECK(f.textHeight(narrow) > wide * 1.5f);

    Annotation big = a;
    big.as<TextShape>()->style.size = 48;
    CHECK(f.textHeight(big) > one * 1.6f);

    Annotation empty = a; // 空的方塊仍有一行高
    empty.as<TextShape>()->value.clear();
    CHECK(f.textHeight(empty) == one);
}

TEST_CASE("flatten：後面的物件畫在前面的物件上面") {
    RenderFixture f;
    Annotation under = shape(Kind::Rect, {100, 100}, redStroke());
    under.stroke.fill = kRed;
    Annotation over = shape(Kind::Rect, {120, 120}, redStroke());
    over.id = 2;
    over.stroke.stroke = kBlue;
    over.stroke.fill = kBlue;
    Image out = f.flat(whiteImage(200, 200), {under, over});
    CHECK(isNear(pixel(out, 120, 120), kBlue)); // 重疊處是後面那個
    CHECK(isNear(pixel(out, 50, 50), kRed));
    out = f.flat(whiteImage(200, 200), {over, under});
    CHECK(isNear(pixel(out, 120, 120), kRed));
    CHECK(isNear(pixel(out, 170, 170), kBlue));
}

TEST_CASE("flatten：透明的底圖，沒畫到的地方仍是透明") {
    RenderFixture f;
    auto base = std::make_shared<Image>(200, 200); // 全 0
    Annotation a = shape(Kind::Rect, {100, 100}, redStroke());
    Image out = f.flat(base, {a});
    CHECK(pixel(out, 100, 100).a == 0);
    CHECK(pixel(out, 10, 10).a == 0);
    CHECK(isNear(pixel(out, 40, 100), kRed));
    // 半透明的填色：輸出是 premultiplied（顏色已經乘過 alpha）
    a.stroke.fill = {0, 0, 255, 128};
    out = f.flat(base, {a});
    Color c = pixel(out, 100, 100);
    CHECK(std::abs(c.a - 128) <= 2);
    CHECK(std::abs(c.b - 128) <= 2);
    CHECK(c.r == 0);
}

TEST_CASE("flatten：輸出和底圖一樣大，底圖本身不被改動") {
    RenderFixture f;
    auto base = whiteImage(120, 80);
    std::vector<uint8_t> before = base->pixels;
    Image out = f.flat(base, {shape(Kind::Rect, {60, 40}, redStroke())});
    CHECK(out.width == 120);
    CHECK(out.height == 80);
    CHECK(base->pixels == before);
    CHECK(out.pixels != before);
}

TEST_CASE("drawAnnotations：skipId 的物件不畫（文字編輯中由 EDIT 控制項顯示）") {
    RenderFixture f;
    Annotation a = shape(Kind::Rect, {30, 30}, redStroke());
    (*rectOf(a)) = {10, 10, 50, 50};
    a.stroke.fill = kRed;
    Annotation b = a;
    b.id = 2;
    (*rectOf(b)) = {60, 10, 100, 50};
    b.stroke.stroke = kBlue;
    b.stroke.fill = kBlue;
    Image out = f.drawItems(whiteImage(128, 64), {a, b}, 2);
    CHECK(isNear(pixel(out, 30, 30), kRed));
    CHECK(isNear(pixel(out, 80, 30), kWhite));
    out = f.drawItems(whiteImage(128, 64), {a, b}, 0);
    CHECK(isNear(pixel(out, 80, 30), kBlue));
}

TEST_CASE("UI 文字：量到的寬高、畫在指定的矩形裡") {
    RenderFixture f;
    float h1 = 0, h2 = 0;
    float w1 = f.measureUiText(L"Crop", 13, false, 1e6f, &h1);
    float w2 = f.measureUiText(L"CropCrop", 13, false, 1e6f, &h2);
    CHECK(w1 > 10.0f);
    CHECK(w2 > w1 * 1.8f);
    CHECK(h1 > 10.0f);
    CHECK(h2 == h1);
    CHECK(f.measureUiText(L"Crop", 26) > w1 * 1.7f);       // 字級
    CHECK(f.measureUiText(L"Crop", 13, true) >= w1);        // 粗體不會比較窄
    CHECK(f.measureUiText(L"", 13) == 0.0f);
    float hWrap = 0; // 給了寬度就換行：高度變多
    f.measureUiText(L"aaaa bbbb cccc dddd eeee ffff", 13, false, 60.0f, &hWrap);
    CHECK(hWrap > h1 * 2.5f);

    const Color black = {0, 0, 0, 255};
    Rc box = {20, 10, 120, 40};
    // 靠左：字在矩形左邊；置中：左右兩端都沒有字
    Image left = f.drawUi(whiteImage(160, 64), L"Hi", box, 16, black, DWRITE_TEXT_ALIGNMENT_LEADING);
    CHECK(countNear(left, black, {20, 10, 60, 40}, kInkTol) > 10);
    CHECK(countNear(left, black, {80, 10, 120, 40}, kInkTol) == 0);
    Image mid = f.drawUi(whiteImage(160, 64), L"Hi", box, 16, black, DWRITE_TEXT_ALIGNMENT_CENTER);
    CHECK(countNear(mid, black, {20, 10, 50, 40}, kInkTol) == 0);
    CHECK(countNear(mid, black, {50, 10, 90, 40}, kInkTol) > 10);
    CHECK(countNear(mid, black, {90, 10, 120, 40}, kInkTol) == 0);
    Image right = f.drawUi(whiteImage(160, 64), L"Hi", box, 16, black, DWRITE_TEXT_ALIGNMENT_TRAILING);
    CHECK(countNear(right, black, {20, 10, 80, 40}, kInkTol) == 0);
    CHECK(countNear(right, black, {80, 10, 120, 40}, kInkTol) > 10);
    // 預設會裁在矩形內：太長的字不會畫到矩形外
    Image clipped = f.drawUi(whiteImage(160, 64), L"WWWWWWWWWWWWWWWWWWWWWWWW", box, 16, black, DWRITE_TEXT_ALIGNMENT_LEADING);
    CHECK(countNear(clipped, black, {20, 10, 120, 40}, kInkTol) > 50);
    CHECK(countNear(clipped, black, {121, 0, 160, 64}, kInkTol) == 0);
    CHECK(countNear(clipped, black, {0, 0, 19, 64}, kInkTol) == 0);
}

TEST_CASE("UI 文字：先後用不同的對齊與換行，互不影響") {
    // 以前文字格式是共用的可變物件：資訊面板拿出去改成換行，要靠下一次 drawUiText 改回來。
    RenderFixture f;
    const Color black = {0, 0, 0, 255};
    Rc box = {10, 10, 150, 54};
    const wchar_t* longText = L"aaaa bbbb cccc dddd eeee ffff gggg hhhh";
    Image noWrapFirst = f.drawUi(whiteImage(160, 64), longText, box, 12, black, DWRITE_TEXT_ALIGNMENT_LEADING, false);
    Image wrapped = f.drawUi(whiteImage(160, 64), longText, box, 12, black, DWRITE_TEXT_ALIGNMENT_LEADING, true);
    Image noWrapAgain = f.drawUi(whiteImage(160, 64), longText, box, 12, black, DWRITE_TEXT_ALIGNMENT_LEADING, false);
    CHECK(noWrapFirst.pixels == noWrapAgain.pixels);
    CHECK(wrapped.pixels != noWrapFirst.pixels);
    // 換行的版本字比較多行：墨水比較多
    CHECK(countNear(wrapped, black, box, kInkTol) > countNear(noWrapFirst, black, box, kInkTol));
}

TEST_CASE("flatten：輸出的圖配置不到記憶體時回報失敗（E_OUTOFMEMORY），不丟例外、不交出圖") {
    // v0.9.9：flatten 裡配置輸出的 std::bad_alloc 一路逃到存檔的訊息處理，沒有人接。
    RenderFixture f;
    auto base = gradientImage(300, 200);
    DocState doc;
    doc.image = base;
    doc.items = {shape(Kind::Rect, {150, 100}, redStroke())};
    Image out;
    FlattenResult r;
    {
        mp::test::ImageAllocLimit limit(1);
        CHECK_NOTHROW(r = flatten(f.r.device(), doc, out));
    }
    CHECK_FALSE(r.ok);
    CHECK(r.kind == nullptr); // 不是某個標註的問題
    CHECK(r.hr == E_OUTOFMEMORY);
    CHECK(out.empty());
    CHECK(flatten(f.r.device(), doc, out)); // 配置得到了就照常
    CHECK(out.width == 300);
}
