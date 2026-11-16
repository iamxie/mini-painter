#include <catch2/catch_test_macros.hpp>

#include "app/Toolbar.h"
#include "core/Lang.h"
#include "render_fixture.h"
#include "test_util.h"

using namespace mp;
using namespace mp::test;

// 工具列的排版、每顆按鈕的狀態、命中與繪製（不必有視窗）。

namespace {

// 視窗寬 800：主工具列 y 0～44，標示工具列 y 44～84。
const D2D1_RECT_F kBar = D2D1::RectF(0, 0, 800, 44), kMarkup = D2D1::RectF(0, 44, 800, 84);

ToolbarState withImage() {
    ToolbarState st;
    st.hasImage = true;
    st.commands.hasImage = true;
    st.commands.hasPages = true;
    return st;
}

struct Fixture {
    RenderFixture f;
    Toolbar bar;
    void layout(const ToolbarState& st, float ui = 1.0f) {
        const D2D1_RECT_F bar1 = D2D1::RectF(0, 0, 800 * ui, 44 * ui), bar2 = D2D1::RectF(0, 44 * ui, 800 * ui, 84 * ui);
        bar.layout(bar1, bar2, st, UiScale{ui}, f.r.device());
    }
    const Button& button(int id) {
        const Button* b = bar.find(id);
        REQUIRE(b != nullptr);
        return *b;
    }
};

POINT centerOf(const Button& b) { return {static_cast<LONG>((b.rect.left + b.rect.right) / 2), static_cast<LONG>((b.rect.top + b.rect.bottom) / 2)}; }

} // namespace

TEST_CASE("Toolbar：主工具列的位置。左邊從 10 開始、每顆 36×30、間隔 4，右邊兩顆靠右") {
    Fixture t;
    t.layout(withImage());
    REQUIRE(t.bar.buttons().size() == 6);
    const Button& zoomOut = t.button(BTN_ZOOMOUT);
    CHECK(zoomOut.rect.left == 10);
    CHECK(zoomOut.rect.top == 7); // (44 - 30) / 2
    CHECK(zoomOut.rect.right == 46);
    CHECK(zoomOut.rect.bottom == 37);
    CHECK(t.button(BTN_ZOOM100).rect.left == 50);
    CHECK(t.button(BTN_ZOOMIN).rect.left == 90);
    CHECK(t.button(BTN_ROTATE).rect.left == 142); // 多空一組的間隔
    CHECK(t.button(BTN_INFO).rect.right == 790);
    CHECK(t.button(BTN_MARKUP).rect.right == 750);
    CHECK(t.bar.find(BTN_FREEHAND) == nullptr); // 標示工具列沒開
}

TEST_CASE("Toolbar：標示工具列開著時多一列；裁切模式才有「裁切」文字按鈕") {
    Fixture t;
    ToolbarState st = withImage();
    st.showMarkup = true;
    t.layout(st);
    CHECK(t.bar.buttons().size() == 16);
    CHECK(t.button(BTN_FREEHAND).rect.left == 10);
    CHECK(t.button(BTN_FREEHAND).rect.top == 49); // 44 + (40 - 30) / 2
    CHECK(t.button(BTN_SHAPES).dropdown);
    CHECK_FALSE(t.button(BTN_TEXT).dropdown);
    CHECK(t.button(BTN_ADJCOLOR).rect.left == 142);
    CHECK(t.button(BTN_CROP).rect.left == 222);
    CHECK(t.button(BTN_LINESTYLE).rect.left == 274);
    CHECK(t.button(BTN_FORMAT).rect.left == 394);
    CHECK(t.button(BTN_MARKUP).pressed);
    CHECK(t.bar.find(BTN_CROP_APPLY) == nullptr);

    st.cropping = true;
    t.layout(st);
    const Button& apply = t.button(BTN_CROP_APPLY);
    CHECK(apply.rect.right == 790);
    CHECK(apply.rect.right - apply.rect.left > 28); // 字寬加留白
    CHECK(t.button(BTN_CROP).pressed);
}

TEST_CASE("Toolbar：每顆按鈕能不能按、有沒有亮起跟著狀態") {
    Fixture t;
    ToolbarState st; // 沒有圖
    st.showMarkup = true;
    t.layout(st);
    CHECK_FALSE(t.button(BTN_ZOOMIN).enabled);   // 指令的條件：沒有圖不能縮放
    CHECK_FALSE(t.button(BTN_FREEHAND).enabled); // 工具按鈕：沒有圖不能用
    CHECK(t.button(BTN_MARKUP).enabled);         // 工具列的開關隨時能按
    CHECK(t.button(BTN_INFO).enabled);
    CHECK_FALSE(t.button(BTN_INFO).pressed);

    st = withImage();
    st.showMarkup = st.showInfo = true;
    st.freehand = st.adjusting = true;
    t.layout(st);
    CHECK(t.button(BTN_ZOOMIN).enabled);
    CHECK(t.button(BTN_FREEHAND).enabled);
    CHECK(t.button(BTN_FREEHAND).pressed);
    CHECK(t.button(BTN_ADJCOLOR).pressed);
    CHECK(t.button(BTN_INFO).pressed);
    CHECK_FALSE(t.button(BTN_CROP).pressed);
    CHECK(t.button(BTN_ZOOM100).enabled);

    st.zoomIsActual = true; // 已經是 1:1
    t.layout(st);
    CHECK_FALSE(t.button(BTN_ZOOM100).enabled);

    st.commands.dragging = true; // 拖曳中指令都不能用，但按鈕不跟著變灰
    t.layout(st);
    CHECK(t.button(BTN_ROTATE).enabled);
}

