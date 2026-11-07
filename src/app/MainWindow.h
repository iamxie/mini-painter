#pragma once
#include <windows.h>
#include <commctrl.h>

#include <map>
#include <string>
#include <vector>

#include "app/Dialogs.h"
#include "app/InfoPanel.h"
#include "core/Annotation.h"
#include "core/Archive.h"
#include "core/ColorAdjust.h"
#include "core/Document.h"
#include "core/FolderNav.h"
#include "core/Lang.h"
#include "core/Settings.h"
#include "core/Spreads.h"
#include "core/TempDir.h"
#include "render/Renderer.h"

namespace mp {

// 工具列按鈕。
enum ButtonId {
    BTN_NONE = 0,
    BTN_ZOOMOUT, BTN_ZOOM100, BTN_ZOOMIN, BTN_ROTATE, BTN_MARKUP, BTN_INFO,
    BTN_FREEHAND, BTN_SHAPES, BTN_TEXT, BTN_ADJCOLOR, BTN_ADJSIZE, BTN_LINESTYLE, BTN_LINECOLOR, BTN_FILL, BTN_FORMAT,
};

struct Button {
    int id = BTN_NONE;
    D2D1_RECT_F rect{};
    bool enabled = true;
    bool pressed = false;  // 切換按鈕亮起
    bool dropdown = false; // 右下角畫小三角
};

class MainWindow : public ColorAdjustHost {
public:
    bool create(HINSTANCE inst, const std::wstring& initialPath, int nCmdShow);
    HWND hwnd() const { return hwnd_; }
    bool preTranslate(MSG& msg);

    // ColorAdjustHost
    const Image& baseImage() override;
    void previewChanged(const ColorAdjustParams& p) override;
    void commitAdjust(const ColorAdjustParams& p) override;
    void cancelAdjust() override;
    void beginPickWhite() override;

private:
    static LRESULT CALLBACK wndProc(HWND, UINT, WPARAM, LPARAM);
    static LRESULT CALLBACK editProc(HWND, UINT, WPARAM, LPARAM, UINT_PTR, DWORD_PTR);
    LRESULT handle(UINT msg, WPARAM wp, LPARAM lp);

    // 版面
    void layout();
    void buildButtons();
    float px(float v) const { return v * ui_; }

    // 檔案
    bool openPath(const std::wstring& path);
    bool confirmDiscard();
    bool save();
    bool saveAs();   // 換檔名存，之後編輯的就是新檔（只給 save() 遇到不能原地存的格式時用）
    bool exportAs(); // 輸出成另一個檔，目前編輯的還是原檔；壓縮檔內也能用
    bool writeFlattened(const std::wstring& target);           // 壓平後寫到 target，失敗跳訊息
    bool pickSavePath(std::wstring& out, bool exporting);     // 存檔對話框 + 跨格式 metadata 警告
    void openDialog();
    void goPrev();
    void goNext();
    void step(int dir);                 // 翻頁：dir +1 下一頁、-1 上一頁；到底了交給 bookEdge
    void bookEdge(int dir);             // 翻到底：提示；開了「接著看下一本」時第一次待命、3 秒內再按就跳本
    bool openBook(const std::wstring& book, int dir); // 開書架上的一本：dir > 0 從第一頁、< 0 從最後一頁
    std::wstring currentBook() const;   // 目前這本書：壓縮檔路徑，或圖片所在的資料夾
    void disarmJump();                  // 取消待命中的跳本（換方向、按別的鍵、逾時、開了別的檔）
    void showToast(const std::wstring& text, bool sticky); // 畫布中央的提示；sticky 的留到 hideToast，否則 2 秒淡掉
    void hideToast();
    void drawToast();
    void updateTitle();

    // 壓縮檔：整包解到暫存資料夾後當資料夾看，唯讀。
    // failMsg 為 nullptr：失敗時把錯誤顯示在畫布上（取代目前的圖）。給了 failMsg：失敗時目前的圖與狀態都不動，
    // 錯誤訊息寫進 *failMsg（使用者取消時是空字串），給跳本用。
    bool openArchive(const std::wstring& path, std::wstring* failMsg = nullptr);
    void closeArchive();                         // 刪這個壓縮檔的暫存、清狀態
    bool insideArchive(const std::wstring& path) const; // path 是不是目前壓縮檔解出來的檔
    bool readOnly() const { return !archivePath_.empty(); }
    bool editGuard();                            // 唯讀時提示並回傳 false
    std::wstring displayName(const std::wstring& path) const; // 檔名；壓縮檔內的檔顯示「壓縮檔名 › 內部路徑」
    std::wstring archiveNameOf(const std::wstring& path) const; // 壓縮檔內的名字（/ 分隔）；不是壓縮檔內的檔回空字串

