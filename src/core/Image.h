#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace mp {

// 32bpp BGRA，premultiplied alpha，跟 Direct2D 的 D2D1_ALPHA_MODE_PREMULTIPLIED 一致。
// 整張圖是不可變的：任何整圖編輯都產生新的 Image。
struct Image {
    int width = 0;
    int height = 0;
    double dpiX = 96.0;
    double dpiY = 96.0;
    std::vector<uint8_t> pixels; // width * height * 4，stride = width * 4

    Image() = default;
    Image(int w, int h) : width(w), height(h), pixels(static_cast<size_t>(w) * h * 4, 0) {}

    size_t stride() const { return static_cast<size_t>(width) * 4; }
    uint8_t* row(int y) { return pixels.data() + y * stride(); }
    const uint8_t* row(int y) const { return pixels.data() + y * stride(); }
    uint8_t* px(int x, int y) { return row(y) + static_cast<size_t>(x) * 4; }
    const uint8_t* px(int x, int y) const { return row(y) + static_cast<size_t>(x) * 4; }
    bool empty() const { return width <= 0 || height <= 0; }
};

using ImagePtr = std::shared_ptr<const Image>;

// premultiplied <-> straight alpha 互轉（原地）。
void premultiply(Image& img);
void unpremultiply(Image& img);

} // namespace mp
