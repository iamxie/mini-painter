#include <catch2/catch_test_macros.hpp>

#include "app/ToolbarIcons.h"
#include "render_fixture.h"

using namespace mp;
using namespace mp::test;

// 工具列圖示是資料表（app/ToolbarIcons.cpp）。這裡檢查表本身，和照表畫出來的結果。

namespace {

const int kIconButtons[] = {BTN_ZOOMOUT, BTN_ZOOM100, BTN_ZOOMIN,   BTN_ROTATE,  BTN_MARKUP,    BTN_INFO,  BTN_FREEHAND,
                            BTN_SHAPES,  BTN_TEXT,    BTN_ADJCOLOR, BTN_ADJSIZE, BTN_LINESTYLE, BTN_FORMAT, BTN_CROP, BTN_BROWSE};

const Color kInk{51, 51, 51, 255}; // theme::kIcon

// 按鈕在一張 60 × 50 的白圖中央：四周各留 12、10 個像素，畫出界看得出來。
constexpr int kImgW = 60, kImgH = 50;
D2D1_RECT_F buttonRect(float ui = 1.0f) {
    return D2D1::RectF(kImgW * ui / 2 - kBtnW * ui / 2, kImgH * ui / 2 - kBtnH * ui / 2, kImgW * ui / 2 + kBtnW * ui / 2,
                       kImgH * ui / 2 + kBtnH * ui / 2);
}

template <class F>
Image render(RenderFixture& f, float ui, F draw) {
    auto base = whiteImage(static_cast<int>(kImgW * ui), static_cast<int>(kImgH * ui));
    return f.frameOn(base, [&](ID2D1RenderTarget* rt) {
        drawImage(f.r.paintCtx(base, f.r.imageBitmap(base)));
        draw(rt);
    });
}

// 不是白色的像素數：整張圖，與按鈕範圍以外。
int inked(const Image& im, Rc area) {
    int n = 0;
    for (int y = std::max(0, static_cast<int>(area.top)); y < std::min(im.height, static_cast<int>(area.bottom)); ++y)
        for (int x = std::max(0, static_cast<int>(area.left)); x < std::min(im.width, static_cast<int>(area.right)); ++x)
            if (!isNear(pixel(im, x, y), {255, 255, 255, 255}, 8)) ++n;
    return n;
}

} // namespace

TEST_CASE("toolbarIcon：圖示按鈕都有圖示；顯示目前顏色的兩顆與文字按鈕沒有固定圖示") {
    for (int id : kIconButtons) {
        INFO("button " << id);
        const auto* icon = toolbarIcon(id);
        REQUIRE(icon != nullptr);
        CHECK_FALSE(icon->empty());
    }
    for (int id : {BTN_NONE, BTN_LINECOLOR, BTN_FILL, BTN_CROP_APPLY}) CHECK(toolbarIcon(id) == nullptr);
    // 每一顆按鈕不是在表裡，就是上面那三種之一：加按鈕時漏了圖示會在這裡失敗。
    for (int id = BTN_NONE + 1; id <= BTN_CROP_APPLY; ++id) {
        const bool special = id == BTN_LINECOLOR || id == BTN_FILL || id == BTN_CROP_APPLY;
        CHECK((toolbarIcon(id) != nullptr) != special);
    }
}

TEST_CASE("toolbarIcon：表裡的座標都在按鈕範圍內，路徑的寫法合法") {
    const float hw = kBtnW / 2, hh = kBtnH / 2;
    auto inside = [&](float x, float y) { return std::abs(x) <= hw && std::abs(y) <= hh; };
    for (int id : kIconButtons) {
        INFO("button " << id);
        for (const IconPart& p : *toolbarIcon(id)) {
            switch (p.kind) {
            case IconPart::Kind::Line: CHECK((inside(p.v[0], p.v[1]) && inside(p.v[2], p.v[3]))); break;
            case IconPart::Kind::Rect: CHECK((inside(p.v[0], p.v[1]) && inside(p.v[2], p.v[3]) && p.v[0] < p.v[2] && p.v[1] < p.v[3])); break;
            case IconPart::Kind::Ellipse: CHECK((inside(p.v[0] - p.v[2], p.v[1] - p.v[3]) && inside(p.v[0] + p.v[2], p.v[1] + p.v[3]))); break;
            case IconPart::Kind::Dot: CHECK((inside(p.v[0] - p.v[2], p.v[1] - p.v[2]) && inside(p.v[0] + p.v[2], p.v[1] + p.v[2]))); break;
            case IconPart::Kind::Path: {
                REQUIRE_FALSE(p.path.empty());
                CHECK(p.path.front().kind == IconSeg::Kind::Move); // 一定從 Move 開始
                for (const IconSeg& s : p.path) {
                    if (s.kind == IconSeg::Kind::Move || s.kind == IconSeg::Kind::Line || s.kind == IconSeg::Kind::Arc)
                        CHECK(inside(s.v[0], s.v[1]));
                    if (s.kind == IconSeg::Kind::Bezier) CHECK(inside(s.v[4], s.v[5])); // 控制點可以在外面，終點不行
                    if (s.kind == IconSeg::Kind::Arc) CHECK(s.v[2] > 0);
                }
                break;
            }
            case IconPart::Kind::Text:
                REQUIRE(p.text != nullptr);
                CHECK(p.v[0] >= 8); // 字級
                break;
            }
            if (p.kind != IconPart::Kind::Dot && p.kind != IconPart::Kind::Text) CHECK(p.width > 0);
        }
    }
}

