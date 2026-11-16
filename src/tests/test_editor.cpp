#include <catch2/catch_test_macros.hpp>

#include <cmath>

#include "core/Editor.h"

using namespace mp;

// Editor：編輯一張圖的狀態與互動邏輯（原本在 MainWindow 裡，沒有測試）。
// 這裡用圖片座標直接餵滑鼠事件，檢查文件、選取、工具的變化。

namespace {

// 400×300 的圖，文字高度用「每行 20、上下各留 4」的假排版。
struct Fixture {
    Editor ed;
    ImagePtr image = std::make_shared<Image>(400, 300);
    Fixture() {
        ed.setTextMeasurer([](const TextShape& t) {
            int lines = 1;
            for (wchar_t ch : t.value)
                if (ch == L'\n') ++lines;
            return 20.0f * lines + 8.0f;
        });
        ed.reset(image, Dpi{300, 150});
    }
    // 倍率 1 時的滑鼠事件。
    static Pointer at(float x, float y) {
        Pointer p;
        p.pos = {x, y};
        p.tol = 4;
        p.handleTol = 8;
        p.cropTol = 8;
        p.cropCorner = 30;
        p.drawStep = 0.5f;
        return p;
    }
    // 按下、拖到、放開。
    bool dragFromTo(Pt a, Pt b) {
        ed.press(at(a.x, a.y), false);
        ed.move(at(b.x, b.y), true);
        return ed.release();
    }
    const Annotation& item(uint32_t id) const {
        const Annotation* a = ed.doc().findItem(id);
        REQUIRE(a != nullptr);
        return *a;
    }
};

// 預設形狀的座標不是剛好的整數（0.15 不是剛好的浮點數），比到小數兩位。
bool nearly(float a, float b) { return std::abs(a - b) < 0.01f; }

} // namespace

TEST_CASE("Editor：按在空白處是平移，並取消選取；放開不改文件") {
    Fixture f;
    uint32_t id = f.ed.addShape(Kind::Rect, {100, 100}, 400);
    REQUIRE(f.ed.selected() == id);
    const size_t undoBefore = f.ed.doc().undoCount();
    CHECK(f.ed.press(Fixture::at(350, 250), false) == Editor::Press::Drag);
    CHECK(f.ed.drag() == DragKind::Pan);
    CHECK(f.ed.selected() == 0);
    f.ed.move(Fixture::at(300, 200), true);
    CHECK_FALSE(f.ed.release());
    CHECK_FALSE(f.ed.dragging());
    CHECK(f.ed.doc().undoCount() == undoBefore);
}

TEST_CASE("Editor：點物件選取並搬移；只點不拖不產生 undo 步驟") {
    Fixture f;
    uint32_t id = f.ed.addShape(Kind::Rect, {100, 100}, 400); // 40..160
    f.ed.select(0);
    const size_t undoBefore = f.ed.doc().undoCount();

    // 只點一下（沒有超過手抖的範圍）
    CHECK(f.ed.press(Fixture::at(40, 100), false) == Editor::Press::Drag);
    CHECK(f.ed.drag() == DragKind::Move);
    CHECK(f.ed.selected() == id);
    f.ed.move(Fixture::at(41, 100), false);
    CHECK_FALSE(f.ed.release());
    CHECK(f.ed.doc().undoCount() == undoBefore);
    CHECK(nearly(rectOf(f.item(id))->left, 40.0f));

    // 拖 30、20。按在物件裡面、離控制點遠的地方（按在邊的中點會拖到控制點）
    f.ed.press(Fixture::at(70, 75), false);
    CHECK(f.ed.drag() == DragKind::Move);
    f.ed.move(Fixture::at(100, 95), true);
    // 拖曳中：畫面上是拖到一半的樣子，文件還沒動
    REQUIRE(f.ed.selectedItem() != nullptr);
    CHECK(nearly(rectOf(*f.ed.selectedItem())->left, 70.0f));
    CHECK(nearly(rectOf(f.item(id))->left, 40.0f));
    CHECK(f.ed.release());
    CHECK(nearly(rectOf(f.item(id))->left, 70.0f));
    CHECK(nearly(rectOf(f.item(id))->top, 60.0f));
    CHECK(f.ed.doc().undoCount() == undoBefore + 1);
    CHECK(f.ed.visibleItems().size() == 1); // 放開後畫面用的是文件裡的

    f.ed.undo();
    CHECK(nearly(rectOf(f.item(id))->left, 40.0f));
}

