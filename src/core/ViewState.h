#pragma once
#include "core/Geometry.h"
#include "core/ViewFit.h"
#include "core/Viewport.h"

namespace mp {

// 算檢視時要看的外在條件。ViewState 自己不記這些：畫布大小、圖的大小、檢視模式隨時會變，
// 每次由呼叫端（MainWindow::viewFrame）照當下的狀態給，就不會有「忘了通知」的舊值。
struct ViewFrame {
    float canvasW = 0, canvasH = 0;   // 畫布大小（像素）
    float contentW = 0, contentH = 0; // 虛擬畫面大小（圖片座標；單頁是那張圖，兩頁是並排後的）。沒有圖時是 0
    ViewMode mode = ViewMode::FitWindow;
    bool upscaleSmall = false; // 「小圖放大到符合」
    bool cropping = false;     // 正在裁切模式

    bool hasContent() const { return contentW > 0 && contentH > 0; }
};

// 主視窗的檢視狀態：倍率、位移、是不是跟著檢視模式走、裁切模式存的檢視。不碰視窗。
// 沒有圖的時候（frame.hasContent() 是 false）每個會改狀態的函式都不做事。
class ViewState {
public:
    float zoom() const { return view_.zoom; }
    Pt offset() const { return view_.offset; } // 圖片原點在畫布裡的位置（畫布左上為 0）
    Viewport viewport() const { return view_; }
    // 倍率跟著檢視模式走。手動縮放後是 false；翻頁、改模式、旋轉等重新 fit 之後又回 true。
    bool fitting() const { return fit_; }
    // 倍率是 1（工具列「實際大小」按鈕亮不亮）。
    bool atActualSize() const;

    // 照檢視模式重新算倍率，之後跟著模式走。resetOffset：位移歸零（從左上開始，放得下的方向置中）；
    // false 時留著原本的位移、只夾回範圍（視窗改大小）。
    void fit(const ViewFrame& f, bool resetOffset = true);
    // 畫布或版面變了：跟著模式走的重新 fit，手動縮放過的只把位移夾回範圍。
    void reflow(const ViewFrame& f, bool resetOffset);
    // 手動縮放到 zoom（會夾進 kViewMinZoom..kViewMaxZoom），畫布上 anchor 底下的那個圖片點留在原地。之後不跟著模式走。
    void zoomTo(const ViewFrame& f, float zoom, Pt anchorCanvas);
    // 捲動（畫布像素）。回傳畫面有沒有真的動（已經到邊就不動）。
    bool scrollBy(const ViewFrame& f, float dx, float dy);
    // 位移直接設成 offset（拖曳平移），夾回範圍。
    void panTo(const ViewFrame& f, Pt offset);
    // 捲到最底（用 ↑ 翻到上一張時從底部開始看）。
    void scrollToBottom(const ViewFrame& f);
    // 只把位移夾回範圍。
    void clamp(const ViewFrame& f);

    // 裁切模式：進入時記下目前的檢視，整張圖放進畫布（不管檢視模式）；手動縮放或改檢視模式後這個「整張放進畫布」就失效。
    // enterCrop 要在進了裁切模式之後呼叫（f.cropping 是 true）。
    void enterCrop(const ViewFrame& f);
    // 取消裁切：回到進入前的檢視。要在離開裁切模式之後呼叫。
    void leaveCrop(const ViewFrame& f);
    // 「整張圖放進畫布」不再有效（改了檢視模式、裁切套用了）。
    void endCropFit() { cropFit_ = false; }
    bool cropFit() const { return cropFit_; }

private:
    Viewport view_;
    bool fit_ = true;
    bool cropFit_ = false; // 裁切模式的「整張圖放進畫布」還有效
    Viewport savedView_;   // 進入裁切模式前的檢視
    bool savedFit_ = true;
};

} // namespace mp
