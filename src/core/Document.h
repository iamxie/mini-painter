#pragma once
#include <cstdint>
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

// 文件 = 目前狀態 + undo / redo 堆疊。每個完成的編輯動作呼叫 commit 一次。
class Document {
public:
    void reset(ImagePtr image);

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
    DocState cur_;
    std::vector<DocState> undo_, redo_;
    uint64_t nextSerial_ = 1;
    uint64_t savedSerial_ = 0;
    uint32_t lastId_ = 0;
};

} // namespace mp
