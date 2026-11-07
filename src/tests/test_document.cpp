#include <catch2/catch_test_macros.hpp>

#include "core/Document.h"

using namespace mp;

static ImagePtr img(int w, int h) { return std::make_shared<Image>(w, h); }

TEST_CASE("Document：commit、undo、redo") {
    Document d;
    d.reset(img(4, 4));
    CHECK_FALSE(d.dirty());
    CHECK_FALSE(d.canUndo());

    Annotation a = makeShape(Kind::Rect, {2, 2}, 4, StrokeStyle{});
    a.id = d.nextId();
    d.commitItems({a});
    CHECK(d.dirty());
    CHECK(d.items().size() == 1);
    CHECK(d.canUndo());
    CHECK_FALSE(d.canRedo());

    d.undo();
    CHECK(d.items().empty());
    CHECK_FALSE(d.dirty());
    CHECK(d.canRedo());

    d.redo();
    CHECK(d.items().size() == 1);
    CHECK(d.dirty());
}

TEST_CASE("Document：存檔後仍可 undo，undo 回去又變 dirty") {
    Document d;
    d.reset(img(4, 4));
    d.commitItems({makeShape(Kind::Rect, {2, 2}, 4, StrokeStyle{})});
    d.markSaved();
    CHECK_FALSE(d.dirty());
    CHECK(d.canUndo());
    d.undo();
    CHECK(d.dirty());
    d.redo();
    CHECK_FALSE(d.dirty());
}

TEST_CASE("Document：新的 commit 清掉 redo") {
    Document d;
    d.reset(img(4, 4));
    d.commitItems({makeShape(Kind::Rect, {2, 2}, 4, StrokeStyle{})});
    d.undo();
    CHECK(d.canRedo());
    d.commitImage(img(8, 8));
    CHECK_FALSE(d.canRedo());
    CHECK(d.image()->width == 8);
    CHECK(d.items().empty());
}

TEST_CASE("Document：commitImage 保留標註，findItem / indexOf") {
    Document d;
    d.reset(img(4, 4));
    Annotation a = makeShape(Kind::Ellipse, {2, 2}, 4, StrokeStyle{});
    a.id = d.nextId();
    d.commitItems({a});
    d.commitImage(img(8, 8));
    CHECK(d.items().size() == 1);
    CHECK(d.indexOf(a.id) == 0);
    CHECK(d.findItem(a.id) != nullptr);
    CHECK(d.findItem(999) == nullptr);
    CHECK(d.indexOf(999) == -1);
}

TEST_CASE("Document：reset 清空堆疊") {
    Document d;
    d.reset(img(4, 4));
    d.commitItems({makeShape(Kind::Rect, {2, 2}, 4, StrokeStyle{})});
    d.reset(img(2, 2));
    CHECK_FALSE(d.canUndo());
    CHECK_FALSE(d.dirty());
    CHECK(d.items().empty());
}