TEST_CASE("Editor：上層的物件優先被點到") {
    Fixture f;
    StrokeStyle filled;
    filled.fill = Color::rgb(0, 0, 255);
    f.ed.applyStrokeStyle(filled);
    uint32_t below = f.ed.addShape(Kind::Rect, {100, 100}, 400);
    uint32_t above = f.ed.addShape(Kind::Rect, {120, 120}, 400);
    f.ed.select(0);
    f.ed.press(Fixture::at(130, 130), false); // 兩個都蓋到
    CHECK(f.ed.selected() == above);
    f.ed.release();
    f.ed.press(Fixture::at(50, 50), false); // 只有下面那個
    CHECK(f.ed.selected() == below);
    f.ed.release();
}

TEST_CASE("Editor：選取物件的控制點優先於物件本身；拖控制點改大小") {
    Fixture f;
    uint32_t id = f.ed.addShape(Kind::Rect, {100, 100}, 400); // 選取中
    // 右下角的控制點在 (160, 160)
    f.ed.press(Fixture::at(162, 161), false);
    CHECK(f.ed.drag() == DragKind::Handle);
    CHECK(f.ed.dragHandleId() == H_BR);
    f.ed.move(Fixture::at(200, 220), true);
    CHECK(f.ed.release());
    CHECK(nearly(rectOf(f.item(id))->right, 200.0f));
    CHECK(nearly(rectOf(f.item(id))->bottom, 220.0f));
    CHECK(nearly(rectOf(f.item(id))->left, 40.0f));
    // 沒有選取時同一個位置點不到控制點（200,220 這個角在框線上：變成搬移）
    f.ed.select(0);
    f.ed.press(Fixture::at(200, 220), false);
    CHECK(f.ed.drag() == DragKind::Move);
    f.ed.cancelDrag();
}

TEST_CASE("Editor：cancelDrag 丟掉拖到一半的東西") {
    Fixture f;
    uint32_t id = f.ed.addShape(Kind::Rect, {100, 100}, 400);
    const size_t undoBefore = f.ed.doc().undoCount();
    f.ed.press(Fixture::at(70, 75), false);
    REQUIRE(f.ed.drag() == DragKind::Move);
    f.ed.move(Fixture::at(120, 125), true);
    CHECK(nearly(rectOf(*f.ed.selectedItem())->left, 90.0f));
    f.ed.cancelDrag();
    CHECK_FALSE(f.ed.dragging());
    CHECK(nearly(rectOf(f.item(id))->left, 40.0f));
    CHECK(f.ed.doc().undoCount() == undoBefore);
    CHECK(f.ed.visibleItems().size() == 1);
    CHECK_FALSE(f.ed.release()); // 沒有在拖：放開什麼都不做

    // 塗鴉畫到一半
    f.ed.toggleFreehand();
    f.ed.press(Fixture::at(10, 10), false);
    f.ed.move(Fixture::at(50, 50), true);
    CHECK(f.ed.drawPath().size() == 2);
    f.ed.cancelDrag();
    CHECK(f.ed.drawPath().empty());
    CHECK(f.ed.doc().items().size() == 1);
}

TEST_CASE("Editor：塗鴉工具拖一筆就是一個物件，用目前的線條樣式；點一下是一個點") {
    Fixture f;
    StrokeStyle s;
    s.stroke = Color::rgb(0, 128, 0);
    s.width = strokeWidthForLevel(5);
    f.ed.applyStrokeStyle(s);
    uint32_t shape = f.ed.addShape(Kind::Rect, {300, 200}, 200);
    CHECK(f.ed.toggleFreehand());
    CHECK(f.ed.freehand());
    CHECK(f.ed.selected() == 0); // 開塗鴉就取消選取

    f.ed.press(Fixture::at(20, 20), false);
    CHECK(f.ed.drag() == DragKind::Draw);
    f.ed.move(Fixture::at(20.2f, 20.1f), false); // 太近：不記
    CHECK(f.ed.drawPath().size() == 1);
    f.ed.move(Fixture::at(60, 20), true);
    f.ed.move(Fixture::at(60, 80), true);
    CHECK(f.ed.release());
    REQUIRE(f.ed.doc().items().size() == 2);
    const Annotation& a = f.ed.doc().items().back();
    CHECK(a.kind() == Kind::Freehand);
    CHECK(a.as<FreehandShape>()->points.size() == 3);
    CHECK(a.stroke.stroke == Color::rgb(0, 128, 0));
    CHECK(a.stroke.width == strokeWidthForLevel(5));
    CHECK(rectOf(a)->left == 20.0f);
    CHECK(rectOf(a)->right == 60.0f);
    CHECK(rectOf(a)->bottom == 80.0f);
    CHECK(a.id != shape);
    CHECK(a.id != 0);

    // 塗鴉工具開著時點在別的物件上也是畫，不是選
    f.ed.press(Fixture::at(300, 200), false);
    CHECK(f.ed.drag() == DragKind::Draw);
    CHECK(f.ed.release()); // 只點一下：一個點
    CHECK(f.ed.doc().items().size() == 3);
    CHECK(f.ed.doc().items().back().as<FreehandShape>()->points.size() == 1);
    CHECK(f.ed.selected() == 0);

    CHECK_FALSE(f.ed.toggleFreehand());
    CHECK(f.ed.tool() == Tool::Select);
}