    // 設定與 shell 登錄
    void loadSettingsAndRegister();
    void saveSettingsQuiet();
    std::wstring exeDir() const;        // exe 所在資料夾，結尾帶反斜線
    void applyPinnedNow();              // 置頂檔名改了：正在看的資料夾 / 壓縮檔立刻重排
    void showMainWindow(int cmd);       // 第一次顯示主視窗：上次關閉時是最大化就最大化
    void rememberPlacement();           // 關閉前把視窗位置存進 settings_
    void askDefaultProgram(bool fromSettings);
    void showSettings(int startPage = 0);                  // 設定面板；startPage 1 = 直接開「模式」tab
    void applySettingsFromDialog(const Settings& s, std::wstring& pendingLang); // 面板按套用 / 確定
    // 模式（設定集）：「顯示 › 模式」子選單、標題列、切換與儲存
    void rebuildProfileMenu();
    void switchProfile(const Profile& p);
    void saveCurrentToProfile();

    // 語言：讀內建與 lang/ 的語系，依設定或系統語系選一個啟用；選單切換後問是否重啟
    void initLanguage();
    void selectLanguage(const std::wstring& code);

    // 提示：物件教學提示（計次）與工具列按鈕名稱（每次 hover）共用一個 tooltip 視窗
    uint32_t hitItemBox(POINT pt) const;
    void showTipAt(const wchar_t* text, POINT screenPt);
    void showTip(POINT pt);
    void showButtonTip(int id);
    static const wchar_t* buttonName(int id);
    void hideTip();

    // 檢視模式（看漫畫）：設定裡的 viewMode 字串 <-> enum
    enum class ViewMode { FitWindow, FitWidth, FitHeight, Actual, TwoPage };
    static ViewMode viewModeFromString(const std::wstring& s);
    static const wchar_t* viewModeToString(ViewMode m);
    void setViewMode(ViewMode m);
    bool scrollBy(float dx, float dy); // 回傳有沒有真的動
    void scrollKey(int vk);            // 上下鍵：捲動，必要時翻頁

    // 兩頁並排：不能編輯，只看。
    bool twoPage() const { return viewMode_ == ViewMode::TwoPage && doc_.image() != nullptr; }
    void updateSpreads();          // 依資料夾清單與每頁寬高算配對（結果快取）
    int currentSpread() const;     // 目前這頁所在的屏，-1 表示不在清單
    void loadSecond();             // 載入同一屏的另一頁
    float viewW() const;           // 虛擬畫面（圖片座標）的寬高：單頁就是圖片，兩頁是並排後的
    float viewH() const;
    Rc pageRect(bool second) const; // 某頁在虛擬畫面裡的位置（底對齊；右到左時第一頁在右）

    // 檢視
    ImagePtr currentImage() const;
    void fitToView(bool resetOffset = true); // 依目前檢視模式算 zoom
    void setZoom(float z, POINT anchorClient);
    void zoomStep(int dir);
    void clampOffset();
    Pt toImage(POINT client) const;
    POINT toClient(Pt image) const;
    D2D1_MATRIX_3X2_F viewMatrix() const;
    POINT canvasCenter() const;
    float imageTol() const;

    // 繪製
    void paint();
    void drawChrome();
    void drawButton(const Button& b);
    void drawIcon(const Button& b);
    void drawCanvas();
    void drawSelection();
    void repaint() { InvalidateRect(hwnd_, nullptr, FALSE); }

    // 按鈕與選單
    int hitButton(POINT pt, const Button** out = nullptr) const;
    void onButton(int id);
    void onCommand(int id);
    void buildMenu();
    void updateMenu();
    RECT buttonScreenRect(int id) const;

    // 滑鼠
    void onLButtonDown(POINT pt);
    void onLButtonDblClk(POINT pt);
    void onMouseMove(POINT pt, WPARAM wp);
    void onLButtonUp(POINT pt);
    void onMouseWheel(POINT ptScreen, int delta, WPARAM keys);
    bool onSetCursor();
    bool onKeyDown(WPARAM vk);

    // 標註
    const std::vector<Annotation>& visibleItems() const;
    const Annotation* selectedItem() const;
    void commitSelectedChange(const Annotation& updated);
    void deleteSelected();
    void select(uint32_t id);
    void addShape(Kind kind);
    void addText();
    void applyStrokeStyle(const StrokeStyle& s);
    void applyTextStyle(const TextStyle& t);
    // 工具列顯示與選單初始值：有選取物件就是它的樣式，否則是新物件的預設樣式。
    StrokeStyle currentStroke() const;
    TextStyle currentText() const;
    void rotateImage();
    void adjustSize();
    void finishDrag();

    // 文字編輯
    void beginTextEdit(uint32_t id, bool selectAll);
    void endTextEdit(bool commit);
    void positionEdit();
    void updateEditFont();
    void onEditChanged();

