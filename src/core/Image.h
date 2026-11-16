#pragma once
#include <algorithm>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace mp {

// 32bpp BGRA 的像素緩衝區。alpha 有沒有乘進顏色由下面兩個型別區分，這裡只管尺寸與位址。
struct PixelBuffer {
    int width = 0;
    int height = 0;
    std::vector<uint8_t> pixels; // width * height * 4，stride = width * 4

    PixelBuffer() = default;
    PixelBuffer(int w, int h) : width(w), height(h), pixels(static_cast<size_t>(w) * h * 4, 0) {}

    size_t stride() const { return static_cast<size_t>(width) * 4; }
    uint8_t* row(int y) { return pixels.data() + y * stride(); }
    const uint8_t* row(int y) const { return pixels.data() + y * stride(); }
    uint8_t* px(int x, int y) { return row(y) + static_cast<size_t>(x) * 4; }
    const uint8_t* px(int x, int y) const { return row(y) + static_cast<size_t>(x) * 4; }
    bool empty() const { return width <= 0 || height <= 0; }
};

// premultiplied alpha，跟 Direct2D 的 D2D1_ALPHA_MODE_PREMULTIPLIED 一致。程式裡流通的圖都是這種：
// 縮放、合成、繪製都假設 premultiplied。
// 整張圖是不可變的：任何整圖編輯都產生新的 Image（共用時一律拿 ImagePtr，指向 const）。
struct Image : PixelBuffer {
    using PixelBuffer::PixelBuffer;
};

// straight alpha（顏色沒有乘 alpha）。只有編碼器要這種，所以只出現在存檔的最後一段。
// 和 Image 是不同型別，不能互相代入：把 straight 的圖交給假設 premultiplied 的運算會算錯。
struct StraightImage : PixelBuffer {
    using PixelBuffer::PixelBuffer;
};

using ImagePtr = std::shared_ptr<const Image>;

// 解析度（每英吋像素）。不放在像素緩衝區裡：只改解析度不必複製整張圖（文件的狀態 DocState 另外帶著）。
struct Dpi {
    double x = 96.0;
    double y = 96.0;
};
inline bool operator==(Dpi a, Dpi b) { return a.x == b.x && a.y == b.y; }
inline bool operator!=(Dpi a, Dpi b) { return !(a == b); }
// 寬高對調（轉 90 度）之後的解析度。
inline Dpi swapped(Dpi d) { return {d.y, d.x}; }

// 單一色版的換算。整張圖與逐像素的運算（ColorAdjust）共用同一個公式。
inline uint8_t premultiplyChannel(unsigned c, unsigned a) {
    if (a == 255) return static_cast<uint8_t>(c);
    return static_cast<uint8_t>((c * a + 127) / 255);
}
inline uint8_t unpremultiplyChannel(unsigned c, unsigned a) {
    if (a == 255 || a == 0) return static_cast<uint8_t>(c);
    return static_cast<uint8_t>(std::min(255u, (c * 255u + a / 2) / a));
}

// 互轉。像素緩衝區直接搬過去、原地換算，不另外配置。
StraightImage toStraight(Image&& img);
Image toPremultiplied(StraightImage&& img);

// 有沒有 alpha 不是 255 的像素。
bool hasTransparency(const PixelBuffer& img);
// 疊在白底上（原地）：透明處變白，半透明處和白色混合，結果全部不透明。
void compositeOnWhite(Image& img);

} // namespace mp
