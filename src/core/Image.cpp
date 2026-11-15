#include "core/Image.h"

#include <algorithm>

namespace mp {

void premultiply(Image& img) {
    for (int y = 0; y < img.height; ++y) {
        uint8_t* p = img.row(y);
        for (int x = 0; x < img.width; ++x, p += 4) {
            unsigned a = p[3];
            if (a == 255) continue;
            p[0] = static_cast<uint8_t>((p[0] * a + 127) / 255);
            p[1] = static_cast<uint8_t>((p[1] * a + 127) / 255);
            p[2] = static_cast<uint8_t>((p[2] * a + 127) / 255);
        }
    }
}

void unpremultiply(Image& img) {
    for (int y = 0; y < img.height; ++y) {
        uint8_t* p = img.row(y);
        for (int x = 0; x < img.width; ++x, p += 4) {
            unsigned a = p[3];
            if (a == 255 || a == 0) continue;
            p[0] = static_cast<uint8_t>(std::min(255u, (p[0] * 255u + a / 2) / a));
            p[1] = static_cast<uint8_t>(std::min(255u, (p[1] * 255u + a / 2) / a));
            p[2] = static_cast<uint8_t>(std::min(255u, (p[2] * 255u + a / 2) / a));
        }
    }
}

bool hasTransparency(const Image& img) {
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
