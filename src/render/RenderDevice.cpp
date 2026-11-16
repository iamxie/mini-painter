#include "render/RenderDevice.h"

#include "core/Fonts.h"

#include <wincodec.h>

#include <algorithm>

#pragma comment(lib, "d2d1")
#pragma comment(lib, "dwrite")

namespace mp {

namespace {
// 程式裡所有 render target 都用同一種格式：32bpp BGRA、premultiplied、96 DPI（座標就是像素）。
D2D1_RENDER_TARGET_PROPERTIES targetProps() {
    return D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_DEFAULT,
                                        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96.0f, 96.0f);
}
} // namespace

bool RenderDevice::init() {
    HRESULT hr = D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory), nullptr,
                                   reinterpret_cast<void**>(d2d_.GetAddressOf()));
    if (FAILED(hr)) return false;
    hr = DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                             reinterpret_cast<IUnknown**>(dw_.GetAddressOf()));
    if (FAILED(hr)) return false;
    const float kMiterLimit = 10.0f;
    auto style = [&](D2D1_DASH_STYLE dash, ComPtr<ID2D1StrokeStyle>& out) {
        return SUCCEEDED(d2d_->CreateStrokeStyle(
            D2D1::StrokeStyleProperties(D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_LINE_JOIN_ROUND,
                                        kMiterLimit, dash, 0.0f),
            nullptr, 0, &out));
    };
    return style(D2D1_DASH_STYLE_DASH, dash_) && style(D2D1_DASH_STYLE_SOLID, round_);
}

ComPtr<ID2D1HwndRenderTarget> RenderDevice::createHwndTarget(HWND hwnd) const {
    RECT rc;
    GetClientRect(hwnd, &rc);
    D2D1_SIZE_U size = D2D1::SizeU(std::max(1l, rc.right - rc.left), std::max(1l, rc.bottom - rc.top));
    ComPtr<ID2D1HwndRenderTarget> target;
    if (!d2d_ || FAILED(d2d_->CreateHwndRenderTarget(targetProps(), D2D1::HwndRenderTargetProperties(hwnd, size), &target)))
        return nullptr;
    return target;
}

ComPtr<ID2D1RenderTarget> RenderDevice::createWicTarget(IWICBitmap* bitmap) const {
    ComPtr<ID2D1RenderTarget> target;
    if (!d2d_ || !bitmap || FAILED(d2d_->CreateWicBitmapRenderTarget(bitmap, targetProps(), &target))) return nullptr;
    return target;
}

IDWriteTextFormat* RenderDevice::uiFormat(float size, bool bold, DWRITE_TEXT_ALIGNMENT align, DWRITE_PARAGRAPH_ALIGNMENT para,
                                          bool wrap) {
    auto key = std::make_tuple(static_cast<int>(size * 4), bold, static_cast<int>(align), static_cast<int>(para), wrap);
    auto it = uiFormats_.find(key);
    if (it != uiFormats_.end()) return it->second.Get();
    ComPtr<IDWriteTextFormat> fmt;
    if (!dw_ || FAILED(dw_->CreateTextFormat(kUiFont, nullptr,
                                             bold ? DWRITE_FONT_WEIGHT_SEMI_BOLD : DWRITE_FONT_WEIGHT_NORMAL,
                                             DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, size, kTextLocale, &fmt)))
        return nullptr;
    fmt->SetTextAlignment(align);
    fmt->SetParagraphAlignment(para);
    fmt->SetWordWrapping(wrap ? DWRITE_WORD_WRAPPING_WRAP : DWRITE_WORD_WRAPPING_NO_WRAP);
    uiFormats_[key] = fmt;
    return fmt.Get();
}

float RenderDevice::measureUiText(const std::wstring& text, float size, bool bold, float maxWidth, float* height) {
    const bool wrap = maxWidth < 1e5f;
    IDWriteTextFormat* fmt = uiFormat(size, bold, DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_NEAR, wrap);
    ComPtr<IDWriteTextLayout> layout;
    if (!fmt || FAILED(dw_->CreateTextLayout(text.c_str(), static_cast<UINT32>(text.size()), fmt, maxWidth, 1e6f, &layout)))
        return 0;
    DWRITE_TEXT_METRICS m{};
    layout->GetMetrics(&m);
    if (height) *height = m.height;
    return m.widthIncludingTrailingWhitespace;
}

} // namespace mp