TEST_CASE("Editor：兩頁並排時按下去一律是平移") {
    Fixture f;
    uint32_t id = f.ed.addShape(Kind::Rect, {100, 100}, 400);
    f.ed.press(Fixture::at(40, 100), true);
    CHECK(f.ed.drag() == DragKind::Pan);
    CHECK(f.ed.selected() == id); // 選取不動
    f.ed.release();
    f.ed.toggleFreehand();
    f.ed.press(Fixture::at(40, 100), true);
    CHECK(f.ed.drag() == DragKind::Pan);
    f.ed.release();
}

TEST_CASE("Editor：裁切模式的進出與拖框") {
    Fixture f;
    uint32_t id = f.ed.addShape(Kind::Rect, {100, 100}, 400);
    f.ed.toggleFreehand();
    REQUIRE(f.ed.beginCrop());
    CHECK(f.ed.cropping());
    CHECK_FALSE(f.ed.freehand()); // 工具互斥
    CHECK(f.ed.selected() == 0);
    CHECK(f.ed.cropBox() == CropBox{0, 0, 400, 300});

    // 拖右下角
    f.ed.press(Fixture::at(398, 298), false);
    CHECK(f.ed.drag() == DragKind::Crop);
    f.ed.move(Fixture::at(298, 198), true);
    CHECK(f.ed.cropBox() == CropBox{0, 0, 300, 200});
    CHECK_FALSE(f.ed.release()); // 拖框不改文件
    CHECK(f.ed.cropBox() == CropBox{0, 0, 300, 200});

    // 框外：平移；物件點不到
    f.ed.press(Fixture::at(390, 290), false);
    CHECK(f.ed.drag() == DragKind::Pan);
    f.ed.release();
    f.ed.press(Fixture::at(40, 100), false); // 物件的框線上，也在裁切框內
    CHECK(f.ed.drag() == DragKind::Crop);
    CHECK(f.ed.selected() == 0);
    // 拖到一半取消拖曳：框回到拖之前
    f.ed.move(Fixture::at(90, 150), true);
    CHECK(f.ed.cropBox() != CropBox{0, 0, 300, 200});
    f.ed.cancelDrag();
    CHECK(f.ed.cropBox() == CropBox{0, 0, 300, 200});

    // 拖到一半離開裁切：拖曳也結束
    f.ed.press(Fixture::at(150, 100), false);
    CHECK(f.ed.cancelCrop());
    CHECK_FALSE(f.ed.dragging());
    CHECK(f.ed.tool() == Tool::Select);
    CHECK_FALSE(f.ed.cancelCrop()); // 已經不在裁切模式
    CHECK(nearly(rectOf(f.item(id))->left, 40.0f));

    Editor empty;
    CHECK_FALSE(empty.beginCrop()); // 沒有圖
}

TEST_CASE("Editor：套用裁切換掉底圖、標註跟著平移，一個 undo 步驟；框是整張圖時等於取消") {
    Fixture f;
    uint32_t id = f.ed.addShape(Kind::Rect, {100, 100}, 400);
    CHECK(f.ed.applyCrop() == Editor::CropResult::NotCropping);

    f.ed.beginCrop();
    const size_t undoBefore = f.ed.doc().undoCount();
    CHECK(f.ed.applyCrop() == Editor::CropResult::Unchanged);
    CHECK_FALSE(f.ed.cropping());
    CHECK(f.ed.doc().undoCount() == undoBefore);

    f.ed.beginCrop();
    f.ed.press(Fixture::at(1, 1), false); // 左上角
    f.ed.move(Fixture::at(21, 11), true);
    f.ed.release();
    REQUIRE(f.ed.cropBox() == CropBox{20, 10, 400, 300});
    CHECK(f.ed.applyCrop() == Editor::CropResult::Applied);
    CHECK_FALSE(f.ed.cropping());
    CHECK(f.ed.doc().image()->width == 380);
    CHECK(f.ed.doc().image()->height == 290);
    CHECK(f.ed.doc().dpi() == Dpi{300, 150});
    CHECK(nearly(rectOf(f.item(id))->left, 20.0f));
    CHECK(nearly(rectOf(f.item(id))->top, 30.0f));
    CHECK(f.ed.doc().undoCount() == undoBefore + 1);
    f.ed.undo();
    CHECK(f.ed.doc().image()->width == 400);
    CHECK(nearly(rectOf(f.item(id))->left, 40.0f));
}

