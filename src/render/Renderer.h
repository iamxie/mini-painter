#pragma once
#include <windows.h>

#include "render/AnnotationPainter.h"
#include "render/BitmapCache.h"
#include "render/Flatten.h"
#include "render/RenderDevice.h"
#include "render/Surface.h"

namespace mp {

// 主視窗的繪製：視窗的 render target，加上跟著它的快取（底圖點陣圖、馬賽克小圖）。
// target 丟掉（D2DERR_RECREATE_TARGET）時快取一起丟。其他零件各自一個檔：
// RenderDevice（工廠、文字格式）、Surface（target 與畫刷）、BitmapCache、AnnotationPainter（標註怎麼畫）、Flatten（存檔）。
class Renderer {
public:
    bool init() { return device_.init(); }
    RenderDevice& device() { return device_; }
    Surface& surface() { return surface_; }

    // 視窗的 render target：還沒有就建。建不出來回 false。
    bool ensureHwndTarget(HWND hwnd);
    // 改畫在別的 render target 上（例如記憶體裡的）。沒有桌面的 session 建不出視窗 target，測試用這個。
    void setTarget(ComPtr<ID2D1RenderTarget> rt);
    void resize(UINT w, UINT h);
    void discardTarget();
    ID2D1RenderTarget* rt() const { return surface_.rt(); }

    // 底圖在目前 target 上的點陣圖（見 BitmapCache）。兩頁並排時兩頁都在裡面。回傳的指標到下一次 endFrame 前有效。
    const ImageBitmap* imageBitmap(const ImagePtr& img) { return bitmaps_.get(rt(), img); }
    const ImageBitmap* scaledBitmap(const ImagePtr& img, int tw, int th, const ImagePtr& prescaled = nullptr) {
        return bitmaps_.getScaled(rt(), img, tw, th, prescaled);
    }
    // 畫 image 與它的標註要用的東西。bmp 是 imageBitmap / scaledBitmap 給的。
    PaintCtx paintCtx(const ImagePtr& image, const ImageBitmap* bmp) { return {&surface_, image, bmp, &mosaic_}; }
    // 每畫完一幀呼叫：這一幀沒用到的底圖點陣圖與馬賽克小圖都丟掉，連同它們握著的來源圖。
    void endFrame();
    size_t cachedBitmapCount() const { return bitmaps_.size(); }
    size_t cachedMosaicCount() const { return mosaic_.size(); }

    // 畫介面用的捷徑。
    ID2D1SolidColorBrush* brush(D2D1_COLOR_F color) { return surface_.brush(color); }
    ID2D1SolidColorBrush* brush(Color color) { return surface_.brush(color); }
    void drawUiText(const std::wstring& text, const D2D1_RECT_F& rect, float size, D2D1_COLOR_F color,
                    DWRITE_TEXT_ALIGNMENT align = DWRITE_TEXT_ALIGNMENT_LEADING, bool bold = false,
                    DWRITE_PARAGRAPH_ALIGNMENT para = DWRITE_PARAGRAPH_ALIGNMENT_CENTER, bool clip = true, bool wrap = false) {
        surface_.drawUiText(text, rect, size, color, align, bold, para, clip, wrap);
    }
    float measureUiText(const std::wstring& text, float size, bool bold = false, float maxWidth = 1e6f, float* height = nullptr) {
        return device_.measureUiText(text, size, bold, maxWidth, height);
    }

private:
    RenderDevice device_;
    Surface surface_{device_};
    BitmapCache bitmaps_;
    MosaicCache mosaic_;
};

} // namespace mp
