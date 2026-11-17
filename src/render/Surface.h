#pragma once
#include <array>
#include <map>
#include <string>

#include "render/RenderDevice.h"

namespace mp {

// 一個 render target 加上只屬於它的資源（畫刷）。資源跟 target 同生共死：換 target 或 reset 時一起丟，
// 所以不會拿到別的 target 的畫刷。主視窗、每個彈出選單、存檔的壓平各有一個。
class Surface {
public:
    Surface() = default;
    explicit Surface(RenderDevice& device) : device_(&device) {}
    Surface(const Surface&) = delete;
    Surface& operator=(const Surface&) = delete;

    void setDevice(RenderDevice& device) { device_ = &device; }
    RenderDevice& device() const { return *device_; }

    // 換成這個 target。原本那個 target 的畫刷一起丟。
    void attach(ComPtr<ID2D1RenderTarget> rt);
    void reset() { attach(nullptr); }
    ID2D1RenderTarget* rt() const { return rt_.Get(); }
    explicit operator bool() const { return rt_ != nullptr; }

    // 這個顏色的畫刷。一個顏色一支，所以同時拿著兩支不同顏色的畫刷不會互相改色。
    // 指標到下一次 endFrame / attach / reset 前有效。沒有 target 或建不出來回 nullptr；error 不是 nullptr 就填原因（成功填 S_OK）。
    ID2D1SolidColorBrush* brush(D2D1_COLOR_F color, HRESULT* error = nullptr);
    ID2D1SolidColorBrush* brush(Color color, HRESULT* error = nullptr) { return brush(toD2D(color), error); }

    // 畫一段 UI 文字。clip：超出 rect 的部分不畫。wrap：照 rect 的寬度換行。
    void drawUiText(const std::wstring& text, const D2D1_RECT_F& rect, float size, D2D1_COLOR_F color,
                    DWRITE_TEXT_ALIGNMENT align = DWRITE_TEXT_ALIGNMENT_LEADING, bool bold = false,
                    DWRITE_PARAGRAPH_ALIGNMENT para = DWRITE_PARAGRAPH_ALIGNMENT_CENTER, bool clip = true, bool wrap = false);

    // 每畫完一幀呼叫：畫刷累積太多（用過很多種顏色）就全部丟掉，下一幀用到的再建。
    void endFrame();
    size_t brushCount() const { return brushes_.size(); }
    // 畫刷超過這個數量，endFrame 就清空。
    static constexpr size_t kMaxBrushes = 256;

private:
    RenderDevice* device_ = nullptr;
    ComPtr<ID2D1RenderTarget> rt_;
    std::map<std::array<float, 4>, ComPtr<ID2D1SolidColorBrush>> brushes_;
};

} // namespace mp
