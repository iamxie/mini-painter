#pragma once
#include <list>
#include <map>

#include "core/Annotation.h"
#include "core/Image.h"
#include "render/RenderDevice.h"

namespace mp {

// 底圖在某個 render target 上的點陣圖。圖太大時會縮小後再建，scale 記錄縮了多少。
struct ImageBitmap {
    ComPtr<ID2D1Bitmap> bitmap;
    float scale = 1.0f; // bitmap 像素 = 圖片像素 * scale
};

// 把圖上傳成 rt 的點陣圖。超過 rt 的邊長上限時先縮小，*scale 是縮的倍率（沒縮是 1）。建不出來回 nullptr。
ComPtr<ID2D1Bitmap> makeBitmap(ID2D1RenderTarget* rt, const Image& img, float* scale);

// 底圖點陣圖的快取，屬於一個 render target（target 換了要 clear）。只留上一幀畫過的：每畫完一幀呼叫 endFrame，
// 這一幀沒取過的連同它握著的來源圖一起放掉，所以翻走的圖、undo 歷史裡的圖、舊的調色預覽、舊倍率的縮圖不會留在記憶體。
// 每一筆都握著來源的 ImagePtr：圖還在快取裡，它的位址就不會被別張圖重用。
class BitmapCache {
public:
    // 原尺寸的點陣圖。回傳的指標到下一次 endFrame / clear 前有效（同一幀再取別張不會讓它失效）。
    const ImageBitmap* get(ID2D1RenderTarget* rt, const ImagePtr& img);
    // 高品質縮放：先用 Lanczos3 重取樣到 tw×th 再上傳。prescaled 是 tw×th 的話（預先解碼時背景縮好的）直接上傳，不重算。
    // 超過 rt 的邊長上限或 kHqMaxPixels 回 nullptr，呼叫端退回 get。
    const ImageBitmap* getScaled(ID2D1RenderTarget* rt, const ImagePtr& img, int tw, int th, const ImagePtr& prescaled = nullptr);
    void endFrame();
    void clear() { entries_.clear(); }
    size_t size() const { return entries_.size(); }

private:
    struct Entry {
        ImagePtr src;
        int tw = 0, th = 0; // 0 = 原尺寸
        ImageBitmap bmp;
        bool used = false; // 這一幀取過
    };
    // 用 list：同一幀裡先取第一頁再取第二頁，第一頁的指標不能因為新增而失效。
    std::list<Entry> entries_;
};

// 馬賽克物件的小圖（一格一個像素），依物件 id 快取，屬於一個 render target。和 BitmapCache 一樣只留上一幀畫過的。
class MosaicCache {
public:
    struct Tile {
        ComPtr<ID2D1Bitmap> bitmap; // 一格一個像素
        int x0 = 0, y0 = 0;         // 小圖對應的圖片區域左上角
    };
    // 馬賽克物件 m（id 是那個物件的 id）蓋住的那塊底圖做成的小圖。物件的矩形、格子大小或底圖變了就重做。
    // 沒有可畫的（整個在圖外）或建不出點陣圖回 nullptr；error 不是 nullptr 就分得出兩者：整個在圖外填 S_OK，
    // 建不出來填原因。failForTest：測試用，當成點陣圖建不出來（RenderDevice::testFaults.bitmap）。
    // 回傳的指標到下一次 get / endFrame / clear 前有效。
    const Tile* get(ID2D1RenderTarget* rt, const ImagePtr& image, uint32_t id, const MosaicShape& m, HRESULT* error = nullptr,
                    bool failForTest = false);
    void endFrame();
    void clear() { entries_.clear(); }
    size_t size() const { return entries_.size(); }

private:
    struct Entry {
        Rc rect;
        int block = 0;
        ImagePtr source; // 握著來源：拿指標比對「底圖換了沒」才不會被重用的位址騙到
        Tile tile;
        bool used = false;
    };
    std::map<uint32_t, Entry> entries_;
};

} // namespace mp
