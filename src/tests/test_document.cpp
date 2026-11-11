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

TEST_CASE("Document：undo 上限預設 99，clampUndoLimit 夾進 1～999") {
    Document d;
    CHECK(d.undoLimit() == 99);
    CHECK(clampUndoLimit(0) == 1);
    CHECK(clampUndoLimit(-5) == 1);
    CHECK(clampUndoLimit(50) == 50);
    CHECK(clampUndoLimit(5000) == 999);
    d.setUndoLimit(0);
    CHECK(d.undoLimit() == 1);
    d.setUndoLimit(100000);
    CHECK(d.undoLimit() == 999);
}

TEST_CASE("undoLimitFromText：數字夾進範圍，不是數字回 fallback") {
    CHECK(undoLimitFromText(L"20", 99) == 20);
    CHECK(undoLimitFromText(L" 20", 99) == 20);
    CHECK(undoLimitFromText(L"0", 99) == 1);
    CHECK(undoLimitFromText(L"-4", 99) == 1);
    CHECK(undoLimitFromText(L"1000", 99) == 999);
    CHECK(undoLimitFromText(L"99999999999999999999", 50) == 999);
    CHECK(undoLimitFromText(L"-99999999999999999999", 50) == 1);
    CHECK(undoLimitFromText(L"", 42) == 42);
    CHECK(undoLimitFromText(L"abc", 42) == 42);
}

TEST_CASE("Document：超過 undo 上限丟最舊的一步，被丟掉的圖會釋放") {
    Document d;
    d.setUndoLimit(3);
    auto first = img(1, 1);
    std::weak_ptr<const Image> firstWeak = first;
    d.reset(first);
    first.reset();
    for (int w = 2; w <= 6; ++w) d.commitImage(img(w, w)); // 5 步，只留最後 3 步
    CHECK(d.undoCount() == 3);
    CHECK(firstWeak.expired()); // 最舊的底圖沒有人握著了
    CHECK(d.image()->width == 6);
    d.undo();
    d.undo();
    d.undo();
    CHECK(d.image()->width == 3); // 只能回到 3 步前
    CHECK_FALSE(d.canUndo());
    // undo / redo 來回不會讓堆疊超過上限
    d.redo();
    d.redo();
    d.redo();
    CHECK(d.undoCount() == 3);
    CHECK(d.image()->width == 6);
}

TEST_CASE("Document：預設上限下第 100 步開始丟") {
    Document d;
    d.reset(img(1, 1));
    for (int i = 0; i < 99; ++i) d.commitItems({});
    CHECK(d.undoCount() == 99);
    d.commitItems({});
    CHECK(d.undoCount() == 99);
}

TEST_CASE("Document：上限改小立刻丟，reset 不改上限") {
    Document d;
    d.reset(img(1, 1));
    for (int w = 2; w <= 9; ++w) d.commitImage(img(w, w));
    CHECK(d.undoCount() == 8);
    d.setUndoLimit(2);
    CHECK(d.undoCount() == 2);
    d.undo();
    d.undo();
    CHECK(d.image()->width == 7);
    CHECK_FALSE(d.canUndo());
    d.reset(img(1, 1));
    CHECK(d.undoLimit() == 2);
}

TEST_CASE("Document：存檔時的狀態被上限丟掉後，undo 到底仍是 dirty") {
    Document d;
    d.setUndoLimit(2);
    d.reset(img(1, 1)); // 開檔時 = 已存檔
    for (int w = 2; w <= 5; ++w) d.commitImage(img(w, w));
    while (d.canUndo()) d.undo();
    CHECK(d.image()->width == 3);
    CHECK(d.dirty()); // 回不到存檔時的那張圖
}

// ---- undo 記憶體總量上限 ----

static ImagePtr mib(int n = 1) { return std::make_shared<Image>(512, 512 * n); } // n MiB

TEST_CASE("Document：記憶體上限預設 512 MB，clamp 與文字解析") {
    Document d;
    CHECK(d.undoMemoryLimitMB() == 512);
    CHECK(clampUndoMemoryMB(0) == 16);
    CHECK(clampUndoMemoryMB(300) == 300);
    CHECK(clampUndoMemoryMB(1000000) == 65536);
    CHECK(undoMemoryFromText(L"256", 512) == 256);
    CHECK(undoMemoryFromText(L"1", 512) == 16);
    CHECK(undoMemoryFromText(L"99999999999999999999", 512) == 65536);
    CHECK(undoMemoryFromText(L"", 512) == 512);
    CHECK(undoMemoryFromText(L"x", 300) == 300);
    d.setUndoMemoryLimitMB(1);
    CHECK(d.undoMemoryLimitMB() == 16);
}