TEST_CASE("drawIcon：每個圖示都畫得出東西，而且不超出按鈕") {
    RenderFixture f;
    for (float ui : {1.0f, 1.5f}) {
        for (int id : kIconButtons) {
            INFO("button " << id << " ui " << ui);
            Image out = render(f, ui, [&](ID2D1RenderTarget*) { drawIcon(f.r.surface(), *toolbarIcon(id), buttonRect(ui), UiScale{ui}, toD2D(kInk)); });
            const D2D1_RECT_F b = buttonRect(ui);
            const int all = inked(out, {0, 0, kImgW * ui, kImgH * ui});
            const int inButton = inked(out, {b.left, b.top, b.right, b.bottom});
            CHECK(all > 20 * ui);    // 有畫東西
            CHECK(all == inButton);  // 都在按鈕裡
        }
    }
}

TEST_CASE("drawIcon：照表畫。放大鏡的加號比減號多一豎；裁切是兩個交錯的 L") {
    RenderFixture f;
    auto icon = [&](int id) {
        return render(f, 1.0f, [&](ID2D1RenderTarget*) { drawIcon(f.r.surface(), *toolbarIcon(id), buttonRect(), UiScale{1.0f}, toD2D(kInk)); });
    };
    const int cx = kImgW / 2, cy = kImgH / 2;
    Image minus = icon(BTN_ZOOMOUT), plus = icon(BTN_ZOOMIN);
    // 鏡片中心在 (-1.5, -1.5)：橫線兩張都有，往上 3 個像素只有加號有。
    CHECK(isNear(pixel(minus, cx - 3, cy - 2), kInk, 90));
    CHECK(isNear(pixel(plus, cx - 3, cy - 2), kInk, 90));
    CHECK(isNear(pixel(minus, cx - 2, cy - 5), {255, 255, 255, 255}, 8));
    CHECK(isNear(pixel(plus, cx - 2, cy - 5), kInk, 90));

    Image crop = icon(BTN_CROP);
    CHECK(isNear(pixel(crop, cx - 5, cy), kInk, 90));  // 第一個 L 的直線 x = -5
    CHECK(isNear(pixel(crop, cx, cy + 5), kInk, 90));  // 它的橫線 y = 5
    CHECK(isNear(pixel(crop, cx, cy - 5), kInk, 90));  // 第二個 L 的橫線 y = -5
    CHECK(isNear(pixel(crop, cx + 5, cy), kInk, 90));  // 它的直線 x = 5
    CHECK(isNear(pixel(crop, cx, cy), {255, 255, 255, 255}, 8)); // 中間是空的
    CHECK(isNear(pixel(crop, cx + 8, cy - 8), {255, 255, 255, 255}, 8)); // 兩筆沒有被連起來
}

TEST_CASE("drawLineColorIcon / drawFillIcon：顯示目前的顏色；無色時是空心加紅斜線") {
    RenderFixture f;
    const Color red{255, 0, 0, 255}, blue{0, 0, 255, 255}, slash{217, 26, 26, 255}; // theme::kNoColorSlash
    const int cx = kImgW / 2, cy = kImgH / 2;
    const Rc all{0, 0, kImgW, kImgH};

    Image ring = render(f, 1.0f, [&](ID2D1RenderTarget*) { drawLineColorIcon(f.r.surface(), buttonRect(), UiScale{1.0f}, red, toD2D(kInk)); });
    CHECK(isNear(pixel(ring, cx - 1 + 7, cy), red, 40));               // 圓圈上
    CHECK(isNear(pixel(ring, cx - 1, cy), {255, 255, 255, 255}, 8));   // 圓心是空的
    CHECK(countNear(ring, slash, all, 30) == 0);

    Image noRing = render(f, 1.0f, [&](ID2D1RenderTarget*) { drawLineColorIcon(f.r.surface(), buttonRect(), UiScale{1.0f}, Color::noColor(), toD2D(kInk)); });
    CHECK(countNear(noRing, slash, all, 60) > 5);                      // 紅斜線
    CHECK(countNear(noRing, red, all, 20) == 0);

    Image fill = render(f, 1.0f, [&](ID2D1RenderTarget*) { drawFillIcon(f.r.surface(), buttonRect(), UiScale{1.0f}, blue, toD2D(kInk)); });
    CHECK(isNear(pixel(fill, cx - 1, cy), blue, 8));                   // 方塊裡是填色
    // 外面一圈 1 像素的灰框：線壓在像素的交界上，外側那一半和白底各半混合。
    CHECK(countNear(fill, {153, 153, 153, 255}, {cx - 10.0f, cy - 9.0f, cx + 8.0f, cy + 9.0f}, 40) > 20);

    Image noFill = render(f, 1.0f, [&](ID2D1RenderTarget*) { drawFillIcon(f.r.surface(), buttonRect(), UiScale{1.0f}, Color::noColor(), toD2D(kInk)); });
    CHECK(countNear(noFill, slash, all, 60) > 5);
    CHECK(countNear(noFill, blue, all, 20) == 0);
}