TEST_CASE("Editor：Esc 一次取消一樣，順序是裁切、取白點、選取、塗鴉") {
    Fixture f;
    CHECK(f.ed.escape() == Editor::Escape::Nothing);

    f.ed.toggleFreehand();
    CHECK(f.ed.escape() == Editor::Escape::LeftFreehand);
    CHECK(f.ed.tool() == Tool::Select);

    uint32_t id = f.ed.addShape(Kind::Rect, {100, 100}, 400);
    REQUIRE(f.ed.beginAdjust());
    f.ed.beginPickWhite();
    f.ed.select(id);
    CHECK(f.ed.escape() == Editor::Escape::StoppedPickWhite);
    CHECK_FALSE(f.ed.pickingWhite());
    CHECK(f.ed.adjusting()); // 面板還開著
    CHECK(f.ed.selected() == id);
    CHECK(f.ed.escape() == Editor::Escape::Deselected);
    CHECK(f.ed.selected() == 0);
    CHECK(f.ed.escape() == Editor::Escape::Nothing);
    f.ed.cancelAdjust();

    f.ed.beginCrop();
    CHECK(f.ed.escape() == Editor::Escape::CancelledCrop);
    CHECK_FALSE(f.ed.cropping());
}

TEST_CASE("Editor：加形狀與文字方塊") {
    Fixture f;
    f.ed.toggleFreehand();
    uint32_t a = f.ed.addShape(Kind::Ellipse, {200, 150}, 200);
    REQUIRE(a != 0);
    CHECK_FALSE(f.ed.freehand()); // 加物件就離開塗鴉
    CHECK(f.ed.selected() == a);
    CHECK(f.item(a).kind() == Kind::Ellipse);
    CHECK(nearly(rectOf(f.item(a))->center().x, 200.0f));
    CHECK(f.ed.doc().undoCount() == 1);

    uint32_t t = f.ed.addText({200, 150}, 200, L"Hello");
    REQUIRE(t != 0);
    CHECK(t != a);
    CHECK(f.ed.selected() == t);
    CHECK(f.item(t).kind() == Kind::Text);
    CHECK(f.item(t).as<TextShape>()->value == L"Hello");
    CHECK(nearly(rectOf(f.item(t))->height(), 28.0f)); // 排版後的高度（一行）
    CHECK(f.item(t).stroke.stroke.none());   // 文字方塊預設沒有外框
    CHECK(f.ed.doc().undoCount() == 2);

    Editor empty;
    CHECK(empty.addShape(Kind::Rect, {0, 0}, 100) == 0);
    CHECK(empty.addText({0, 0}, 100, L"x") == 0);
}

TEST_CASE("Editor：方向鍵微移、刪除") {
    Fixture f;
    CHECK_FALSE(f.ed.nudgeSelected(1, 0));
    CHECK_FALSE(f.ed.deleteSelected());
    uint32_t id = f.ed.addShape(Kind::Rect, {100, 100}, 400);
    CHECK(f.ed.nudgeSelected(10, -1));
    CHECK(nearly(rectOf(f.item(id))->left, 50.0f));
    CHECK(nearly(rectOf(f.item(id))->top, 39.0f));
    CHECK(f.ed.doc().undoCount() == 2);
    CHECK(f.ed.deleteSelected());
    CHECK(f.ed.doc().items().empty());
    CHECK(f.ed.selected() == 0);
    f.ed.undo();
    CHECK(f.ed.doc().items().size() == 1);
}