TEST_CASE("Document：historyBytes 只算目前這張以外的底圖，同一張算一次") {
    Document d;
    d.reset(mib());
    CHECK(d.historyBytes() == 0);
    for (int i = 0; i < 10; ++i) d.commitItems({}); // 只改標註：共用底圖
    CHECK(d.historyBytes() == 0);
    d.commitImage(mib(2)); // 舊的 1 MiB 底圖被 11 個狀態共用，只算一次
    CHECK(d.historyBytes() == 1024 * 1024);
    d.undo(); // 目前變回 1 MiB 那張，2 MiB 那張在 redo
    CHECK(d.historyBytes() == 2 * 1024 * 1024);
}

TEST_CASE("Document：整圖步驟超過記憶體上限，丟最舊的並釋放") {
    Document d;
    d.setUndoMemoryLimitMB(16);
    auto first = mib();
    std::weak_ptr<const Image> firstWeak = first;
    d.reset(first);
    first.reset();
    for (int i = 0; i < 30; ++i) d.commitImage(mib());
    CHECK(d.undoCount() == 16); // 16 張 1 MiB，剛好不超過
    CHECK(d.historyBytes() == 16u * 1024 * 1024);
    CHECK(firstWeak.expired());
    CHECK(d.undoLimit() == 99); // 步數上限沒到，是記憶體上限在管
}

TEST_CASE("Document：只改標註的步驟不受記憶體上限影響") {
    Document d;
    d.setUndoMemoryLimitMB(16);
    d.reset(mib(20)); // 目前這張就超過上限，但它不算在歷史裡
    for (int i = 0; i < 50; ++i) d.commitItems({});
    CHECK(d.undoCount() == 50);
}

TEST_CASE("Document：單張圖就超過記憶體上限時，仍留最近一步") {
    Document d;
    d.setUndoMemoryLimitMB(16);
    d.reset(mib(20));
    d.commitImage(mib(20));
    CHECK(d.undoCount() == 1);
    d.commitImage(mib(20));
    d.commitImage(mib(20));
    CHECK(d.undoCount() == 1);
    d.undo();
    CHECK(d.image()->height == 512 * 20);
    CHECK_FALSE(d.canUndo());
}

TEST_CASE("Document：記憶體上限改小立刻丟最舊的 undo") {
    Document d;
    d.setUndoMemoryLimitMB(64);
    d.reset(mib());
    for (int i = 0; i < 40; ++i) d.commitImage(mib());
    CHECK(d.undoCount() == 40);
    d.setUndoMemoryLimitMB(16);
    CHECK(d.undoCount() == 16);
}

TEST_CASE("Document：記憶體上限改小時，undo 丟完再從最遠的 redo 丟") {
    Document d;
    d.setUndoMemoryLimitMB(64);
    d.reset(std::make_shared<Image>(512, 512));
    for (int w = 1; w <= 30; ++w) d.commitImage(std::make_shared<Image>(512 + w, 512)); // 寬度當編號
    for (int i = 0; i < 30; ++i) d.undo(); // 全部退回：undo 空，redo 30 步
    CHECK(d.undoCount() == 0);
    CHECK(d.redoCount() == 30);
    d.setUndoMemoryLimitMB(16);
    CHECK(d.historyBytes() <= 16u * 1024 * 1024);
    CHECK(d.redoCount() == 15); // 每張略大於 1 MiB，16 MiB 裝得下 15 張
    d.redo();
    CHECK(d.image()->width == 513); // 留下的是離現在最近的
    while (d.canRedo()) d.redo();
    CHECK(d.image()->width == 512 + 15);
}

TEST_CASE("Document：undo / redo 來回不丟東西") {
    Document d;
    d.setUndoMemoryLimitMB(16);
    d.reset(mib());
    for (int i = 0; i < 16; ++i) d.commitImage(mib());
    CHECK(d.undoCount() == 16);
    for (int i = 0; i < 16; ++i) d.undo();
    CHECK(d.redoCount() == 16);
    for (int i = 0; i < 16; ++i) d.redo();
    CHECK(d.undoCount() == 16);
}

TEST_CASE("Document：步數上限與記憶體上限同時生效，取先到的") {
    Document d;
    d.setUndoLimit(5);
    d.setUndoMemoryLimitMB(16);
    d.reset(mib());
    for (int i = 0; i < 30; ++i) d.commitImage(mib());
    CHECK(d.undoCount() == 5);
    d.setUndoLimit(99);
    for (int i = 0; i < 30; ++i) d.commitImage(mib());
    CHECK(d.undoCount() == 16);
}
