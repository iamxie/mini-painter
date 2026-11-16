#include "render/Renderer.h"

#include <algorithm>

namespace mp {

bool Renderer::ensureHwndTarget(HWND hwnd) {
    if (surface_) return true;
    surface_.attach(device_.createHwndTarget(hwnd));
    return static_cast<bool>(surface_);
}

void Renderer::setTarget(ComPtr<ID2D1RenderTarget> rt) {
    discardTarget();
    surface_.attach(std::move(rt));
}

void Renderer::resize(UINT w, UINT h) {
    ComPtr<ID2D1RenderTarget> rt = surface_.rt();
    ComPtr<ID2D1HwndRenderTarget> target;
    if (rt && SUCCEEDED(rt.As(&target))) target->Resize(D2D1::SizeU(std::max(1u, w), std::max(1u, h)));
}

void Renderer::discardTarget() {
    // 點陣圖是 target 的資源：先丟快取，再丟 target。
    bitmaps_.clear();
    mosaic_.clear();
    surface_.reset();
}

void Renderer::endFrame() {
    bitmaps_.endFrame();
    mosaic_.endFrame();
    surface_.endFrame();
}

} // namespace mp
