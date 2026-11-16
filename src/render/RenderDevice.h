#pragma once
#include <windows.h>
#include <d2d1.h>
#include <dwrite.h>
#include <wrl/client.h>

#include <map>
#include <string>
#include <tuple>

#include "core/Annotation.h"
#include "core/Geometry.h"

struct IWICBitmap;

namespace mp {

using Microsoft::WRL::ComPtr;

inline D2D1_COLOR_F toD2D(Color c) {
    return D2D1::ColorF(c.r / 255.0f, c.g / 255.0f, c.b / 255.0f, c.a / 255.0f);
}
inline D2D1_POINT_2F toD2D(Pt p) { return D2D1::Point2F(p.x, p.y); }
inline D2D1_RECT_F toD2D(const Rc& r) { return D2D1::RectF(r.left, r.top, r.right, r.bottom); }

// 霧線沒有真正的模糊，用三層漸淡的描邊模擬：由外到內，每層的不透明度（乘在線條顏色的 alpha 上）與線寬倍數。
// 實際繪製（AnnotationPainter）與線型選單的預覽（Popups）都用這一張表。
struct FuzzyLayer {
    float alpha;
    float widthScale;
};
constexpr FuzzyLayer kFuzzyLayers[] = {{0.12f, 3.0f}, {0.25f, 2.0f}, {0.8f, 1.0f}};

// Direct2D / DirectWrite 的工廠，和不綁 render target 的資源（線型、UI 文字格式）。
// 整個程式一份；render target 各自的資源在 Surface。只在 UI 執行緒用（工廠是 single-threaded）。
class RenderDevice {
public:
    bool init();
    ID2D1Factory* d2d() const { return d2d_.Get(); }
    IDWriteFactory* dwrite() const { return dw_.Get(); }
    ID2D1StrokeStyle* dashStyle() const { return dash_.Get(); }
    ID2D1StrokeStyle* roundStyle() const { return round_.Get(); }

    // 視窗的 render target（大小是 client 區域）。建不出來回 nullptr。
    ComPtr<ID2D1HwndRenderTarget> createHwndTarget(HWND hwnd) const;
    // 畫在記憶體裡的 WIC 點陣圖上（存檔的壓平、測試）。建不出來回 nullptr。
    ComPtr<ID2D1RenderTarget> createWicTarget(IWICBitmap* bitmap) const;

    // 建一個路徑幾何：build 收到 ID2D1GeometrySink&，在裡面 BeginFigure … EndFigure。
    // 建不出來（記憶體不足）回 nullptr，呼叫端要檢查。
    template <class Build>
    ComPtr<ID2D1PathGeometry> pathGeometry(Build build) const {
        ComPtr<ID2D1PathGeometry> geo;
        ComPtr<ID2D1GeometrySink> sink;
        if (!d2d_ || FAILED(d2d_->CreatePathGeometry(&geo)) || FAILED(geo->Open(&sink))) return nullptr;
        build(*sink.Get());
        if (FAILED(sink->Close())) return nullptr;
        return geo;
    }

    // UI 文字的格式。每種組合一個物件，建好之後不再改，所以拿去用的人不會互相影響。
    // 建不出來（size 不合法、記憶體不足）回 nullptr，而且不留在快取裡。
    IDWriteTextFormat* uiFormat(float size, bool bold, DWRITE_TEXT_ALIGNMENT align = DWRITE_TEXT_ALIGNMENT_LEADING,
                                DWRITE_PARAGRAPH_ALIGNMENT para = DWRITE_PARAGRAPH_ALIGNMENT_CENTER, bool wrap = false);
    // 量 UI 文字的寬度（含結尾空白）；height 不是 nullptr 就填高度。maxWidth 小於 1e5 時照這個寬度換行。量不出來回 0。
    float measureUiText(const std::wstring& text, float size, bool bold = false, float maxWidth = 1e6f, float* height = nullptr);

private:
    ComPtr<ID2D1Factory> d2d_;
    ComPtr<IDWriteFactory> dw_;
    ComPtr<ID2D1StrokeStyle> dash_, round_;
    // key：字級（四分之一像素）、粗體、對齊、段落對齊、換行
    std::map<std::tuple<int, bool, int, int, bool>, ComPtr<IDWriteTextFormat>> uiFormats_;
};

} // namespace mp
