#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "core/Annotation.h"
#include "core/ColorAdjust.h"
#include "core/Crop.h"
#include "core/Document.h"
#include "core/Image.h"

namespace mp {

// 畫布的工具。三個互斥：選了一個，另外兩個就不是。
//   Select   點選、搬移、拖控制點；沒點到東西就平移畫面
//   Freehand 拖曳畫塗鴉
//   Crop     拖裁切框
// 另外三個狀態可以疊在工具上面，各自有進有出：文字編輯中、調整顏色的面板開著、取白點。
enum class Tool { Select, Freehand, Crop };

// 滑鼠按著拖的是什麼。
enum class DragKind { None, Pan, Move, Handle, Draw, Crop };

// 滑鼠事件帶的東西：位置（圖片座標），和幾個隨縮放倍率變的距離（都換成圖片像素）。
struct Pointer {
    Pt pos;
    float tol = 0;        // 點物件的容差
    float handleTol = 0;  // 點控制點的半徑
    float cropTol = 0;    // 裁切框邊的容差
    float cropCorner = 0; // 裁切框的角沿邊多長
    float drawStep = 0;   // 塗鴉：離上一點至少這麼遠才記下一點
};

enum class CursorKind { Default, Cross, Move, SizeNWSE, SizeNESW, SizeWE, SizeNS, Hand };

// 編輯一張圖的狀態與互動邏輯：文件（底圖、標註、undo）、選取、工具、拖曳、裁切框、調整顏色、文字編輯的內容。
// 不碰視窗：滑鼠位置由呼叫端換成圖片座標再給；畫面平移、抓滑鼠、EDIT 控制項、對話框都是呼叫端（MainWindow）的事。
// 「現在按下去會發生什麼」只在這裡判斷（press、cursorAt、tipItemAt、escape），所以加一種工具只改這個類別。
class Editor {
public:
    // 文字方塊依目前寬度排版後的高度（要 DirectWrite，由 app 給）。沒給時文字方塊的高度不會跟著內容變。
    void setTextMeasurer(std::function<float(const Annotation&)> measure) { measure_ = std::move(measure); }

    Document& doc() { return doc_; }
    const Document& doc() const { return doc_; }

    // 換一張圖（nullptr = 沒有圖）。選取清掉；工具不變（塗鴉工具跨圖留著）。
    // 呼叫前要先收掉進行中的互動（拖曳、裁切、調色、文字編輯）。
    void reset(ImagePtr image, Dpi dpi = {});

    // ---- 狀態 ----
    Tool tool() const { return tool_; }
    bool freehand() const { return tool_ == Tool::Freehand; }
    bool cropping() const { return tool_ == Tool::Crop; }
    DragKind drag() const { return drag_; }
    bool dragging() const { return drag_ != DragKind::None; }
    bool adjusting() const { return adjusting_; }
    bool pickingWhite() const { return pickingWhite_; }
    bool textEditing() const { return editingId_ != 0; }
    uint32_t editingId() const { return editingId_; }

    uint32_t selected() const { return selected_; }
    void select(uint32_t id) { selected_ = id; }
    // 選取的物件（拖曳中是拖到一半的樣子）；沒有選取、或那個物件已經不在，回 nullptr。
    const Annotation* selectedItem() const;
    // 畫面上要畫的物件：拖曳搬移或拖控制點時是拖到一半的那份，否則是文件裡的。
    const std::vector<Annotation>& visibleItems() const;
    // 畫面上要畫的底圖：調整顏色時是預覽，否則是文件的底圖。
    ImagePtr displayImage() const;
    // 塗鴉畫到一半的筆跡（圖片座標）。
    const std::vector<Pt>& drawPath() const { return drawPath_; }
    const CropBox& cropBox() const { return cropBox_; }
    // 正在拖的控制點（drag() 是 Handle 時有意義）。
    int dragHandleId() const { return dragHandle_; }

    // 新物件的預設樣式。
    const StrokeStyle& stroke() const { return stroke_; }
    const TextStyle& text() const { return text_; }
    // 工具列顯示與選單初始值：有選取物件就是它的樣式，否則是新物件的預設樣式。
    StrokeStyle currentStroke() const;
    TextStyle currentText() const;

    // ---- 工具 ----
    // 塗鴉工具開 / 關。打開時取消選取。回傳現在是不是開著。
    bool toggleFreehand();
    void leaveFreehand();
    // 進入裁切：框是整張圖，選取清掉，其他工具關掉。沒有圖回 false。
    bool beginCrop();
    // 離開裁切、不裁。拖裁切框拖到一半的也放掉。不在裁切模式回 false。
    bool cancelCrop();
    enum class CropResult { NotCropping, Unchanged, Applied };
    // 照裁切框裁。框就是整張圖時等於取消（Unchanged）。Applied：底圖與標註都換了，進 undo。
    CropResult applyCrop();

    // ---- 滑鼠（在畫布上、有圖的時候）----
    enum class Press { PickedWhite, Drag };
    // 按下左鍵。由上到下第一個成立的決定這一下做什麼：
    //   兩頁並排 → 平移；取白點中 → 取那一點的顏色；裁切 → 拖框（框外平移）；塗鴉 → 開始畫；
    //   其餘 → 選取物件的控制點、物件（上層優先）、都沒有就取消選取並平移。
    // 回傳 Drag 時 drag() 是拖的種類（呼叫端要抓住滑鼠；Pan 的位移由呼叫端自己算）。
    // 文字編輯要由呼叫端先結束。
    Press press(const Pointer& p, bool twoPage);
    // 拖曳中移動。pastClickSlop：滑鼠離按下的位置夠遠了（不是手抖），搬移與拖控制點才算數。
    void move(const Pointer& p, bool pastClickSlop);
    // 放開：拖曳的結果寫進文件。回傳文件有沒有變（新的 undo 步驟）。
    bool release();
    // 丟掉拖到一半的東西：物件回到拖之前的樣子，裁切框回到拖之前，畫到一半的筆跡不留。
    void cancelDrag();

