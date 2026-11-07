#include "core/ImageOps.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <thread>
#include <vector>

namespace mp {

Image rotateCCW90(const Image& src) {
    Image out(src.height, src.width);
    out.dpiX = src.dpiY;
    out.dpiY = src.dpiX;
    // 原圖 (x, y) -> 新圖 (y, W-1-x)
    for (int y = 0; y < src.height; ++y) {
        const uint8_t* s = src.row(y);
        for (int x = 0; x < src.width; ++x) {
            std::memcpy(out.px(y, src.width - 1 - x), s + static_cast<size_t>(x) * 4, 4);
        }
    }
    return out;
}

static Image rotateCW90(const Image& src) {
    Image out(src.height, src.width);
    out.dpiX = src.dpiY;
    out.dpiY = src.dpiX;
    for (int y = 0; y < src.height; ++y) {
        const uint8_t* s = src.row(y);
        for (int x = 0; x < src.width; ++x) {
            std::memcpy(out.px(src.height - 1 - y, x), s + static_cast<size_t>(x) * 4, 4);
        }
    }
    return out;
}

static Image flipH(const Image& src) {
    Image out = src;
    for (int y = 0; y < src.height; ++y) {
        const uint8_t* s = src.row(y);
        for (int x = 0; x < src.width; ++x) {
            std::memcpy(out.px(src.width - 1 - x, y), s + static_cast<size_t>(x) * 4, 4);
        }
    }
    return out;
}

static Image flipV(const Image& src) {
    Image out = src;
    for (int y = 0; y < src.height; ++y) {
        std::memcpy(out.row(src.height - 1 - y), src.row(y), src.stride());
    }
    return out;
}

Image applyExifOrientation(const Image& src, int orientation) {
    switch (orientation) {
    case 2: return flipH(src);
    case 3: return rotateCW90(rotateCW90(src));
    case 4: return flipV(src);
    case 5: return rotateCW90(flipV(src));
    case 6: return rotateCW90(src);
    case 7: return rotateCCW90(flipV(src));
    case 8: return rotateCCW90(src);
    default: return src;
    }
}

Image resampleFast(const Image& src, int newW, int newH) {
    newW = std::max(1, newW);
    newH = std::max(1, newH);
    if (newW == src.width && newH == src.height) return src;
    Image out(newW, newH);
    out.dpiX = src.dpiX;
    out.dpiY = src.dpiY;
    if (src.empty()) return out;

    const bool shrinkX = newW < src.width;
    const bool shrinkY = newH < src.height;
    const float sx = static_cast<float>(src.width) / newW;
    const float sy = static_cast<float>(src.height) / newH;

    for (int oy = 0; oy < newH; ++oy) {
        uint8_t* o = out.row(oy);
        for (int ox = 0; ox < newW; ++ox, o += 4) {
            float acc[4] = {0, 0, 0, 0};
            if (shrinkX || shrinkY) {
                // 面積平均：取來源對應的格子，整數邊界近似。
                int x0 = static_cast<int>(ox * sx), x1 = std::max(x0 + 1, static_cast<int>((ox + 1) * sx));
                int y0 = static_cast<int>(oy * sy), y1 = std::max(y0 + 1, static_cast<int>((oy + 1) * sy));
                x1 = std::min(x1, src.width);
                y1 = std::min(y1, src.height);
                int n = 0;
                for (int y = y0; y < y1; ++y) {
                    const uint8_t* s = src.px(x0, y);
                    for (int x = x0; x < x1; ++x, s += 4) {
                        acc[0] += s[0]; acc[1] += s[1]; acc[2] += s[2]; acc[3] += s[3];
                        ++n;
                    }
                }
                for (float& v : acc) v /= static_cast<float>(n);
            } else {
                // 雙線性
                float fx = (ox + 0.5f) * sx - 0.5f, fy = (oy + 0.5f) * sy - 0.5f;
                int x0 = static_cast<int>(std::floor(fx)), y0 = static_cast<int>(std::floor(fy));
                float tx = fx - x0, ty = fy - y0;
                int xa = std::clamp(x0, 0, src.width - 1), xb = std::clamp(x0 + 1, 0, src.width - 1);
                int ya = std::clamp(y0, 0, src.height - 1), yb = std::clamp(y0 + 1, 0, src.height - 1);
                const uint8_t* p00 = src.px(xa, ya);
                const uint8_t* p10 = src.px(xb, ya);
                const uint8_t* p01 = src.px(xa, yb);
                const uint8_t* p11 = src.px(xb, yb);
                for (int c = 0; c < 4; ++c) {
                    float top = p00[c] + (p10[c] - p00[c]) * tx;
                    float bot = p01[c] + (p11[c] - p01[c]) * tx;
                    acc[c] = top + (bot - top) * ty;
                }
            }
            for (int c = 0; c < 4; ++c) o[c] = static_cast<uint8_t>(std::clamp(acc[c] + 0.5f, 0.0f, 255.0f));
        }
    }
    return out;
}

namespace {

// 一個輸出像素對應的來源像素區間與權重（已正規化到總和 1）。
struct Taps {
    int start = 0;
    std::vector<float> w;
};

float lanczos3(float x) {
    x = std::abs(x);
    if (x < 1e-6f) return 1.0f;
    if (x >= 3.0f) return 0.0f;
    const float pi = 3.14159265358979f;
    float px = pi * x;
    return 3.0f * std::sin(px) * std::sin(px / 3.0f) / (px * px);
}

// 來源長度 n 對到輸出長度 m 的每個輸出位置的 taps。縮小時 support 放大 n/m 倍。
std::vector<Taps> makeTaps(int n, int m) {
    std::vector<Taps> out(static_cast<size_t>(m));
    float scale = static_cast<float>(n) / m;          // 每個輸出像素跨幾個來源像素
    float filterScale = std::max(1.0f, scale);        // 縮小時濾波器變寬
    float support = 3.0f * filterScale;
    for (int i = 0; i < m; ++i) {
        float center = (i + 0.5f) * scale - 0.5f;
        int lo = static_cast<int>(std::floor(center - support)) + 1;
        int hi = static_cast<int>(std::floor(center + support));
        lo = std::max(lo, 0);
        hi = std::min(hi, n - 1);
        if (hi < lo) { lo = std::clamp(static_cast<int>(center), 0, n - 1); hi = lo; }
        Taps& t = out[static_cast<size_t>(i)];
        t.start = lo;
        t.w.resize(static_cast<size_t>(hi - lo + 1));
        float sum = 0;
        for (int x = lo; x <= hi; ++x) {
            float v = lanczos3((x - center) / filterScale);
            t.w[static_cast<size_t>(x - lo)] = v;
            sum += v;
        }
        if (std::abs(sum) < 1e-6f) { std::fill(t.w.begin(), t.w.end(), 0.0f); t.w[0] = 1.0f; sum = 1.0f; }
        for (float& v : t.w) v /= sum;
    }
    return out;
}

// 把 [0, rows) 切給幾條執行緒跑 fn(y0, y1)。
template <class Fn>
void parallelRows(int rows, Fn fn) {
    unsigned hw = std::thread::hardware_concurrency();
    int threads = static_cast<int>(std::clamp<unsigned>(hw ? hw : 2, 1, 16));
    if (rows < 64) threads = 1;
    if (threads == 1) { fn(0, rows); return; }
    std::vector<std::thread> pool;
    int chunk = (rows + threads - 1) / threads;
    for (int t = 0; t < threads; ++t) {
        int y0 = t * chunk, y1 = std::min(rows, y0 + chunk);
        if (y0 >= y1) break;
        pool.emplace_back([=] { fn(y0, y1); });
    }
    for (auto& th : pool) th.join();
}

} // namespace

Image resample(const Image& src, int newW, int newH) {
    newW = std::max(1, newW);
    newH = std::max(1, newH);
    if (newW == src.width && newH == src.height) return src;
    Image out(newW, newH);
    out.dpiX = src.dpiX;
    out.dpiY = src.dpiY;
    if (src.empty()) return out;

    // 先水平（src.height 列 -> newW 寬），再垂直。中間用 8 位元，誤差可接受。
    std::vector<Taps> tx = makeTaps(src.width, newW);
    std::vector<Taps> ty = makeTaps(src.height, newH);
    Image mid(newW, src.height);
    parallelRows(src.height, [&](int y0, int y1) {
        for (int y = y0; y < y1; ++y) {
            const uint8_t* s = src.row(y);
            uint8_t* o = mid.row(y);
            for (int x = 0; x < newW; ++x, o += 4) {
                const Taps& t = tx[static_cast<size_t>(x)];
                float acc[4] = {0, 0, 0, 0};
                const uint8_t* p = s + static_cast<size_t>(t.start) * 4;
                for (float w : t.w) {
                    acc[0] += w * p[0]; acc[1] += w * p[1]; acc[2] += w * p[2]; acc[3] += w * p[3];
                    p += 4;
                }
                for (int c = 0; c < 4; ++c) o[c] = static_cast<uint8_t>(std::clamp(acc[c] + 0.5f, 0.0f, 255.0f));
            }
        }
    });
    parallelRows(newH, [&](int y0, int y1) {
        std::vector<float> acc(static_cast<size_t>(newW) * 4);
        for (int y = y0; y < y1; ++y) {
            const Taps& t = ty[static_cast<size_t>(y)];
            std::fill(acc.begin(), acc.end(), 0.0f);
            for (size_t k = 0; k < t.w.size(); ++k) {
                float w = t.w[k];
                const uint8_t* s = mid.row(t.start + static_cast<int>(k));
                for (size_t i = 0; i < acc.size(); ++i) acc[i] += w * s[i];
            }
            uint8_t* o = out.row(y);
            for (size_t i = 0; i < acc.size(); ++i) o[i] = static_cast<uint8_t>(std::clamp(acc[i] + 0.5f, 0.0f, 255.0f));
        }
    });
    return out;
}

Image mosaicBlocks(const Image& src, int x0, int y0, int x1, int y1, int block) {
    x0 = std::clamp(x0, 0, src.width);
    x1 = std::clamp(x1, 0, src.width);
    y0 = std::clamp(y0, 0, src.height);
    y1 = std::clamp(y1, 0, src.height);
    if (block <= 0 || x1 <= x0 || y1 <= y0) return {};
    int bw = (x1 - x0 + block - 1) / block;
    int bh = (y1 - y0 + block - 1) / block;
    Image out(bw, bh);
    for (int by = 0; by < bh; ++by) {
        uint8_t* o = out.row(by);
        for (int bx = 0; bx < bw; ++bx, o += 4) {
            int sx0 = x0 + bx * block, sx1 = std::min(sx0 + block, x1);
            int sy0 = y0 + by * block, sy1 = std::min(sy0 + block, y1);
            unsigned acc[4] = {0, 0, 0, 0};
            unsigned n = 0;
            for (int y = sy0; y < sy1; ++y) {
                const uint8_t* s = src.px(sx0, y);
                for (int x = sx0; x < sx1; ++x, s += 4) {
                    acc[0] += s[0]; acc[1] += s[1]; acc[2] += s[2]; acc[3] += s[3];
                    ++n;
                }
            }
            for (int c = 0; c < 4; ++c) o[c] = static_cast<uint8_t>((acc[c] + n / 2) / n);
        }
    }
    return out;
}

} // namespace mp
