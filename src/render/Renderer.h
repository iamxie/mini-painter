#pragma once
#include <windows.h>
#include <d2d1.h>
#include <dwrite.h>
#include <wrl/client.h>

#include <list>
#include <map>
#include <string>
#include <vector>

#include "core/Annotation.h"
#include "core/Document.h"
#include "core/Image.h"

namespace mp {

using Microsoft::WRL::ComPtr;

inline D2D1_COLOR_F toD2D(Color c) {
    return D2D1::ColorF(c.r / 255.0f, c.g / 255.0f, c.b / 255.0f, c.a / 255.0f);
}
inline D2D1_POINT_2F toD2D(Pt p) { return D2D1::Point2F(p.x, p.y); }
inline D2D1_RECT_F toD2D(const Rc& r) { return D2D1::RectF(r.left, r.top, r.right, r.bottom); }

// 底圖在某個 render target 上的點陣圖。圖太大時會縮小後再建，scale 記錄縮了多少。
struct ImageBitmap {
    ComPtr<ID2D1Bitmap> bitmap;
    const Image* source = nullptr;
    float scale = 1.0f; // bitmap 像素 = 圖片像素 * scale
};

// 馬賽克小圖的快取，依物件 id。
struct MosaicEntry {
    Rc rect;
    int block = 0;
    const Image* source = nullptr;
    ComPtr<ID2D1Bitmap> bitmap;
    int x0 = 0, y0 = 0; // 小圖對應的圖片區域左上角
    bool used = false;  // 這一幀畫過（Renderer::endFrame 用）
};

// 畫標註時需要的東西。
struct DrawCtx {
    ID2D1RenderTarget* rt = nullptr;
    const Image* image = nullptr;
    const ImageBitmap* imageBmp = nullptr;
    std::map<uint32_t, MosaicEntry>* mosaicCache = nullptr;
};

class Renderer {
public:
    bool init();
    ID2D1Factory* d2d() const { return d2d_.Get(); }
    IDWriteFactory* dwrite() const { return dw_.Get(); }

    // 視窗用的 render target。
    bool ensureHwndTarget(HWND hwnd);
    // 改畫在別的 render target 上（例如記憶體裡的）。沒有桌面的 session 建不出視窗 target，測試用這個。
    void setTarget(ComPtr<ID2D1RenderTarget> rt);
    void resize(UINT w, UINT h);
    void discardTarget();
    ID2D1RenderTarget* rt() const { return rt_.Get(); }
    // 畫刷綁定 render target，所以依 target 取。brush() 是視窗 target 的捷徑。
    ID2D1SolidColorBrush* brushFor(ID2D1RenderTarget* rt, D2D1_COLOR_F color);
    ID2D1SolidColorBrush* brushFor(ID2D1RenderTarget* rt, Color color) { return brushFor(rt, toD2D(color)); }
    ID2D1SolidColorBrush* brush(D2D1_COLOR_F color) { return brushFor(rt_.Get(), color); }
    ID2D1SolidColorBrush* brush(Color color) { return brush(toD2D(color)); }
    // 某個 render target 不再使用時呼叫，釋放它的畫刷。
    void forgetTarget(ID2D1RenderTarget* rt);

    // 底圖在視窗 render target 上的點陣圖。快取只留上一幀畫過的（見 endFrame），兩頁並排時兩頁都在裡面。
    // 回傳的指標到下一次 endFrame 前有效。
    const ImageBitmap* imageBitmap(const ImagePtr& img);
    // 高品質縮放：底圖先用 Lanczos3 重取樣到 tw×th 再上傳，同一個快取。prescaled 是 tw×th 的話（預先解碼時背景縮好的）
    // 直接上傳，不重算。超過 GPU 上限或太大時回傳 nullptr，呼叫端退回 imageBitmap。
    const ImageBitmap* scaledBitmap(const ImagePtr& img, int tw, int th, const ImagePtr& prescaled = nullptr);
    std::map<uint32_t, MosaicEntry>& mosaicCache() { return mosaic_; }
    // 每畫完一幀呼叫：這一幀沒用到的底圖點陣圖與馬賽克小圖都丟掉，連同它們握著的來源圖。
    // 翻走的圖、undo 歷史裡的圖、舊的調色預覽、舊倍率的縮圖因此不會留在記憶體。
    void endFrame();
    size_t cachedBitmapCount() const { return bitmaps_.size(); }

    // 在目前的 transform 下畫底圖與所有標註（圖片座標）。
    void drawImage(DrawCtx& ctx);
    void drawAnnotations(DrawCtx& ctx, const std::vector<Annotation>& items, uint32_t skipId = 0);
    void drawAnnotation(DrawCtx& ctx, const Annotation& a);

    // 文字物件依目前寬度排版後的高度（圖片像素）。
    float textHeight(const Annotation& a);

    // 把底圖與標註壓成一張圖。
    Image flatten(const DocState& state);

    // UI 文字。
    void drawUiText(ID2D1RenderTarget* rt, const std::wstring& text, const D2D1_RECT_F& rect, float size,
                    D2D1_COLOR_F color, DWRITE_TEXT_ALIGNMENT align = DWRITE_TEXT_ALIGNMENT_LEADING,
                    bool bold = false, DWRITE_PARAGRAPH_ALIGNMENT para = DWRITE_PARAGRAPH_ALIGNMENT_CENTER,
                    bool clip = true, bool wrap = false);
    float measureUiText(const std::wstring& text, float size, bool bold = false, float maxWidth = 1e6f,
                        float* height = nullptr);
    IDWriteTextFormat* uiFormat(float size, bool bold);

    ID2D1StrokeStyle* dashStyle() const { return dash_.Get(); }
    ID2D1StrokeStyle* roundStyle() const { return round_.Get(); }

private:
    ComPtr<ID2D1Bitmap> makeBitmap(ID2D1RenderTarget* rt, const Image& img, float* scale);
    ComPtr<IDWriteTextLayout> textLayout(const Annotation& a, float width);
    void drawShapeGeometry(DrawCtx& ctx, ID2D1Geometry* geo, const StrokeStyle& s);
    void strokeGeometry(ID2D1RenderTarget* rt, ID2D1Geometry* geo, const StrokeStyle& s, float widthScale = 1.0f);

    ComPtr<ID2D1Factory> d2d_;
    ComPtr<IDWriteFactory> dw_;
    ComPtr<ID2D1RenderTarget> rt_; // 平常是視窗的 ID2D1HwndRenderTarget
    std::map<ID2D1RenderTarget*, ComPtr<ID2D1SolidColorBrush>> brushes_;
    ComPtr<ID2D1StrokeStyle> dash_, round_;
    struct BitmapEntry {
        ImagePtr src; // 釘住來源，指標才不會被重用
        int tw = 0, th = 0; // 0 = 原尺寸
        ImageBitmap bmp;
        bool used = false; // 這一幀畫過
    };
    // 用 list：同一幀裡先取第一頁再取第二頁，第一頁的指標不能因為新增而失效。
    std::list<BitmapEntry> bitmaps_;
    std::map<uint32_t, MosaicEntry> mosaic_;
    std::map<std::pair<int, bool>, ComPtr<IDWriteTextFormat>> uiFormats_;
};

} // namespace mp