    HINSTANCE inst_ = nullptr;
    HWND hwnd_ = nullptr;
    Settings settings_;
    std::wstring exePath_, settingsPath_;
    std::vector<LangTable> langs_; // 可選的語系，順序同語言選單
    bool settingsWritable_ = true;
    bool askPending_ = false;
    HWND tip_ = nullptr;
    TOOLINFOW tipInfo_{};
    uint32_t hoverItem_ = 0;
    uint32_t tipShownFor_ = 0;
    bool tipVisible_ = false;
    HMENU menu_ = nullptr;
    HMENU profileMenu_ = nullptr; // 顯示 › 模式（內容隨模式清單重建）
    float ui_ = 1.0f;
    Renderer renderer_;

    // 版面矩形（視窗座標）
    D2D1_RECT_F toolbarRc_{}, markupRc_{}, nameRc_{}, canvasRc_{}, infoRc_{};
    std::vector<Button> buttons_;
    int hoverBtn_ = BTN_NONE;
    int pressBtn_ = BTN_NONE;
    bool trackingMouse_ = false;
    int popupBtn_ = BTN_NONE; // 最近一次開彈出選單的按鈕

    bool showMarkup_ = false;
    bool showInfo_ = false;
    bool freehandMode_ = false;

    // 文件
    Document doc_;
    FolderNav nav_;
    std::wstring path_;
    std::wstring loadedFrom_; // 這張圖的 metadata 來源：原本開的檔；存檔成功後就是存的那個檔
    std::wstring loadError_;
    // 壓縮檔
    InstanceTempDir tempDir_;             // 本執行個體的暫存資料夾，第一次開壓縮檔時建立
    bool tempDirStale_ = false;           // 設定面板改了暫存位置：目前這本看完（closeArchive）就在新位置重建
    int archiveSeq_ = 0;                  // 每開一個壓縮檔用一個子資料夾
    std::wstring archivePath_;            // 目前開的壓縮檔；空 = 不是壓縮檔
    std::wstring archiveDir_;             // 它解出來的資料夾
    std::vector<std::wstring> archiveFiles_; // 解出來的檔（磁碟檔名是序號），閱讀順序
    std::vector<std::wstring> archiveNames_; // 與 archiveFiles_ 同序的壓縮檔內名字，顯示用
    InfoData infoData_;
    InfoPanelState infoState_;

    // 檢視
    float zoom_ = 1.0f;
    Pt offset_;        // 圖片原點在畫布裡的位置（畫布左上為 0）
    bool fit_ = true;  // 跟著檢視模式走；手動縮放後 false，翻頁或改模式後又回 true
    ViewMode viewMode_ = ViewMode::FitWindow;
    bool hqBusy_ = false;   // 連續縮放中：先畫 bilinear，停下來（timer）再換高品質
    bool scrollToEnd_ = false; // 用上鍵翻到上一張時，開在底部

    // 翻到底接著看下一本
    int armedDir_ = 0;             // 已經在底提示過一次、等再按一次的方向；0 = 沒有待命
    std::wstring armedTarget_;     // 待命要跳的那本
    std::wstring toast_;           // 畫布中央的提示文字；空 = 沒有
    bool toastSticky_ = false;

    // 兩頁並排
    ImagePtr second_;              // 同一屏的另一頁；沒有就是 nullptr
    std::wstring secondPath_;
    std::vector<Spread> spreads_;
    std::wstring spreadsKey_;      // spreads_ 對應的資料夾清單（第一個檔 + 數量 + 選項）
    std::map<std::wstring, bool> wideCache_; // 路徑 -> 寬大於高
    bool shiftOne_ = false;        // 「錯開一頁」，不存 ini

    // 樣式
    StrokeStyle stroke_;
    TextStyle text_;

    // 選取與拖曳
    uint32_t selected_ = 0;
    enum class Drag { None, Pan, Move, Handle, Draw };
    Drag drag_ = Drag::None;
    Pt dragStartImg_;
    POINT dragStartClient_{};
    Pt dragOffsetStart_;
    int dragHandle_ = -1;
    bool dragMoved_ = false;
    std::vector<Annotation> liveItems_; // 拖曳中的暫時狀態
    std::vector<Pt> drawPath_;

    // 文字編輯
    HWND edit_ = nullptr;
    uint32_t editingId_ = 0;
    HFONT editFont_ = nullptr;
    HBRUSH editBrush_ = nullptr;
    std::wstring editOriginal_;
    std::wstring editText_;

    // 調整顏色
    bool adjusting_ = false;
    bool pickingWhite_ = false;
    ColorAdjustParams adjustParams_;
    ImagePtr adjustBase_;
    ImagePtr adjustPreview_;
};

} // namespace mp