TEST_CASE("Editor：線條樣式套到選取的物件；改文字方塊的外框不影響之後新畫的形狀") {
    Fixture f;
    StrokeStyle blue;
    blue.stroke = Color::rgb(0, 0, 255);
    CHECK_FALSE(f.ed.applyStrokeStyle(blue)); // 沒有選取：只改預設
    CHECK(f.ed.stroke().stroke == Color::rgb(0, 0, 255));
    CHECK(f.ed.currentStroke().stroke == Color::rgb(0, 0, 255));

    uint32_t shape = f.ed.addShape(Kind::Rect, {100, 100}, 400);
    CHECK(f.item(shape).stroke.stroke == Color::rgb(0, 0, 255));
    StrokeStyle green = blue;
    green.stroke = Color::rgb(0, 255, 0);
    CHECK(f.ed.applyStrokeStyle(green));
    CHECK(f.item(shape).stroke.stroke == Color::rgb(0, 255, 0));
    CHECK(f.ed.stroke().stroke == Color::rgb(0, 255, 0));

    uint32_t text = f.ed.addText({200, 200}, 400, L"x");
    StrokeStyle red = green;
    red.stroke = Color::rgb(255, 0, 0);
    CHECK(f.ed.applyStrokeStyle(red));
    CHECK(f.item(text).stroke.stroke == Color::rgb(255, 0, 0));
    CHECK(f.ed.currentStroke().stroke == Color::rgb(255, 0, 0)); // 工具列顯示選取物件的
    CHECK(f.ed.stroke().stroke == Color::rgb(0, 255, 0));        // 預設沒變
}

TEST_CASE("Editor：文字樣式變成預設，選取的是文字方塊才套上去並重算高度") {
    Fixture f;
    TextStyle big;
    big.size = 48;
    uint32_t shape = f.ed.addShape(Kind::Rect, {100, 100}, 400);
    CHECK_FALSE(f.ed.applyTextStyle(big));
    CHECK(f.ed.text().size == 48.0f);
    CHECK(f.ed.currentText().size == 48.0f);
    (void)shape;

    uint32_t text = f.ed.addText({200, 200}, 400, L"x");
    CHECK(f.item(text).as<TextShape>()->style.size == 48.0f);
    TextStyle bold = big;
    bold.bold = true;
    const size_t undoBefore = f.ed.doc().undoCount();
    CHECK(f.ed.applyTextStyle(bold));
    CHECK(f.item(text).as<TextShape>()->style.bold);
    CHECK(f.ed.doc().undoCount() == undoBefore + 1);
}

TEST_CASE("Editor：commitItem 換掉同一個 id 的物件；不在文件裡的不做") {
    Fixture f;
    uint32_t id = f.ed.addShape(Kind::Rect, {100, 100}, 400);
    Annotation a = f.item(id);
    rectOf(a)->right = 300;
    CHECK(f.ed.commitItem(a));
    CHECK(nearly(rectOf(f.item(id))->right, 300.0f));
    a.id = 999;
    const size_t undoBefore = f.ed.doc().undoCount();
    CHECK_FALSE(f.ed.commitItem(a));
    CHECK(f.ed.doc().undoCount() == undoBefore);
}

TEST_CASE("Editor：旋轉整張圖，寬高與解析度對調，標註跟著轉") {
    Fixture f;
    uint32_t id = f.ed.addShape(Kind::Rect, {100, 50}, 100); // 中心 (100, 50)
    CHECK(f.ed.rotateCCW());
    CHECK(f.ed.doc().image()->width == 300);
    CHECK(f.ed.doc().image()->height == 400);
    CHECK(f.ed.doc().dpi() == Dpi{150, 300});
    // 逆時針 90 度：(x, y) → (y, W − x)
    CHECK(std::abs(rectOf(f.item(id))->center().x - 50.0f) < 0.01f);
    CHECK(std::abs(rectOf(f.item(id))->center().y - 300.0f) < 0.01f);
    f.ed.undo();
    CHECK(f.ed.doc().image()->width == 400);
    CHECK(f.ed.doc().dpi() == Dpi{300, 150});

    Editor empty;
    CHECK_FALSE(empty.rotateCCW());
}

TEST_CASE("Editor：調整尺寸。重新取樣時標註跟著縮放；只改解析度時底圖共用") {
    Fixture f;
    uint32_t id = f.ed.addShape(Kind::Rect, {100, 100}, 400); // 40..160
    CHECK(f.ed.resize(200, 150, 72, true));
    CHECK(f.ed.doc().image()->width == 200);
    CHECK(f.ed.doc().image()->height == 150);
    CHECK(f.ed.doc().dpi() == Dpi{72, 72});
    CHECK(nearly(rectOf(f.item(id))->left, 20.0f));
    CHECK(nearly(rectOf(f.item(id))->right, 80.0f));

    ImagePtr before = f.ed.doc().image();
    const size_t bytes = f.ed.doc().historyBytes();
    CHECK(f.ed.resize(999, 999, 300, false)); // 不重新取樣：寬高不看
    CHECK(f.ed.doc().image() == before);
    CHECK(f.ed.doc().dpi() == Dpi{300, 300});
    CHECK(f.ed.doc().historyBytes() == bytes);
    CHECK(f.ed.resize(200, 150, 96, true)); // 尺寸沒變：也只改解析度
    CHECK(f.ed.doc().image() == before);
    f.ed.undo();
    f.ed.undo();
    CHECK(f.ed.doc().dpi() == Dpi{72, 72});
}

