#include "core/Document.h"

#include <algorithm>
#include <set>
#include <stdexcept>

namespace mp {

void Document::reset(ImagePtr image) {
    cur_ = DocState{};
    cur_.image = std::move(image);
    cur_.serial = nextSerial_++;
    undo_.clear();
    redo_.clear();
    savedSerial_ = cur_.serial;
}

namespace {

// 文字 → [lo, hi] 的整數。不是數字回 fallback；位數多到 int 裝不下時，正的當成 hi、負的當成 lo。
int clampedIntFromText(const std::wstring& text, int fallback, int lo, int hi) {
    try {
        return std::clamp(std::stoi(text), lo, hi);
    } catch (const std::invalid_argument&) {
        return fallback;
    } catch (const std::out_of_range&) {
        return text.find(L'-') != std::wstring::npos ? lo : hi;
    }
}

} // namespace

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
