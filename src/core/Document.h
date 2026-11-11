#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "core/Annotation.h"
#include "core/Image.h"

namespace mp {

// 一個可還原的狀態：底圖加上所有標註物件。
struct DocState {
    ImagePtr image;
    std::vector<Annotation> items;
    uint64_t serial = 0; // 每個狀態唯一，用來判斷有沒有未存的變更
};

// undo 最多留幾步：預設值與設定面板允許的範圍。
constexpr int kUndoLimitDefault = 99, kUndoLimitMin = 1, kUndoLimitMax = 999;
// 把設定值夾進 [kUndoLimitMin, kUndoLimitMax]。
int clampUndoLimit(int n);
// 使用者輸入或 ini 裡的文字 → 上限。不是數字（含空字串）回 fallback，數字夾進範圍。
int undoLimitFromText(const std::wstring& text, int fallback);

// undo / redo 歷史裡的底圖最多佔多少記憶體（MB = MiB）：預設值與允許的範圍。
constexpr int kUndoMemoryDefaultMB = 512, kUndoMemoryMinMB = 16, kUndoMemoryMaxMB = 65536;
int clampUndoMemoryMB(int n);
int undoMemoryFromText(const std::wstring& text, int fallback);

// 文件 = 目前狀態 + undo / redo 堆疊。每個完成的編輯動作呼叫 commit 一次。
class Document {
public:
    void reset(ImagePtr image);

    // undo 最多留幾步（會先夾進允許範圍）。超過時丟最舊的；改小時立刻丟。
    void setUndoLimit(int n);
    int undoLimit() const { return undoLimit_; }
    size_t undoCount() const { return undo_.size(); }
    size_t redoCount() const { return redo_.size(); }

    // 歷史裡的底圖最多佔多少 MB（會先夾進允許範圍）。commit 後超過就從最舊的 undo 丟起，
    // 但最近一步一定留著（單張圖就超過上限時仍能 undo 一步）。改小時立刻丟：undo 丟到剩一步還超過，
    // 再從離現在最遠的 redo 丟。undo / redo 本身不丟東西。
    void setUndoMemoryLimitMB(int mb);
    int undoMemoryLimitMB() const { return undoMemoryMB_; }
    // undo / redo 裡的底圖位元組：同一張只算一次，目前顯示的那張不算（只改標註的步驟共用底圖，不佔）。
    size_t historyBytes() const;

    const DocState& state() const { return cur_; }
    ImagePtr image() const { return cur_.image; }
    const std::vector<Annotation>& items() const { return cur_.items; }
    // 直接改目前狀態，不進 undo。只給「編輯中暫時更新」用，例如打字時文字方塊長高。
    std::vector<Annotation>& itemsMutable() { return cur_.items; }

    // 把新狀態變成目前狀態；舊的進 undo，redo 清空。
    void commit(DocState next);
    // 方便用法：只改標註物件。
    void commitItems(std::vector<Annotation> items);
    // 只改底圖，標註物件維持。
    void commitImage(ImagePtr image);

    bool canUndo() const { return !undo_.empty(); }
    bool canRedo() const { return !redo_.empty(); }
    void undo();
    void redo();

    bool dirty() const { return cur_.serial != savedSerial_; }
    void markSaved() { savedSerial_ = cur_.serial; }

    // 配給新物件的 id。
    uint32_t nextId() { return ++lastId_; }

    const Annotation* findItem(uint32_t id) const;
    int indexOf(uint32_t id) const;

private:
    void trimUndo();

    int undoLimit_ = kUndoLimitDefault;
    int undoMemoryMB_ = kUndoMemoryDefaultMB;
    DocState cur_;
    std::vector<DocState> undo_, redo_;
    uint64_t nextSerial_ = 1;
    uint64_t savedSerial_ = 0;
    uint32_t lastId_ = 0;
};

} // namespace mp
