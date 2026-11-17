#include "render/Flatten.h"

#include <wincodec.h>

#include <algorithm>
#include <cstring>
#include <new>

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

static FlattenResult flattenImpl(RenderDevice& device, const DocState& state, Image& out) {
    FlattenResult fail;
    auto failed = [&](HRESULT hr) {
        fail.hr = hr;
        return fail;
    };
    const Image& src = *state.image;
    if (state.items.empty()) {
        out = src;
        return {true};
    }
    if (src.width > kFlattenMaxSide || src.height > kFlattenMaxSide) return fail;

    ComPtr<IWICImagingFactory> wic;
    HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic));
    if (FAILED(hr)) return failed(hr);
    // 畫布一開始就是底圖的像素（直接複製，不經過繪製），標註畫在上面。
    ComPtr<IWICBitmap> canvas;
    hr = wic->CreateBitmap(src.width, src.height, GUID_WICPixelFormat32bppPBGRA, WICBitmapCacheOnLoad, &canvas);
    if (FAILED(hr)) return failed(hr);
    if (!forEachRow(canvas.Get(), src.width, src.height, WICBitmapLockWrite,
                    [&](int y, BYTE* line) { memcpy(line, src.row(y), src.stride()); }))
        return fail;

    {
        Surface surface(device);
        surface.attach(device.createWicTarget(canvas.Get()));
        if (!surface) return fail;
        // 放大鏡要拿底圖當來源畫，才需要另外上傳一張底圖的點陣圖。
        ImageBitmap base;
        const bool needBase = std::any_of(state.items.begin(), state.items.end(),
                                          [](const Annotation& a) { return a.as<MagnifierShape>() != nullptr; });
        if (needBase) {
            base.bitmap = makeBitmap(surface.rt(), src, &base.scale);
            // 建不起來：放大鏡裡會是空的。被縮小過：放大的內容不是原本的像素。兩種都不能拿去存。
            if (!base.bitmap || base.scale != 1.0f) {
                fail.kind = kindName(Kind::Magnifier);
                return fail;
            }
        }
        MosaicCache mosaic;
        PaintCtx ctx{&surface, state.image, needBase ? &base : nullptr, &mosaic};
        surface.rt()->BeginDraw();
        uint32_t failedId = 0;
        HRESULT drawn = drawAnnotations(ctx, state.items, 0, &failedId);
        hr = surface.rt()->EndDraw(); // 先結束這一輪繪製，再看標註有沒有畫不完整
        if (FAILED(drawn)) {
            for (const Annotation& a : state.items)
                if (a.id == failedId) fail.kind = kindName(a.kind());
            return failed(drawn);
        }
        if (FAILED(hr)) return failed(hr);
    } // target 與它的點陣圖在這裡放掉，再配置 out

    Image result(src.width, src.height);
    if (!forEachRow(canvas.Get(), src.width, src.height, WICBitmapLockRead,
                    [&](int y, BYTE* line) { memcpy(result.row(y), line, result.stride()); }))
        return fail;
    out = std::move(result);
    return {true};
}

FlattenResult flatten(RenderDevice& device, const DocState& state, Image& out) {
    try {
        return flattenImpl(device, state, out);
    } catch (const std::bad_alloc&) {
        // 複製底圖、配置 out 時記憶體不夠：和畫不出來一樣回報失敗（存檔的訊息帶 E_OUTOFMEMORY）。
        FlattenResult r;
        r.hr = E_OUTOFMEMORY;
        return r;
    }
}

} // namespace mp
