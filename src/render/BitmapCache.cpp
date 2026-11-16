#include "render/BitmapCache.h"

#include <algorithm>
#include <cmath>

#include "core/ImageOps.h"
#include "core/ViewFit.h"

namespace mp {

namespace {
ComPtr<ID2D1Bitmap> upload(ID2D1RenderTarget* rt, const Image& img) {
    ComPtr<ID2D1Bitmap> bmp;
    if (img.empty() ||
        FAILED(rt->CreateBitmap(D2D1::SizeU(img.width, img.height), img.pixels.data(), static_cast<UINT32>(img.stride()),
                                D2D1::BitmapProperties(D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED)),
                                &bmp)))
        return nullptr;
    return bmp;
}
} // namespace

ComPtr<ID2D1Bitmap> makeBitmap(ID2D1RenderTarget* rt, const Image& img, float* scale) {
    *scale = 1.0f;
    UINT maxSize = rt->GetMaximumBitmapSize();
    if (static_cast<UINT>(img.width) <= maxSize && static_cast<UINT>(img.height) <= maxSize) return upload(rt, img);
    float s = static_cast<float>(maxSize) / std::max(img.width, img.height);
    Image shrunk = resampleFast(img, std::max(1, static_cast<int>(img.width * s)), std::max(1, static_cast<int>(img.height * s)));
    *scale = static_cast<float>(shrunk.width) / img.width;
    return upload(rt, shrunk);
}

const ImageBitmap* BitmapCache::get(ID2D1RenderTarget* rt, const ImagePtr& img) {
    if (!rt || !img) return nullptr;
    for (auto& e : entries_) {
        if (e.src == img && e.tw == 0) {
            e.used = true;
            return &e.bmp;
        }
    }
    Entry e;
    e.src = img;
    e.used = true;
    e.bmp.bitmap = makeBitmap(rt, *img, &e.bmp.scale);
    if (!e.bmp.bitmap) return nullptr;
    entries_.push_back(std::move(e));
    return &entries_.back().bmp;
}

const ImageBitmap* BitmapCache::getScaled(ID2D1RenderTarget* rt, const ImagePtr& img, int tw, int th, const ImagePtr& prescaled) {
    if (!rt || !img || tw <= 0 || th <= 0) return nullptr;
    for (auto& e : entries_) {
        if (e.src == img && e.tw == tw && e.th == th) {
            e.used = true;
            return &e.bmp;
        }
    }
    UINT maxSize = rt->GetMaximumBitmapSize();
    // 放大到超過 GPU 上限或 kHqMaxPixels 就不做，記憶體和時間都不值得。
    if (static_cast<UINT>(tw) > maxSize || static_cast<UINT>(th) > maxSize || static_cast<long long>(tw) * th > kHqMaxPixels)
        return nullptr;
    ComPtr<ID2D1Bitmap> bmp;
    if (prescaled && prescaled->width == tw && prescaled->height == th) bmp = upload(rt, *prescaled);
    else bmp = upload(rt, resample(*img, tw, th));
    if (!bmp) return nullptr;
    Entry e;
    e.src = img;
    e.tw = tw;
    e.th = th;
    e.used = true;
    e.bmp.bitmap = bmp;
    e.bmp.scale = static_cast<float>(tw) / img->width;
    entries_.push_back(std::move(e));
    return &entries_.back().bmp;
}

void BitmapCache::endFrame() {
    entries_.remove_if([](Entry& e) {
        bool drop = !e.used;
        e.used = false;
        return drop;
    });
}

const MosaicCache::Tile* MosaicCache::get(ID2D1RenderTarget* rt, const ImagePtr& image, const Annotation& a) {
    if (!rt || !image) return nullptr;
    Entry& entry = entries_[a.id];
    if (!entry.tile.bitmap || !(entry.rect == a.rect) || entry.block != a.blockSize || entry.source != image) {
        int x0 = static_cast<int>(std::floor(a.rect.left)), y0 = static_cast<int>(std::floor(a.rect.top));
        int x1 = static_cast<int>(std::ceil(a.rect.right)), y1 = static_cast<int>(std::ceil(a.rect.bottom));
        entry = {};
        entry.rect = a.rect;
        entry.block = a.blockSize;
        entry.source = image;
        entry.tile.x0 = std::max(0, x0);
        entry.tile.y0 = std::max(0, y0);
        entry.tile.bitmap = upload(rt, mosaicBlocks(*image, x0, y0, x1, y1, a.blockSize));
    }
    entry.used = true;
    return entry.tile.bitmap ? &entry.tile : nullptr;
}

void MosaicCache::endFrame() {
    for (auto it = entries_.begin(); it != entries_.end();) {
        if (!it->second.used) {
            it = entries_.erase(it);
            continue;
        }
        it->second.used = false;
        ++it;
    }
}

} // namespace mp
