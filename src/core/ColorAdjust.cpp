#include "core/ColorAdjust.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace mp {

namespace {

inline float clamp01(float v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); }

inline uint8_t toByte(float v) { return static_cast<uint8_t>(clamp01(v) * 255.0f + 0.5f); }

// 跟 Image.cpp 的 unpremultiply 同一個公式，逐像素用。
inline unsigned unpremul(unsigned c, unsigned a) {
    if (a == 255 || a == 0) return c;
    return std::min(255u, (c * 255u + a / 2) / a);
}

// 跟 Image.cpp 的 premultiply 同一個公式。
inline uint8_t premul(unsigned c, unsigned a) {
    if (a == 255) return static_cast<uint8_t>(c);
    return static_cast<uint8_t>((c * a + 127) / 255);
}

inline float luma601(float r, float g, float b) { return 0.299f * r + 0.587f * g + 0.114f * b; }

// 逐點部分的 LUT：白平衡增益 → 曝光 → 色階（黑白點 + gamma）→ 對比。輸出夾在 0..1。
void buildLut(float lut[256], float gain, const ColorAdjustParams& p) {
    const float expMul = std::exp2(p.exposure);
    const float bp = p.blackPoint;
    const float range = std::max(1e-4f, p.whitePoint - p.blackPoint);
    // 中間調 → gamma 指數：exponent = 2^((midtone - 0.5) * 4)。
    // midtone 0.5 → 1（不變）；midtone 0 → 1/4（提亮）；midtone 1 → 4（壓暗）。單調遞增。
    const float gammaExp = std::exp2((p.midtone - 0.5f) * 4.0f);
    const float contrastMul = 1.0f + p.contrast;
    for (int i = 0; i < 256; ++i) {
        float v = static_cast<float>(i) / 255.0f * gain * expMul;
        v = clamp01((v - bp) / range);
        if (gammaExp != 1.0f) v = std::pow(v, gammaExp);
        v = (v - 0.5f) * contrastMul + 0.5f;
        lut[i] = clamp01(v);
    }
}

// 對 8-bit 平面做 clamp-to-edge 的 box blur（可分離，running sum）。
void boxBlur(const std::vector<uint8_t>& src, std::vector<uint8_t>& dst, int w, int h, int r) {
    const int win = 2 * r + 1;
    std::vector<uint8_t> tmp(src.size());
    // 水平
    for (int y = 0; y < h; ++y) {
        const uint8_t* s = src.data() + static_cast<size_t>(y) * w;
        uint8_t* t = tmp.data() + static_cast<size_t>(y) * w;
        unsigned sum = 0;
        for (int k = -r; k <= r; ++k) sum += s[std::min(std::max(k, 0), w - 1)];
        for (int x = 0; x < w; ++x) {
            t[x] = static_cast<uint8_t>((sum + win / 2) / win);
            const int add = std::min(x + r + 1, w - 1);
            const int sub = std::max(x - r, 0);
            sum += s[add];
            sum -= s[sub];
        }
    }
    // 垂直：逐列維護欄總和，存取順序是連續的。
    std::vector<unsigned> col(static_cast<size_t>(w), 0);
    for (int k = -r; k <= r; ++k) {
        const uint8_t* t = tmp.data() + static_cast<size_t>(std::min(std::max(k, 0), h - 1)) * w;
        for (int x = 0; x < w; ++x) col[x] += t[x];
    }
    for (int y = 0; y < h; ++y) {
        uint8_t* d = dst.data() + static_cast<size_t>(y) * w;
        for (int x = 0; x < w; ++x) d[x] = static_cast<uint8_t>((col[x] + win / 2) / win);
        const uint8_t* ta = tmp.data() + static_cast<size_t>(std::min(y + r + 1, h - 1)) * w;
        const uint8_t* ts = tmp.data() + static_cast<size_t>(std::max(y - r, 0)) * w;
        for (int x = 0; x < w; ++x) col[x] = col[x] + ta[x] - ts[x];
    }
}

} // namespace