    // 游標在 p 時該用什麼形狀（和 press 同一套判斷）。
    CursorKind cursorAt(const Pointer& p, bool twoPage) const;
    // 外框包住 p 的最上層物件；沒有回 0。
    uint32_t itemBoxAt(Pt p) const;
    // 可以顯示教學提示的物件（文字編輯、塗鴉、取白點、裁切、兩頁並排時不顯示）。
    uint32_t tipItemAt(Pt p, bool twoPage) const;
    // 命中 p 的最上層文字方塊（雙擊編輯用）；沒有回 0。
    uint32_t textItemAt(const Pointer& p) const;

    enum class Escape { Nothing, CancelledCrop, StoppedPickWhite, Deselected, LeftFreehand };
    // 按 Esc：裁切 → 取消裁切；取白點 → 不取了；有選取 → 取消選取；塗鴉 → 關掉。一次只做第一個成立的。
    Escape escape();

    // ---- 標註 ----
    // 在 center 加一個預設形狀並選取它（塗鴉工具關掉）。viewSize 是看得到的範圍的短邊（圖片像素），決定預設大小。
    uint32_t addShape(Kind kind, Pt center, float viewSize);
    // 加一個文字方塊並選取。label 是方塊裡預設的字。
    uint32_t addText(Pt center, float viewSize, const std::wstring& label);
    // 把同一個 id 的物件換成 updated（文字方塊重算高度），進 undo。不在文件裡回 false。
    bool commitItem(const Annotation& updated);
    // 選取的物件搬 (dx, dy)。沒有選取回 false。
    bool nudgeSelected(float dx, float dy);
    // 刪掉選取的物件。文字編輯中不刪。
    bool deleteSelected();
    // 改線條樣式：套到選取的物件；選取的不是文字方塊時也變成新物件的預設（文字方塊的外框預設無色，不要帶到之後的形狀）。
    // 回傳選取的物件有沒有被改。
    bool applyStrokeStyle(const StrokeStyle& s);
    // 改文字樣式：變成預設，選取的是文字方塊就套上去。回傳有沒有物件被改。
    bool applyTextStyle(const TextStyle& t);

    // ---- 整張圖 ----
    // 逆時針轉 90 度，標註跟著轉。沒有圖回 false。
    bool rotateCCW();
    // 調整尺寸。resample 為 false 或尺寸沒變：只改解析度，底圖共用。否則重新取樣，標註跟著縮放。
    bool resize(int width, int height, double dpi, bool resample);
    void undo();
    void redo();

    // ---- 調整顏色 ----
    // 開始（面板打開）。已經在調、或沒有圖回 false。
    bool beginAdjust();
    // 調色前的底圖（面板算直方圖用）。沒在調時是 nullptr。
    ImagePtr adjustBase() const { return adjustBase_; }
    const ColorAdjustParams& adjustParams() const { return adjustParams_; }
    void previewAdjust(const ColorAdjustParams& p);
    // 套用（面板按好）。有改到圖回 true（進 undo）。之後不在調色狀態。
    bool commitAdjust(const ColorAdjustParams& p);
    void cancelAdjust();
    void beginPickWhite() { pickingWhite_ = true; }

    // ---- 文字編輯（輸入框本身是呼叫端的 EDIT 控制項）----
    // 開始編輯 id 這個文字方塊。不是文字方塊回 false。
    bool beginTextEdit(uint32_t id);
    const std::wstring& editText() const { return editText_; }
    // 輸入框的內容變了。方塊的高度跟著內容長（直接改文件裡的物件，不進 undo）。回傳高度有沒有變。
    bool setEditText(const std::wstring& text);
    // 結束編輯。commit：打成空的就刪掉方塊，有改就進 undo；否則丟掉打的字。回傳文件有沒有新的 undo 步驟。
    bool endTextEdit(bool commit);

private:
    void fitText(Annotation& a) const;
    void commitItems(std::vector<Annotation> items) { doc_.commitItems(std::move(items)); }

    std::function<float(const Annotation&)> measure_;
    Document doc_;
    Tool tool_ = Tool::Select;
    uint32_t selected_ = 0;
    StrokeStyle stroke_;
    TextStyle text_;

    // 拖曳
    DragKind drag_ = DragKind::None;
    Pt dragStart_;
    int dragHandle_ = -1;
    bool dragMoved_ = false;
    std::vector<Annotation> liveItems_; // 搬移、拖控制點時的暫時狀態
    std::vector<Pt> drawPath_;

    // 裁切
    CropBox cropBox_;
    CropBox cropStart_; // 拖曳開始時的框
    CropPart cropPart_ = CropPart::None;

    // 調整顏色
    bool adjusting_ = false;
    bool pickingWhite_ = false;
    ColorAdjustParams adjustParams_;
    ImagePtr adjustBase_;
    ImagePtr adjustPreview_;

    // 文字編輯
    uint32_t editingId_ = 0;
    std::wstring editOriginal_;
    std::wstring editText_;
};

} // namespace mp
