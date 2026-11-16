#include "render/Flatten.h"

#include <wincodec.h>

#include <algorithm>
#include <cstring>

#include "render/AnnotationPainter.h"

namespace mp {

namespace {
// 鎖住 WIC 點陣圖整張，每一列呼叫 row(y, 那一列在點陣圖裡的位址)。
template <class Row>
bool forEachRow(IWICBitmap* bmp, int width, int height, DWORD mode, Row row) {
    ComPtr<IWICBitmapLock> lock;
    WICRect full{0, 0, width, height};
    if (FAILED(bmp->Lock(&full, mode, &lock))) return false;
    UINT stride = 0, size = 0;
    BYTE* data = nullptr;
    if (FAILED(lock->GetStride(&stride)) || FAILED(lock->GetDataPointer(&size, &data)) || !data) return false;
    for (int y = 0; y < height; ++y) row(y, data + static_cast<size_t>(y) * stride);
    return true;
}
} // namespace

bool flatten(RenderDevice& device, const DocState& state, Image& out) {
    const Image& src = *state.image;
    if (state.items.empty()) {
        out = src;
        return true;
    }
    if (src.width > kFlattenMaxSide || src.height > kFlattenMaxSide) return false;

    ComPtr<IWICImagingFactory> wic;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic)))) return false;
    // 畫布一開始就是底圖的像素（直接複製，不經過繪製），標註畫在上面。
    ComPtr<IWICBitmap> canvas;
    if (FAILED(wic->CreateBitmap(src.width, src.height, GUID_WICPixelFormat32bppPBGRA, WICBitmapCacheOnLoad, &canvas))) return false;
    if (!forEachRow(canvas.Get(), src.width, src.height, WICBitmapLockWrite,
                    [&](int y, BYTE* line) { memcpy(line, src.row(y), src.stride()); }))
        return false;

    {
        Surface surface(device);
        surface.attach(device.createWicTarget(canvas.Get()));
        if (!surface) return false;
        // 放大鏡要拿底圖當來源畫，才需要另外上傳一張底圖的點陣圖。
        ImageBitmap base;
        const bool needBase = std::any_of(state.items.begin(), state.items.end(),
                                          [](const Annotation& a) { return a.kind == Kind::Magnifier; });
        if (needBase) {
            base.bitmap = makeBitmap(surface.rt(), src, &base.scale);
            // 建不起來：放大鏡裡會是空的。被縮小過：放大的內容不是原本的像素。兩種都不能拿去存。
            if (!base.bitmap || base.scale != 1.0f) return false;
        }
        MosaicCache mosaic;
        PaintCtx ctx{&surface, state.image, needBase ? &base : nullptr, &mosaic};
        surface.rt()->BeginDraw();
        drawAnnotations(ctx, state.items);
        if (FAILED(surface.rt()->EndDraw())) return false;
    } // target 與它的點陣圖在這裡放掉，再配置 out

    Image result(src.width, src.height);
    if (!forEachRow(canvas.Get(), src.width, src.height, WICBitmapLockRead,
                    [&](int y, BYTE* line) { memcpy(result.row(y), line, result.stride()); }))
        return false;
    out = std::move(result);
    return true;
}

} // namespace mp
