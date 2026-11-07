#include "core/Document.h"

namespace mp {

void Document::reset(ImagePtr image) {
    cur_ = DocState{};
    cur_.image = std::move(image);
    cur_.serial = nextSerial_++;
    undo_.clear();
    redo_.clear();
    savedSerial_ = cur_.serial;
}

void Document::commit(DocState next) {
    next.serial = nextSerial_++;
    undo_.push_back(std::move(cur_));
    cur_ = std::move(next);
    redo_.clear();
}

void Document::commitItems(std::vector<Annotation> items) {
    DocState next;
    next.image = cur_.image;
    next.items = std::move(items);
    commit(std::move(next));
}

void Document::commitImage(ImagePtr image) {
    DocState next;
    next.image = std::move(image);
    next.items = cur_.items;
    commit(std::move(next));
}

void Document::undo() {
    if (undo_.empty()) return;
    redo_.push_back(std::move(cur_));
    cur_ = std::move(undo_.back());
    undo_.pop_back();
}

void Document::redo() {
    if (redo_.empty()) return;
    undo_.push_back(std::move(cur_));
    cur_ = std::move(redo_.back());
    redo_.pop_back();
}

const Annotation* Document::findItem(uint32_t id) const {
    int i = indexOf(id);
    return i < 0 ? nullptr : &cur_.items[i];
}

int Document::indexOf(uint32_t id) const {
    for (size_t i = 0; i < cur_.items.size(); ++i)
        if (cur_.items[i].id == id) return static_cast<int>(i);
    return -1;
}

} // namespace mp
