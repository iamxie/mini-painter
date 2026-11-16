#include "core/Document.h"

#include "core/TextUtil.h"

#include <algorithm>
#include <set>
#include <stdexcept>

namespace mp {

void Document::reset(ImagePtr image, Dpi dpi) {
    cur_ = DocState{};
    cur_.image = std::move(image);
    cur_.dpi = dpi;
    cur_.serial = nextSerial_++;
    undo_.clear();
    redo_.clear();
    savedSerial_ = cur_.serial;
}

int clampUndoLimit(int n) { return std::clamp(n, kUndoLimitMin, kUndoLimitMax); }

int undoLimitFromText(const std::wstring& text, int fallback) {
    return clampedIntFromText(text, fallback, kUndoLimitMin, kUndoLimitMax);
}

int clampUndoMemoryMB(int n) { return std::clamp(n, kUndoMemoryMinMB, kUndoMemoryMaxMB); }

int undoMemoryFromText(const std::wstring& text, int fallback) {
    return clampedIntFromText(text, fallback, kUndoMemoryMinMB, kUndoMemoryMaxMB);
}

void Document::setUndoLimit(int n) {
    undoLimit_ = clampUndoLimit(n);
    trimUndo();
}

void Document::setUndoMemoryLimitMB(int mb) {
    undoMemoryMB_ = clampUndoMemoryMB(mb);
    trimUndo();
}

size_t Document::historyBytes() const {
    std::set<const Image*> seen;
    size_t bytes = 0;
    for (const auto* stack : {&undo_, &redo_}) {
        for (const auto& st : *stack) {
            const Image* img = st.image.get();
            if (img && img != cur_.image.get() && seen.insert(img).second) bytes += img->pixels.size();
        }
    }
    return bytes;
}

bool Document::nextCommitMayDropUndo() const {
    if (undo_.empty()) return false;
    if (undo_.size() >= static_cast<size_t>(undoLimit_)) return true;
    // commit 後：目前這張進 undo、redo 清空、新的那張不算。
    std::set<const Image*> seen;
    size_t bytes = 0;
    auto add = [&](const ImagePtr& p) {
        if (p && seen.insert(p.get()).second) bytes += p->pixels.size();
    };
    for (const auto& st : undo_) add(st.image);
    add(cur_.image);
    return bytes > static_cast<size_t>(undoMemoryMB_) * 1024 * 1024;
}

void Document::trimUndo() {
    size_t limit = static_cast<size_t>(undoLimit_);
    if (undo_.size() > limit) undo_.erase(undo_.begin(), undo_.end() - static_cast<std::ptrdiff_t>(limit));
    size_t budget = static_cast<size_t>(undoMemoryMB_) * 1024 * 1024;
    while (undo_.size() > 1 && historyBytes() > budget) undo_.erase(undo_.begin());
    // 只有改小上限時才會走到：undo 剩一步還超過，從最遠的 redo 丟（redo_ 的最前面離現在最遠）。
    while (undo_.size() + redo_.size() > 1 && !redo_.empty() && historyBytes() > budget) redo_.erase(redo_.begin());
}

void Document::commit(DocState next) {
    next.serial = nextSerial_++;
    undo_.push_back(std::move(cur_));
    cur_ = std::move(next);
    redo_.clear();
    trimUndo();
}

void Document::commitItems(std::vector<Annotation> items) {
    DocState next;
    next.image = cur_.image;
    next.dpi = cur_.dpi;
    next.items = std::move(items);
    commit(std::move(next));
}

void Document::commitImage(ImagePtr image) {
    DocState next;
    next.image = std::move(image);
    next.dpi = cur_.dpi;
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
