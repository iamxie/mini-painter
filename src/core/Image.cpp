#include "core/Image.h"

namespace mp {

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
            unsigned rest = 255u - p[3]; // premultiplied：c + 白 × (1 − a)，c ≤ a 所以不會超過 255
            if (!rest) continue;
            p[0] = static_cast<uint8_t>(p[0] + rest);
            p[1] = static_cast<uint8_t>(p[1] + rest);
            p[2] = static_cast<uint8_t>(p[2] + rest);
            p[3] = 255;
        }
    }
}

} // namespace mp
