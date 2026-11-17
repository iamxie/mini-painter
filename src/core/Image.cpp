#include "core/Image.h"

#include <algorithm>
#include <atomic>
#include <new>

namespace mp {

namespace {
std::atomic<size_t> gAllocLimitForTest{0};
} // namespace

void setImageAllocLimitForTest(size_t bytes) { gAllocLimitForTest = bytes; }

size_t detail::pixelAllocBytes(int w, int h) {
    const size_t bytes = static_cast<size_t>(w) * h * 4;
    const size_t limit = gAllocLimitForTest;
    if (limit && bytes > limit) throw std::bad_alloc();
    return bytes;
}

StraightImage toStraight(Image&& img) {
    StraightImage out;
    static_cast<PixelBuffer&>(out) = std::move(static_cast<PixelBuffer&>(img));
    for (int y = 0; y < out.height; ++y) {
        uint8_t* p = out.row(y);
        for (int x = 0; x < out.width; ++x, p += 4) {
            unsigned a = p[3];
            if (a == 255 || a == 0) continue;
            p[0] = unpremultiplyChannel(p[0], a);
            p[1] = unpremultiplyChannel(p[1], a);
            p[2] = unpremultiplyChannel(p[2], a);
        }
    }
    return out;
}

Image toPremultiplied(StraightImage&& img) {
    Image out;
    static_cast<PixelBuffer&>(out) = std::move(static_cast<PixelBuffer&>(img));
    for (int y = 0; y < out.height; ++y) {
        uint8_t* p = out.row(y);
        for (int x = 0; x < out.width; ++x, p += 4) {
            unsigned a = p[3];
            if (a == 255) continue;
            p[0] = premultiplyChannel(p[0], a);
            p[1] = premultiplyChannel(p[1], a);
            p[2] = premultiplyChannel(p[2], a);
        }
    }
    return out;
}

bool hasTransparency(const PixelBuffer& img) {
    for (int y = 0; y < img.height; ++y) {
        const uint8_t* p = img.row(y) + 3;
        for (int x = 0; x < img.width; ++x, p += 4)
            if (*p != 255) return true;
    }
    return false;
}

void compositeOnWhite(Image& img) {
    for (int y = 0; y < img.height; ++y) {
        uint8_t* p = img.row(y);
        for (int x = 0; x < img.width; ++x, p += 4) {
            // premultiplied：c + 白 × (1 − a)。c ≤ a 時不會超過 255；不合規的像素（c > a）停在 255，不繞回暗色。
            unsigned rest = 255u - p[3];
            if (!rest) continue;
            p[0] = static_cast<uint8_t>(std::min(255u, p[0] + rest));
            p[1] = static_cast<uint8_t>(std::min(255u, p[1] + rest));
            p[2] = static_cast<uint8_t>(std::min(255u, p[2] + rest));
            p[3] = 255;
        }
    }
}

} // namespace mp