bool ColorAdjustParams::isIdentity() const {
    const ColorAdjustParams d;
    return blackPoint == d.blackPoint && midtone == d.midtone && whitePoint == d.whitePoint &&
           exposure == d.exposure && contrast == d.contrast && highlights == d.highlights &&
           shadows == d.shadows && saturation == d.saturation && temperature == d.temperature &&
           tint == d.tint && sepia == d.sepia && clarity == d.clarity && wbGainR == d.wbGainR &&
           wbGainG == d.wbGainG && wbGainB == d.wbGainB;
}

Image applyColorAdjust(const Image& src, const ColorAdjustParams& p) {
    if (p.isIdentity() || src.empty()) return src;

    Image dst(src.width, src.height);
    dst.dpiX = src.dpiX;
    dst.dpiY = src.dpiY;

    float lutR[256], lutG[256], lutB[256];
    buildLut(lutR, p.wbGainR, p);
    buildLut(lutG, p.wbGainG, p);
    buildLut(lutB, p.wbGainB, p);

    const float shK = 0.4f * p.shadows;
    const float hiK = 0.4f * p.highlights;
    const float satMul = 1.0f + p.saturation;
    const float tempD = 0.2f * p.temperature;
    const float tintD = 0.2f * p.tint;
    const float sep = clamp01(p.sepia);
    const float keep = 1.0f - sep;
    const bool doClarity = p.clarity != 0.0f;

    const int w = src.width;
    const int h = src.height;
    std::vector<uint8_t> lumaPlane;
    if (doClarity) lumaPlane.resize(static_cast<size_t>(w) * h);

    // 第一階段：逐點處理。沒有 clarity 時直接 premultiply 寫出；有 clarity 時先寫 straight RGB。
    for (int y = 0; y < h; ++y) {
        const uint8_t* s = src.row(y);
        uint8_t* d = dst.row(y);
        uint8_t* lp = doClarity ? lumaPlane.data() + static_cast<size_t>(y) * w : nullptr;
        for (int x = 0; x < w; ++x, s += 4, d += 4) {
            const unsigned a = s[3];
            float b = lutB[unpremul(s[0], a)];
            float g = lutG[unpremul(s[1], a)];
            float r = lutR[unpremul(s[2], a)];

            // 陰影 / 亮部：以亮度加權的遮罩。陰影遮罩 (1-L)^2，亮部遮罩 L^2。
            float L = luma601(r, g, b);
            const float iL = 1.0f - L;
            const float sh = shK * iL * iL + hiK * L * L;
            r += sh;
            g += sh;
            b += sh;

            // 飽和度
            L = luma601(r, g, b);
            r = L + (r - L) * satMul;
            g = L + (g - L) * satMul;
            b = L + (b - L) * satMul;

            // 色溫 / 色調
            r += tempD;
            b -= tempD;
            g -= tintD;

            // 復古（經典 sepia 矩陣），依 amount 混合
            const float sr = 0.393f * r + 0.769f * g + 0.189f * b;
            const float sg = 0.349f * r + 0.686f * g + 0.168f * b;
            const float sb = 0.272f * r + 0.534f * g + 0.131f * b;
            r = clamp01(r * keep + sr * sep);
            g = clamp01(g * keep + sg * sep);
            b = clamp01(b * keep + sb * sep);

            const uint8_t R = toByte(r), G = toByte(g), B = toByte(b);
            d[3] = static_cast<uint8_t>(a);
            if (doClarity) {
                d[0] = B;
                d[1] = G;
                d[2] = R;
                lp[x] = static_cast<uint8_t>((299u * R + 587u * G + 114u * B + 500u) / 1000u);
            } else {
                d[0] = premul(B, a);
                d[1] = premul(G, a);
                d[2] = premul(R, a);
            }
        }
    }

    if (!doClarity) return dst;

    // 第二階段：clarity。對亮度做 unsharp mask，box blur 半徑 max(3, min(w,h)/100)，amount = clarity*0.6。
    const int radius = std::max(3, std::min(w, h) / 100);
    std::vector<uint8_t> blur(lumaPlane.size());
    boxBlur(lumaPlane, blur, w, h, radius);
    const float amount = p.clarity * 0.6f;
    for (int y = 0; y < h; ++y) {
        uint8_t* d = dst.row(y);
        const uint8_t* lp = lumaPlane.data() + static_cast<size_t>(y) * w;
        const uint8_t* bp = blur.data() + static_cast<size_t>(y) * w;
        for (int x = 0; x < w; ++x, d += 4) {
            const float delta = amount * (static_cast<float>(lp[x]) - static_cast<float>(bp[x]));
            const unsigned a = d[3];
            for (int c = 0; c < 3; ++c) {
                float v = static_cast<float>(d[c]) + delta;
                v = v < 0.0f ? 0.0f : (v > 255.0f ? 255.0f : v);
                d[c] = premul(static_cast<unsigned>(v + 0.5f), a);
            }
        }
    }
    return dst;
}