TEST_CASE("Editor：undo 之後選取的物件不在了就取消選取") {
    Fixture f;
    uint32_t first = f.ed.addShape(Kind::Rect, {100, 100}, 400);
    uint32_t second = f.ed.addShape(Kind::Rect, {200, 200}, 400);
    CHECK(f.ed.selected() == second);
    f.ed.undo(); // second 不見了
    CHECK(f.ed.selected() == 0);
    f.ed.select(first);
    f.ed.redo(); // first 還在
    CHECK(f.ed.selected() == first);
}

TEST_CASE("Editor：調整顏色的預覽、套用、取消") {
    Fixture f;
    auto red = std::make_shared<Image>(4, 4);
    for (int i = 0; i < 16; ++i) {
        red->pixels[i * 4 + 2] = 200;
        red->pixels[i * 4 + 3] = 255;
    }
    f.ed.reset(red);
    CHECK(f.ed.displayImage() == red);
    REQUIRE(f.ed.beginAdjust());
    CHECK(f.ed.adjusting());
    CHECK(f.ed.adjustBase() == red);
    CHECK_FALSE(f.ed.beginAdjust()); // 已經在調

    ColorAdjustParams p;
    f.ed.previewAdjust(p); // 沒調：預覽就是原圖，不複製
    CHECK(f.ed.displayImage() == red);
    p.exposure = 1.0f;
    f.ed.previewAdjust(p);
    ImagePtr preview = f.ed.displayImage();
    CHECK(preview != red);
    CHECK(preview->pixels != red->pixels);
    CHECK(f.ed.doc().image() == red); // 文件還沒動
    CHECK(f.ed.doc().undoCount() == 0);

    CHECK(f.ed.commitAdjust(p));
    CHECK_FALSE(f.ed.adjusting());
    CHECK(f.ed.adjustBase() == nullptr);
    CHECK(f.ed.doc().image() == preview); // 套用的就是預覽那張，不重算
    CHECK(f.ed.doc().undoCount() == 1);
    CHECK(f.ed.displayImage() == preview);

    // 取消：文件不動
    REQUIRE(f.ed.beginAdjust());
    p.exposure = -1.0f;
    f.ed.previewAdjust(p);
    CHECK(f.ed.displayImage() != preview);
    f.ed.cancelAdjust();
    CHECK(f.ed.displayImage() == preview);
    CHECK(f.ed.doc().undoCount() == 1);

    // 沒調就按好：不產生 undo 步驟
    REQUIRE(f.ed.beginAdjust());
    CHECK_FALSE(f.ed.commitAdjust(ColorAdjustParams{}));
    CHECK(f.ed.doc().undoCount() == 1);
    // 沒預覽過直接套用：照參數算
    REQUIRE(f.ed.beginAdjust());
    p.exposure = 0.5f;
    CHECK(f.ed.commitAdjust(p));
    CHECK(f.ed.doc().undoCount() == 2);

    Editor empty;
    CHECK_FALSE(empty.beginAdjust());
}

TEST_CASE("Editor：取白點時按一下是取顏色，不是拖曳；取完回到原本的工具") {
    Fixture f;
    auto img = std::make_shared<Image>(8, 8);
    for (int i = 0; i < 64; ++i) { // 偏藍的灰
        img->pixels[i * 4 + 0] = 200;
        img->pixels[i * 4 + 1] = 150;
        img->pixels[i * 4 + 2] = 100;
        img->pixels[i * 4 + 3] = 255;
    }
    f.ed.reset(img);
    f.ed.toggleFreehand();
    REQUIRE(f.ed.beginAdjust());
    f.ed.beginPickWhite();
    CHECK(f.ed.pickingWhite());
    CHECK(f.ed.press(Fixture::at(3, 3), false) == Editor::Press::PickedWhite);
    CHECK_FALSE(f.ed.dragging());
    CHECK_FALSE(f.ed.pickingWhite());
    CHECK_FALSE(f.ed.adjustParams().isIdentity()); // 白平衡變了
    CHECK(f.ed.displayImage() != img);              // 預覽也更新了
    // 下一下回到塗鴉
    CHECK(f.ed.press(Fixture::at(3, 3), false) == Editor::Press::Drag);
    CHECK(f.ed.drag() == DragKind::Draw);
    f.ed.cancelDrag();
    // 取消調色時取白點也一起結束
    f.ed.beginPickWhite();
    f.ed.cancelAdjust();
    CHECK_FALSE(f.ed.pickingWhite());
}

