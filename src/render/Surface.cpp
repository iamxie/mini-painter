#include "render/Surface.h"

namespace mp {

void Surface::attach(ComPtr<ID2D1RenderTarget> rt) {
    brushes_.clear();
    rt_ = std::move(rt);
}

ID2D1SolidColorBrush* Surface::brush(D2D1_COLOR_F color, HRESULT* error) {
    HRESULT hr = rt_ ? S_OK : E_FAIL;
    ID2D1SolidColorBrush* out = nullptr;
    if (SUCCEEDED(hr)) {
        auto& b = brushes_[{color.r, color.g, color.b, color.a}];
        if (!b) hr = device_ && device_->testFaults.brush ? E_OUTOFMEMORY : rt_->CreateSolidColorBrush(color, &b);
        if (SUCCEEDED(hr)) out = b.Get();
        else brushes_.erase({color.r, color.g, color.b, color.a});
    }
    if (error) *error = hr;
    return out;
}

void Surface::drawUiText(const std::wstring& text, const D2D1_RECT_F& rect, float size, D2D1_COLOR_F color,
                         DWRITE_TEXT_ALIGNMENT align, bool bold, DWRITE_PARAGRAPH_ALIGNMENT para, bool clip, bool wrap) {
    if (!rt_ || !device_) return;
    IDWriteTextFormat* fmt = device_->uiFormat(size, bold, align, para, wrap);
    ID2D1SolidColorBrush* b = brush(color);
    if (!fmt || !b) return;
    rt_->DrawText(text.c_str(), static_cast<UINT32>(text.size()), fmt, rect, b,
                  clip ? D2D1_DRAW_TEXT_OPTIONS_CLIP : D2D1_DRAW_TEXT_OPTIONS_NONE);
}

void Surface::endFrame() {
    if (brushes_.size() > kMaxBrushes) brushes_.clear();
}

} // namespace mp