std::array<uint32_t, 256> luminanceHistogram(const Image& img) {
    std::array<uint32_t, 256> hist{};
    for (int y = 0; y < img.height; ++y) {
        const uint8_t* s = img.row(y);
        for (int x = 0; x < img.width; ++x, s += 4) {
            const unsigned a = s[3];
            const unsigned b = unpremul(s[0], a), g = unpremul(s[1], a), r = unpremul(s[2], a);
            ++hist[(299u * r + 587u * g + 114u * b + 500u) / 1000u];
        }
    }
    return hist;
}

ColorAdjustParams autoLevels(const Image& img, ColorAdjustParams base) {
    if (img.empty()) return base;
    const auto hist = luminanceHistogram(img);
    const double total = static_cast<double>(img.width) * img.height;
    const double lowT = total * 0.001;
    const double highT = total * 0.999;
    int lo = 0, hi = 255;
    bool loFound = false;
    double cum = 0.0;
    for (int i = 0; i < 256; ++i) {
        cum += hist[i];
        if (!loFound && cum >= lowT) {
            lo = i;
            loFound = true;
        }
        if (cum >= highT) {
            hi = i;
            break;
        }
    }
    float bp = static_cast<float>(lo) / 255.0f;
    float wp = static_cast<float>(hi) / 255.0f;
    if (wp <= bp + 0.01f) {
        wp = bp + 0.01f;
        if (wp > 1.0f) {
            wp = 1.0f;
            bp = 0.99f;
        }
    }
    base.blackPoint = bp;
    base.whitePoint = wp;
    return base;
}

void setWhiteBalanceFrom(ColorAdjustParams& p, const Image& img, int x, int y) {
    if (img.empty()) return;
    x = std::min(std::max(x, 0), img.width - 1);
    y = std::min(std::max(y, 0), img.height - 1);
    const int x0 = std::max(0, x - 2), x1 = std::min(img.width - 1, x + 2);
    const int y0 = std::max(0, y - 2), y1 = std::min(img.height - 1, y + 2);
    double sr = 0, sg = 0, sb = 0;
    int n = 0;
    for (int yy = y0; yy <= y1; ++yy) {
        for (int xx = x0; xx <= x1; ++xx) {
            const uint8_t* s = img.px(xx, yy);
            const unsigned a = s[3];
            if (a == 0) continue; // 全透明像素沒有顏色資訊
            sb += unpremul(s[0], a);
            sg += unpremul(s[1], a);
            sr += unpremul(s[2], a);
            ++n;
        }
    }
    if (n == 0) return;
    const float ar = static_cast<float>(sr / n / 255.0);
    const float ag = static_cast<float>(sg / n / 255.0);
    const float ab = static_cast<float>(sb / n / 255.0);
    const float L = luma601(ar, ag, ab);
    auto gainFor = [L](float avg) {
        if (avg < 1.0f / 255.0f) return 1.0f;
        return std::min(5.0f, std::max(0.2f, L / avg));
    };
    p.wbGainR = gainFor(ar);
    p.wbGainG = gainFor(ag);
    p.wbGainB = gainFor(ab);
}

} // namespace mp