TEST_CASE("Editor：文字編輯。打字時方塊長高但不進 undo，結束時才一次 commit") {
    Fixture f;
    uint32_t shape = f.ed.addShape(Kind::Rect, {100, 100}, 400);
    CHECK_FALSE(f.ed.beginTextEdit(shape)); // 不是文字方塊
    CHECK_FALSE(f.ed.beginTextEdit(999));
    CHECK_FALSE(f.ed.textEditing());

    uint32_t id = f.ed.addText({200, 150}, 400, L"one");
    const size_t undoBefore = f.ed.doc().undoCount();
    REQUIRE(f.ed.beginTextEdit(id));
    CHECK(f.ed.textEditing());
    CHECK(f.ed.editingId() == id);
    CHECK(f.ed.editText() == L"one");
    REQUIRE(f.ed.editingShape() != nullptr);
    CHECK(f.ed.editingShape() == f.item(id).as<TextShape>()); // 文件裡的那個方塊
    CHECK_FALSE(f.ed.deleteSelected()); // 編輯中不刪物件（Delete 是給輸入框用的）

    CHECK_FALSE(f.ed.setEditText(L"onex")); // 還是一行：高度沒變
    CHECK(f.ed.setEditText(L"one\r\ntwo"));  // EDIT 控制項給 CRLF
    CHECK(f.ed.editText() == L"one\ntwo");
    CHECK(nearly(rectOf(f.item(id))->height(), 48.0f));
    CHECK(f.item(id).as<TextShape>()->value == L"one"); // 文件裡的字還沒換
    CHECK(f.ed.doc().undoCount() == undoBefore);

    CHECK(f.ed.endTextEdit(true));
    CHECK_FALSE(f.ed.textEditing());
    CHECK(f.ed.editingShape() == nullptr);
    CHECK(f.item(id).as<TextShape>()->value == L"one\ntwo");
    CHECK(f.ed.doc().undoCount() == undoBefore + 1);
    f.ed.undo();
    CHECK(f.item(id).as<TextShape>()->value == L"one");
}

TEST_CASE("Editor：結束文字編輯。沒改不進 undo；打成空的刪掉方塊；不 commit 就丟掉打的字") {
    Fixture f;
    uint32_t id = f.ed.addText({200, 150}, 400, L"one");
    const size_t undoBefore = f.ed.doc().undoCount();

    REQUIRE(f.ed.beginTextEdit(id));
    CHECK_FALSE(f.ed.endTextEdit(true)); // 沒改
    CHECK(f.ed.doc().undoCount() == undoBefore);

    REQUIRE(f.ed.beginTextEdit(id));
    f.ed.setEditText(L"changed");
    CHECK_FALSE(f.ed.endTextEdit(false)); // 換圖時：丟掉
    CHECK(f.item(id).as<TextShape>()->value == L"one");
    CHECK(f.ed.doc().undoCount() == undoBefore);

    REQUIRE(f.ed.beginTextEdit(id));
    f.ed.setEditText(L"");
    CHECK(f.ed.selected() == id);
    CHECK(f.ed.endTextEdit(true));
    CHECK(f.ed.doc().findItem(id) == nullptr);
    CHECK(f.ed.selected() == 0);
    CHECK(f.ed.doc().undoCount() == undoBefore + 1);

    CHECK_FALSE(f.ed.endTextEdit(true)); // 沒有在編輯
}