TEST_CASE("Toolbar：命中。按鈕上回它的 id，空隙回 BTN_NONE") {
    Fixture t;
    ToolbarState st = withImage();
    st.showMarkup = true;
    t.layout(st);
    for (const Button& b : t.bar.buttons()) {
        const Button* got = nullptr;
        CHECK(t.bar.hit(centerOf(b), &got) == b.id);
        CHECK(got == &b);
    }
    CHECK(t.bar.hit({48, 20}) == BTN_NONE);  // 兩顆之間
    CHECK(t.bar.hit({400, 20}) == BTN_NONE); // 中間的空白
    CHECK(t.bar.hit({20, 3}) == BTN_NONE);   // 按鈕上方
    CHECK(t.bar.hit({46, 20}) == BTN_NONE);  // 右邊界不算在內
    CHECK(t.bar.hit({10, 7}) == BTN_ZOOMOUT);
}

TEST_CASE("Toolbar：DPI 倍率下位置與大小等比例") {
    Fixture t;
    t.layout(withImage(), 1.5f);
    const Button& zoomOut = t.button(BTN_ZOOMOUT);
    CHECK(zoomOut.rect.left == 15);
    CHECK(zoomOut.rect.right - zoomOut.rect.left == 54);
    CHECK(zoomOut.rect.bottom - zoomOut.rect.top == 45);
    CHECK(t.button(BTN_INFO).rect.right == 1200 - 15);
}

TEST_CASE("Toolbar::name：圖示按鈕都有名稱，跟著語系；文字按鈕沒有") {
    for (int id = BTN_NONE + 1; id < BTN_CROP_APPLY; ++id) CHECK(*Toolbar::name(id) != 0);
    CHECK(*Toolbar::name(BTN_CROP_APPLY) == 0);
    CHECK(*Toolbar::name(BTN_NONE) == 0);
    CHECK(std::wstring(Toolbar::name(BTN_ROTATE)) == L"Rotate 90° Counterclockwise");
    LangGuard guard;
    setActiveLang(parseLangText(L"_code=zh-TW\nShapes=形狀\n"));
    CHECK(std::wstring(Toolbar::name(BTN_SHAPES)) == L"形狀");
}

TEST_CASE("Toolbar：畫出來。亮起的按鈕有底色、hover 的有灰底、停用的圖示比較淡，都不超出工具列") {
    Fixture t;
    ToolbarState st = withImage();
    st.showMarkup = true;
    st.stroke = Color::rgb(255, 0, 0);
    st.fill = Color::rgb(0, 0, 255);
    t.layout(st);
    t.bar.hover = BTN_ROTATE;
    auto base = whiteImage(800, 120);
    Image out = t.f.frameOn(base, [&](ID2D1RenderTarget*) {
        drawImage(t.f.r.paintCtx(base, t.f.r.imageBitmap(base)));
        t.bar.draw(t.f.r.surface(), UiScale{1.0f});
    });
    const Color selected{199, 217, 242, 255}, hoverBg{222, 222, 222, 255}; // theme::kSelectedBg、kBtnHover
    const Button& markup = t.button(BTN_MARKUP);
    CHECK(isNear(pixel(out, static_cast<int>(markup.rect.left) + 3, static_cast<int>(markup.rect.top) + 15), selected, 6));
    const Button& rotate = t.button(BTN_ROTATE);
    CHECK(isNear(pixel(out, static_cast<int>(rotate.rect.left) + 3, static_cast<int>(rotate.rect.top) + 15), hoverBg, 6));
    const Button& zoomIn = t.button(BTN_ZOOMIN); // 沒有 hover、沒有亮起：底是白的
    CHECK(isNear(pixel(out, static_cast<int>(zoomIn.rect.left) + 2, static_cast<int>(zoomIn.rect.top) + 2), {255, 255, 255, 255}, 4));
    // 線條顏色與填色兩顆顯示給的顏色
    const Button& line = t.button(BTN_LINECOLOR);
    CHECK(countNear(out, {255, 0, 0, 255}, {line.rect.left, line.rect.top, line.rect.right, line.rect.bottom}, 40) > 20);
    const Button& fill = t.button(BTN_FILL);
    CHECK(countNear(out, {0, 0, 255, 255}, {fill.rect.left, fill.rect.top, fill.rect.right, fill.rect.bottom}, 8) > 100);
    // 工具列下面（y >= 84）什麼都沒畫
    CHECK(countNear(out, {255, 255, 255, 255}, {0, 84, 800, 120}, 2) == 800 * 36);

    // 沒有圖：工具按鈕停用，圖示用淡的顏色
    ToolbarState none;
    none.showMarkup = true;
    t.layout(none);
    Image off = t.f.frameOn(base, [&](ID2D1RenderTarget*) {
        drawImage(t.f.r.paintCtx(base, t.f.r.imageBitmap(base)));
        t.bar.draw(t.f.r.surface(), UiScale{1.0f});
    });
    const Button& shapes = t.button(BTN_SHAPES);
    const Rc area{shapes.rect.left, shapes.rect.top, shapes.rect.right, shapes.rect.bottom};
    CHECK(countNear(off, {51, 51, 51, 255}, area, 40) == 0);    // 沒有深色的筆劃
    CHECK(countNear(off, {179, 179, 179, 255}, area, 40) > 20); // 淡的
}
