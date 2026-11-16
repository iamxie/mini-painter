#include "core/Editor.h"

#include <algorithm>
#include <cmath>

#include "core/ImageOps.h"

namespace mp {

void Editor::reset(ImagePtr image, Dpi dpi) {
    doc_.reset(std::move(image), dpi);
    selected_ = 0;
}

// ---- 狀態 ----

const std::vector<Annotation>& Editor::visibleItems() const {
    return drag_ == DragKind::Move || drag_ == DragKind::Handle ? liveItems_ : doc_.items();
}

const Annotation* Editor::selectedItem() const {
    if (!selected_) return nullptr;
    for (const auto& a : visibleItems())
        if (a.id == selected_) return &a;
    return nullptr;
}

ImagePtr Editor::displayImage() const {
    if (adjusting_ && adjustPreview_) return adjustPreview_;
    return doc_.image();
}

StrokeStyle Editor::currentStroke() const {
    if (const Annotation* a = doc_.findItem(selected_)) return a->stroke;
    return stroke_;
}

TextStyle Editor::currentText() const {
    if (const Annotation* a = doc_.findItem(selected_)) {
        if (const auto* t = a->as<TextShape>()) return t->style;
    }
    return text_;
}

void Editor::fitText(Annotation& a) const {
    if (auto* t = a.as<TextShape>(); t && measure_) t->rect.bottom = t->rect.top + measure_(*t);
}

// ---- 工具 ----

bool Editor::toggleFreehand() {
    tool_ = tool_ == Tool::Freehand ? Tool::Select : Tool::Freehand;
    if (tool_ == Tool::Freehand) selected_ = 0;
    return tool_ == Tool::Freehand;
}

void Editor::leaveFreehand() {
    if (tool_ == Tool::Freehand) tool_ = Tool::Select;
}

bool Editor::beginCrop() {
    auto img = doc_.image();
    if (!img) return false;
    tool_ = Tool::Crop;
    selected_ = 0;
    cropBox_ = {0, 0, img->width, img->height};
    cropPart_ = CropPart::None;
    return true;
}

bool Editor::cancelCrop() {
    if (tool_ != Tool::Crop) return false;
    if (drag_ == DragKind::Crop) drag_ = DragKind::None;
    tool_ = Tool::Select;
    return true;
}

Editor::CropResult Editor::applyCrop() {
    auto img = doc_.image();
    if (tool_ != Tool::Crop || !img) return CropResult::NotCropping;
    const CropBox box = cropBox_;
    if (box == CropBox{0, 0, img->width, img->height}) {
        cancelCrop();
        return CropResult::Unchanged;
    }
    if (drag_ == DragKind::Crop) drag_ = DragKind::None;
    tool_ = Tool::Select;
    DocState next;
    next.image = std::make_shared<Image>(cropImage(*img, box.left, box.top, box.width(), box.height()));
    next.dpi = doc_.dpi();
    next.items = cropAnnotations(doc_.items(), box);
    doc_.commit(std::move(next));
    return CropResult::Applied;
}

// ---- 滑鼠 ----

Editor::Press Editor::press(const Pointer& p, bool twoPage) {
    dragStart_ = p.pos;
    dragMoved_ = false;
    if (twoPage) {
        drag_ = DragKind::Pan;
        return Press::Drag;
    }
    if (pickingWhite_ && adjustBase_) {
        pickingWhite_ = false;
        setWhiteBalanceFrom(adjustParams_, *adjustBase_, static_cast<int>(p.pos.x), static_cast<int>(p.pos.y));
        previewAdjust(adjustParams_);
        return Press::PickedWhite;
    }
    if (tool_ == Tool::Crop) {
        // 按在框的角、邊、框內就拖框，框外就平移畫面；物件不能點選。
        cropPart_ = cropHitTest(cropBox_, p.pos, p.cropTol, p.cropCorner);
        if (cropPart_ != CropPart::None) {
            drag_ = DragKind::Crop;
            cropStart_ = cropBox_;
        } else {
            drag_ = DragKind::Pan;
        }
        return Press::Drag;
    }
    if (tool_ == Tool::Freehand) {
        drag_ = DragKind::Draw;
        drawPath_ = {p.pos};
        return Press::Drag;
    }
    // 選取物件的控制點
    if (const Annotation* sel = selectedItem()) {
        for (const auto& h : handles(*sel)) {
            if (dist(h.pos, p.pos) <= p.handleTol) {
                drag_ = DragKind::Handle;
                dragHandle_ = h.id;
                liveItems_ = doc_.items();
                return Press::Drag;
            }
        }
    }
    // 物件（上層優先）
    const auto& items = doc_.items();
    for (int i = static_cast<int>(items.size()) - 1; i >= 0; --i) {
        if (hitTest(items[i], p.pos, p.tol)) {
            selected_ = items[i].id;
            drag_ = DragKind::Move;
            liveItems_ = items;
            return Press::Drag;
        }
    }
    selected_ = 0;
    drag_ = DragKind::Pan;
    return Press::Drag;
}

void Editor::move(const Pointer& p, bool pastClickSlop) {
    if (pastClickSlop) dragMoved_ = true;
    switch (drag_) {
    case DragKind::Draw:
        if (drawPath_.empty() || dist(drawPath_.back(), p.pos) >= p.drawStep) drawPath_.push_back(p.pos);
        break;
    case DragKind::Crop:
        if (auto img = doc_.image())
            cropBox_ = dragCropBox(cropStart_, cropPart_, p.pos.x - dragStart_.x, p.pos.y - dragStart_.y, img->width, img->height);
        break;
    case DragKind::Move: {
        int i = doc_.indexOf(selected_);
        if (i >= 0) {
            liveItems_[i] = doc_.items()[i];
            moveBy(liveItems_[i], p.pos.x - dragStart_.x, p.pos.y - dragStart_.y);
        }
        break;
    }
    case DragKind::Handle: {
        int i = doc_.indexOf(selected_);
        if (i >= 0) {
            dragHandle(liveItems_[i], dragHandle_, p.pos);
            fitText(liveItems_[i]);
        }
        break;
    }
    case DragKind::Pan:
    case DragKind::None: break;
    }
}

bool Editor::release() {
    const DragKind d = drag_;
    drag_ = DragKind::None;
    bool changed = false;
    switch (d) {
    case DragKind::Draw:
        if (!drawPath_.empty()) {
            doc_.addItem(makeFreehand(drawPath_, stroke_));
            changed = true;
        }
        drawPath_.clear();
        break;
    case DragKind::Move:
    case DragKind::Handle:
        if (dragMoved_) {
            doc_.commitItems(liveItems_);
            changed = true;
        }
        liveItems_.clear();
        break;
    case DragKind::Crop:
    case DragKind::Pan:
    case DragKind::None: break;
    }
    return changed;
}

void Editor::cancelDrag() {
    if (drag_ == DragKind::Crop) cropBox_ = cropStart_;
    drag_ = DragKind::None;
    liveItems_.clear();
    drawPath_.clear();
}

CursorKind Editor::cursorAt(const Pointer& p, bool twoPage) const {
    if (pickingWhite_) return CursorKind::Cross;
    if (tool_ == Tool::Crop && drag_ != DragKind::Pan) {
        switch (drag_ == DragKind::Crop ? cropPart_ : cropHitTest(cropBox_, p.pos, p.cropTol, p.cropCorner)) {
        case CropPart::TopLeft:
        case CropPart::BottomRight: return CursorKind::SizeNWSE;
        case CropPart::TopRight:
        case CropPart::BottomLeft: return CursorKind::SizeNESW;
        case CropPart::Left:
        case CropPart::Right: return CursorKind::SizeWE;
        case CropPart::Top:
        case CropPart::Bottom: return CursorKind::SizeNS;
        case CropPart::Inside: return CursorKind::Move;
        case CropPart::None: return CursorKind::Default;
        }
    }
    if (tool_ == Tool::Freehand || drag_ == DragKind::Draw) return CursorKind::Cross;
    if (drag_ == DragKind::Pan || drag_ == DragKind::Move || drag_ == DragKind::Handle) return CursorKind::Move;
    if (const Annotation* sel = selectedItem()) {
        for (const auto& h : handles(*sel))
            if (dist(h.pos, p.pos) <= p.handleTol) return CursorKind::Move;
    }
    // 物件的外框方框內：手指，告訴使用者這裡是物件。
    if (!textEditing() && !twoPage && itemBoxAt(p.pos)) return CursorKind::Hand;
    return CursorKind::Default;
}

uint32_t Editor::itemBoxAt(Pt p) const {
    const auto& items = doc_.items();
    for (int i = static_cast<int>(items.size()) - 1; i >= 0; --i)
        if (bounds(items[i]).contains(p)) return items[i].id;
    return 0;
}

uint32_t Editor::tipItemAt(Pt p, bool twoPage) const {
    if (textEditing() || tool_ != Tool::Select || pickingWhite_ || twoPage) return 0;
    return itemBoxAt(p);
}

uint32_t Editor::textItemAt(const Pointer& p) const {
    const auto& items = doc_.items();
    for (int i = static_cast<int>(items.size()) - 1; i >= 0; --i)
        if (items[i].as<TextShape>() && hitTest(items[i], p.pos, p.tol)) return items[i].id;
    return 0;
}

Editor::Escape Editor::escape() {
    if (cancelCrop()) return Escape::CancelledCrop;
    if (pickingWhite_) {
        pickingWhite_ = false;
        return Escape::StoppedPickWhite;
    }
    if (selected_) {
        selected_ = 0;
        return Escape::Deselected;
    }
    if (tool_ == Tool::Freehand) {
        tool_ = Tool::Select;
        return Escape::LeftFreehand;
    }
    return Escape::Nothing;
}

// ---- 標註 ----

uint32_t Editor::addShape(Kind kind, Pt center, float viewSize) {
    if (!doc_.image()) return 0;
    leaveFreehand();
    selected_ = doc_.addItem(makeShape(kind, center, viewSize, stroke_));
    return selected_;
}

uint32_t Editor::addText(Pt center, float viewSize, const std::wstring& label) {
    if (!doc_.image()) return 0;
    leaveFreehand();
    Annotation a = makeText(center, viewSize, stroke_, text_, label);
    fitText(a);
    selected_ = doc_.addItem(std::move(a));
    return selected_;
}

bool Editor::commitItem(const Annotation& updated) {
    Annotation a = updated;
    fitText(a);
    return doc_.replaceItem(a);
}

bool Editor::nudgeSelected(float dx, float dy) {
    const Annotation* sel = selectedItem();
    if (!sel) return false;
    Annotation a = *sel;
    moveBy(a, dx, dy);
    return commitItem(a);
}

bool Editor::deleteSelected() {
    if (textEditing()) return false;
    if (!doc_.removeItem(selected_)) return false;
    selected_ = 0;
    return true;
}

bool Editor::applyStrokeStyle(const StrokeStyle& s) {
    const Annotation* sel = doc_.findItem(selected_);
    if (!sel || !sel->as<TextShape>()) stroke_ = s;
    if (!sel) return false;
    Annotation a = *sel;
    a.stroke = s;
    return commitItem(a);
}

bool Editor::applyTextStyle(const TextStyle& t) {
    text_ = t;
    const Annotation* sel = doc_.findItem(selected_);
    if (!sel || !sel->as<TextShape>()) return false;
    Annotation a = *sel;
    a.as<TextShape>()->style = t;
    return commitItem(a);
}

// ---- 整張圖 ----

bool Editor::rotateCCW() {
    auto img = doc_.image();
    if (!img) return false;
    DocState next;
    next.image = std::make_shared<Image>(rotateCCW90(*img));
    next.dpi = swapped(doc_.dpi());
    next.items = doc_.items();
    for (auto& a : next.items) {
        rotateCCW90(a, img->width);
        fitText(a);
    }
    doc_.commit(std::move(next));
    return true;
}

bool Editor::resize(int width, int height, double dpi, bool resample) {
    auto img = doc_.image();
    if (!img) return false;
    DocState next;
    next.items = doc_.items();
    next.dpi = {dpi, dpi};
    if (resample && (width != img->width || height != img->height)) {
        next.image = std::make_shared<Image>(mp::resample(*img, width, height));
        float sx = static_cast<float>(width) / img->width, sy = static_cast<float>(height) / img->height;
        for (auto& a : next.items) {
            scaleBy(a, sx, sy);
            fitText(a);
        }
    } else {
        next.image = img; // 只改解析度：同一張底圖，不複製
    }
    doc_.commit(std::move(next));
    return true;
}

void Editor::undo() {
    doc_.undo();
    if (!selectedItem()) selected_ = 0;
}

void Editor::redo() {
    doc_.redo();
    if (!selectedItem()) selected_ = 0;
}

// ---- 調整顏色 ----

bool Editor::beginAdjust() {
    if (adjusting_ || !doc_.image()) return false;
    adjusting_ = true;
    adjustBase_ = doc_.image();
    adjustParams_ = {};
    adjustPreview_.reset();
    return true;
}

void Editor::previewAdjust(const ColorAdjustParams& p) {
    adjustParams_ = p;
    if (!adjustBase_) return;
    adjustPreview_ = p.isIdentity() ? adjustBase_ : std::make_shared<Image>(applyColorAdjust(*adjustBase_, p));
}

bool Editor::commitAdjust(const ColorAdjustParams& p) {
    adjustParams_ = p;
    bool changed = false;
    if (adjustBase_ && !p.isIdentity()) {
        ImagePtr out = adjustPreview_ && adjustPreview_ != adjustBase_ ? adjustPreview_
                                                                        : std::make_shared<Image>(applyColorAdjust(*adjustBase_, p));
        doc_.commitImage(out);
        changed = true;
    }
    cancelAdjust();
    return changed;
}

void Editor::cancelAdjust() {
    adjusting_ = false;
    pickingWhite_ = false;
    adjustPreview_.reset();
    adjustBase_.reset();
}

// ---- 文字編輯 ----

bool Editor::beginTextEdit(uint32_t id) {
    const Annotation* a = doc_.findItem(id);
    const TextShape* t = a ? a->as<TextShape>() : nullptr;
    if (!t) return false;
    editingId_ = id;
    editOriginal_ = t->value;
    editText_ = t->value;
    return true;
}

const TextShape* Editor::editingShape() const {
    const Annotation* a = doc_.findItem(editingId_);
    return a ? a->as<TextShape>() : nullptr;
}

bool Editor::setEditText(const std::wstring& text) {
    // EDIT 控制項給的是 CRLF，統一成 LF（DirectWrite 兩種都認得，但統一才好比對有沒有改）。
    editText_.clear();
    editText_.reserve(text.size());
    for (wchar_t ch : text)
        if (ch != L'\r') editText_ += ch;
    const Annotation* a = doc_.findItem(editingId_);
    const TextShape* cur = editingShape();
    if (!cur || !measure_) return false;
    TextShape typed = *cur;
    typed.value = editText_;
    const float newBottom = typed.rect.top + measure_(typed);
    if (std::abs(newBottom - cur->rect.bottom) <= 0.5f) return false;
    // 高度跟著內容長：直接更新文件裡的物件（不進 undo，結束編輯時才一次 commit）。內容仍是編輯前的。
    Annotation taller = *a;
    taller.as<TextShape>()->rect.bottom = newBottom;
    doc_.amendItem(taller);
    return true;
}

bool Editor::endTextEdit(bool commit) {
    const uint32_t id = editingId_;
    editingId_ = 0;
    const Annotation* a = doc_.findItem(id);
    if (!a || !commit) return false;
    if (editText_.empty()) {
        if (selected_ == id) selected_ = 0;
        return doc_.removeItem(id);
    }
    if (editText_ == editOriginal_) return false;
    Annotation typed = *a;
    typed.as<TextShape>()->value = editText_;
    return commitItem(typed);
}

} // namespace mp
