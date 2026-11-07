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

} // namespace mp