TEST_CASE("Editor：游標形狀和按下去會做的事一致") {
    Fixture f;
    CHECK(f.ed.cursorAt(Fixture::at(300, 250), false) == CursorKind::Default);
    uint32_t id = f.ed.addShape(Kind::Rect, {100, 100}, 400); // 選取中，40..160
    CHECK(f.ed.cursorAt(Fixture::at(160, 160), false) == CursorKind::Move); // 控制點
    CHECK(f.ed.cursorAt(Fixture::at(100, 100), false) == CursorKind::Hand); // 外框方框內
    CHECK(f.ed.cursorAt(Fixture::at(100, 100), true) == CursorKind::Default); // 兩頁並排：不能編輯
    f.ed.select(0);
    CHECK(f.ed.cursorAt(Fixture::at(158, 158), false) == CursorKind::Hand); // 沒選取就沒有控制點
    CHECK(f.ed.cursorAt(Fixture::at(300, 250), false) == CursorKind::Default);

    f.ed.press(Fixture::at(300, 250), false); // 平移中
    CHECK(f.ed.cursorAt(Fixture::at(300, 250), false) == CursorKind::Move);
    f.ed.release();

    f.ed.toggleFreehand();
    CHECK(f.ed.cursorAt(Fixture::at(100, 100), false) == CursorKind::Cross);
    f.ed.toggleFreehand();

    f.ed.beginCrop();
    CHECK(f.ed.cursorAt(Fixture::at(1, 1), false) == CursorKind::SizeNWSE);
    CHECK(f.ed.cursorAt(Fixture::at(399, 1), false) == CursorKind::SizeNESW);
    CHECK(f.ed.cursorAt(Fixture::at(1, 150), false) == CursorKind::SizeWE);
    CHECK(f.ed.cursorAt(Fixture::at(200, 299), false) == CursorKind::SizeNS);
    CHECK(f.ed.cursorAt(Fixture::at(200, 150), false) == CursorKind::Move);
    CHECK(f.ed.cursorAt(Fixture::at(500, 500), false) == CursorKind::Default); // 框外
    f.ed.press(Fixture::at(1, 1), false); // 拖著角：游標跟著拖的部位，不看現在的位置
    CHECK(f.ed.cursorAt(Fixture::at(200, 150), false) == CursorKind::SizeNWSE);
    f.ed.cancelDrag();
    f.ed.cancelCrop();

    f.ed.beginAdjust();
    f.ed.beginPickWhite();
    CHECK(f.ed.cursorAt(Fixture::at(300, 250), false) == CursorKind::Cross);
    f.ed.cancelAdjust();

    // 文字編輯中：外框方框內不顯示手指（滑鼠在輸入框外的留白）
    uint32_t text = f.ed.addText({300, 200}, 400, L"x");
    f.ed.select(0);
    REQUIRE(f.ed.beginTextEdit(text));
    CHECK(f.ed.cursorAt(Fixture::at(100, 100), false) == CursorKind::Default);
    f.ed.endTextEdit(false);
    (void)id;
}

TEST_CASE("Editor：itemBoxAt / tipItemAt / textItemAt") {
    Fixture f;
    uint32_t shape = f.ed.addShape(Kind::Rect, {100, 100}, 400);
    uint32_t text = f.ed.addText({300, 200}, 400, L"x");
    Pt inShape{100, 100}, inText = rectOf(f.item(text))->center(), nowhere{390, 10};
    CHECK(f.ed.itemBoxAt(inShape) == shape);
    CHECK(f.ed.itemBoxAt(inText) == text);
    CHECK(f.ed.itemBoxAt(nowhere) == 0);

    CHECK(f.ed.tipItemAt(inShape, false) == shape);
    CHECK(f.ed.tipItemAt(inShape, true) == 0); // 兩頁並排
    f.ed.toggleFreehand();
    CHECK(f.ed.tipItemAt(inShape, false) == 0);
    f.ed.toggleFreehand();
    f.ed.beginCrop();
    CHECK(f.ed.tipItemAt(inShape, false) == 0);
    f.ed.cancelCrop();
    f.ed.beginAdjust();
    CHECK(f.ed.tipItemAt(inShape, false) == shape); // 面板開著不影響
    f.ed.beginPickWhite();
    CHECK(f.ed.tipItemAt(inShape, false) == 0);
    f.ed.cancelAdjust();
    REQUIRE(f.ed.beginTextEdit(text));
    CHECK(f.ed.tipItemAt(inShape, false) == 0);
    f.ed.endTextEdit(false);

    CHECK(f.ed.textItemAt(Fixture::at(inText.x, inText.y)) == text);
    CHECK(f.ed.textItemAt(Fixture::at(40, 100)) == 0); // 形狀不算
}

TEST_CASE("Editor：換圖時選取清掉，塗鴉工具留著") {
    Fixture f;
    f.ed.addShape(Kind::Rect, {100, 100}, 400);
    f.ed.toggleFreehand();
    f.ed.addShape(Kind::Rect, {100, 100}, 400);
    CHECK(f.ed.selected() != 0);
    f.ed.toggleFreehand(); // 加形狀時離開了塗鴉，再打開（選取會清掉）
    f.ed.reset(std::make_shared<Image>(10, 10));
    CHECK(f.ed.selected() == 0);
    CHECK(f.ed.freehand());
    CHECK(f.ed.doc().items().empty());
    CHECK(f.ed.doc().dpi() == Dpi{});
    CHECK_FALSE(f.ed.doc().canUndo());
    f.ed.reset(nullptr);
    CHECK(f.ed.displayImage() == nullptr);
}
