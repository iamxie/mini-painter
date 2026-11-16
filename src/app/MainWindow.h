#pragma once
#include <windows.h>
#include <commctrl.h>

#include <map>
#include <memory>
#include <string>
#include <vector>

#include "app/ImageDialogs.h"
#include "app/InfoPanel.h"
#include "core/Annotation.h"
#include "core/Archive.h"
#include "core/ColorAdjust.h"
#include "core/Commands.h"
#include "core/Crop.h"
#include "core/Document.h"
#include "core/Editor.h"
#include "core/FolderNav.h"
#include "core/Lang.h"
#include "core/Paging.h"
#include "core/Prefetch.h"
#include "core/Settings.h"
#include "core/Spreads.h"
#include "core/TempDir.h"
#include "core/Update.h"
#include "core/ViewFit.h"
#include "core/Viewport.h"
#include "render/Renderer.h"

namespace mp {

// 工具列按鈕。
enum ButtonId {
    BTN_NONE = 0,
    BTN_ZOOMOUT, BTN_ZOOM100, BTN_ZOOMIN, BTN_ROTATE, BTN_MARKUP, BTN_INFO,
    BTN_FREEHAND, BTN_SHAPES, BTN_TEXT, BTN_ADJCOLOR, BTN_ADJSIZE, BTN_LINESTYLE, BTN_LINECOLOR, BTN_FILL, BTN_FORMAT,
    BTN_CROP,       // 標示工具列：進入 / 取消裁切模式
    BTN_CROP_APPLY, // 裁切模式中才有：標示工具列最右邊的「裁切」文字按鈕
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
    // 剛被自動更新重啟（main 收到 --after-update）：開好視窗後提示「已更新」。create 前呼叫。
    void setJustUpdated(bool v) { justUpdated_ = v; }
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
    // 開一個路徑（圖片、資料夾或壓縮檔），換掉目前的圖。呼叫前要先問過使用者（confirmLeaveImage）：
    // 這裡不問，因為有些呼叫端已經問過、或是剛啟動沒有東西可問。還沒問過的入口用 openConfirmed。
    // landAtBottom：開好後捲到底（用 ↑ 翻到上一張時）。
    bool openPath(const std::wstring& path, bool landAtBottom = false);
    bool openConfirmed(const std::wstring& path); // confirmLeaveImage 通過才 openPath
    // 開不了：把訊息畫在畫布上，目前的圖清掉。resetList：連翻頁清單與檔案資訊也清掉（資料夾沒圖、壓縮檔打不開）。
    void showLoadError(const std::wstring& path, const std::wstring& message, bool resetList);
    LoadResult loadPage(const std::wstring& path); // 解一頁：預先解好的直接拿，沒有就現在解（解好的交給 prefetch_ 留著）
    // 預先解碼：告訴背景目前這屏、翻頁方向的下一屏、上一屏，到一本的盡頭時加上換本預備。翻頁、改檢視設定、畫布大小變了時呼叫。
    void schedulePrefetch();
    std::wstring prefetchArchiveRoot(); // 換本預開壓縮檔要解到哪（必要時先載 7z.dll、建暫存資料夾）；不能預開回空字串
    bool ensureArchiveLibrary();      // 載入 exe 旁的 7z.dll（已載入就直接回 true）。背景預開壓縮檔之前也要先由這裡載好
    std::wstring archiveLibraryProblem() const; // ensureArchiveLibrary 回 false 時給使用者看的原因
    bool ensureTempDir();             // 建這個執行個體的解壓暫存資料夾（已建就直接回 true）
    bool confirmDiscard(bool leavingImage = false); // leavingImage：換圖時多說一句復原紀錄會清空
    // 換到別張圖之前（翻頁、跳本、拖檔、開啟、兩頁並排換到別頁）：沒存的修改問存檔，存過但還有復原紀錄就提醒會清空。
    bool confirmLeaveImage();
    bool save();
    bool saveAs();   // 換檔名存，之後編輯的就是新檔（只給 save() 遇到不能原地存的格式時用）
    bool exportAs(); // 輸出成另一個檔，目前編輯的還是原檔；壓縮檔內也能用
    bool writeFlattened(const std::wstring& target);           // 壓平後寫到 target，失敗跳訊息
    bool pickSavePath(std::wstring& out, bool exporting);     // 存檔對話框 + 跨格式 metadata 警告
    void openDialog();
    void goPrev();
    void goNext();
    void step(int dir, bool landAtBottom = false); // 翻頁：dir +1 下一頁、-1 上一頁；到底了交給 bookEdge
    void bookEdge(int dir);             // 翻到底：提示；開了「接著看下一本」時第一次待命、3 秒內再按就跳本
    bool openBook(const std::wstring& book, int dir); // 開書架上的一本：dir > 0 從第一頁、< 0 從最後一頁
    std::wstring currentBook() const;   // 目前這本書：壓縮檔路徑，或圖片所在的資料夾
    void disarmJump();                  // 取消待命中的跳本（換方向、按別的鍵、逾時、開了別的檔）
    // 畫布中央的提示；sticky 的留到 hideToast，否則 ms 毫秒後淡掉（0 = 預設 2 秒）
    void showToast(const std::wstring& text, bool sticky, UINT ms = 0);
    void hideToast();
    void drawToast();
    void updateTitle();                 // 每次編輯後都會呼叫，順便 checkUndoRoom
    void checkUndoRoom();               // 復原紀錄快滿（下一步可能丟最早的一步）時提示一次

    // 壓縮檔：整包解到暫存資料夾後當資料夾看，唯讀。
    // failMsg 為 nullptr：失敗時把錯誤顯示在畫布上（取代目前的圖）。給了 failMsg：失敗時目前的圖與狀態都不動，
    // 錯誤訊息寫進 *failMsg（使用者取消時是空字串），給跳本用。openLast：開最後一頁（往回跳本），否則開第一頁。
    bool openArchive(const std::wstring& path, std::wstring* failMsg = nullptr, bool openLast = false);
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

    // 新版本檢查與自動更新
    void startAutoUpdateCheck();                   // 啟動時：背景查，有新版就 post WM_MP_UPDATE_FOUND
    // 現在能不能打斷使用者（跳出別的視窗、把這個視窗關掉）：模態對話框或進度框開著（主視窗被停用）、選單開著、
    // 滑鼠拖到一半都不行。背景做完事要跳視窗、或別的行程要求關閉之前，都先問這個。
    bool canInterrupt() const;
    void offerPendingUpdate();                     // 有空檔（canInterrupt，而且沒有彈出選單、調色面板、文字編輯）才跳新版本對話框，否則稍後再試
    void checkForUpdatesNow();                     // 「關於」的檢查更新按鈕
    void offerUpdate(const ReleaseInfo& r);        // 新版本對話框 → 略過 / 稍後 / 安裝
    void installUpdate(const ReleaseInfo& r);      // 其他視窗會被關的提示 → 存檔確認 → 下載核對解開 → 關掉其他 mpainter → 換檔 → 關閉並重啟
    bool closeOtherInstances();                    // 請同一個 exe 的其他 mpainter 關閉並等它們結束；有一個不關回 false
    void manualDownloadFallback(const std::wstring& reason, const std::wstring& pageUrl); // 不能自動更新時打開下載頁

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

    // 檢視模式（看漫畫）
    using ViewMode = mp::ViewMode;
    bool setViewMode(ViewMode m); // 切到兩頁並排要換掉正在改的圖、使用者取消時回 false，什麼都不改
    bool scrollBy(float dx, float dy); // 回傳有沒有真的動
    void scrollKey(int vk);            // 上下鍵：捲動，必要時翻頁

    // 兩頁並排：不能編輯，只看。
    bool twoPage() const { return viewMode_ == ViewMode::TwoPage && doc_.image() != nullptr; }
    void updateSpreads();          // 依資料夾清單與每頁寬高算配對（結果在 spreadCache_）
    const std::vector<Spread>& spreads() const { return spreadCache_.spreads(); }
    int currentSpread() const;     // 目前這頁所在的屏，-1 表示不在清單
    bool confirmSpreadMove();      // spreads_ 算好後：目前這頁不是那屏的第一頁就要換圖，照 confirmLeaveImage 問
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
    CommandState commandState() const; // 指令可用條件要看的狀態（core/Commands.h）；選單、工具列、鍵盤共用
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

    // 標註。編輯的狀態與邏輯在 editor_（core/Editor.h），這裡是視窗這一側：先收掉文字框、做完更新標題與畫面。
    Pointer pointerAt(POINT client) const; // 滑鼠位置換成圖片座標，連同目前倍率下的各種容差
    float visibleShortSide() const;        // 看得到的範圍的短邊（圖片像素）
    void afterItemChange();                // 選取的物件改了之後：標題、文字框位置、重畫
    void deleteSelected();
    void select(uint32_t id);
    void addShape(Kind kind);
    void addText();
    void applyStrokeStyle(const StrokeStyle& s);
    void applyTextStyle(const TextStyle& t);
    void rotateImage();
    void adjustSize();
    // 結束拖曳並放掉滑鼠。apply：套用拖曳的結果（放開滑鼠）；false：丟掉（被別的動作打斷、滑鼠被搶走）。
    void endDrag(bool apply);
    // 收掉進行中的互動，回到閒置：拖到一半的丟掉、文字編輯結束（commitText：留下打的字）、裁切取消、
    // 調整顏色的面板關掉不套用。每個會換掉或改掉整張圖的入口都先呼叫這個，清理只寫在這一處。
    void endInteraction(bool commitText);
    Viewport viewport() const { return {zoom_, offset_}; }

    // 裁切模式：框預設整張圖，拖四角、四邊或框內調整，按「裁切」文字按鈕才真的裁。
    void beginCrop();
    void cancelCrop();  // 離開裁切模式、檢視回到進入前；不在裁切模式時什麼都不做
    void applyCrop();   // 框是整張圖時等於 cancelCrop
    void drawCrop();

    // 文字編輯
    void beginTextEdit(uint32_t id, bool selectAll);
    void endTextEdit(bool commit);
    void positionEdit();
    void updateEditFont();
    void onEditChanged();

    HINSTANCE inst_ = nullptr;
    HWND hwnd_ = nullptr;
    Settings settings_;
    Settings settingsBase_;           // 上次和設定檔同步時的 settings_（存檔時的三方合併用，見 mergeSettings）
    std::wstring exePath_, settingsPath_;
    std::vector<LangTable> langs_; // 可選的語系，順序同語言選單
    bool settingsWritable_ = true;
    bool settingsUnreadable_ = false; // 啟動時設定檔在但讀不到：這次執行用預設值，而且不存檔（存了會蓋掉原本的設定）
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

    // 文件與編輯狀態（選取、工具、拖曳、裁切框、調整顏色、文字編輯的內容）。
    Editor editor_;
    Document& doc_ = editor_.doc();
    bool undoWarned_ = false; // 這張圖已經提示過復原紀錄快滿；undo 到底（含換圖）或改設定後重來
    FolderNav nav_;
    std::wstring path_;
    std::wstring loadedFrom_; // 這張圖的 metadata 來源：原本開的檔；存檔成功後就是存的那個檔
    std::wstring loadError_;
    // 壓縮檔
    LibraryLoad archiveLoad_;             // 最近一次載入 7z.dll 的結果
    InstanceTempDir tempDir_;             // 本執行個體的暫存資料夾，第一次開壓縮檔（或預開下一本）時建立
    bool tempDirStale_ = false;           // 設定面板改了暫存位置：目前這本看完（closeArchive）就在新位置重建
    int archiveSeq_ = 0;                  // 每開一個壓縮檔用一個子資料夾
    std::wstring archivePath_;            // 目前開的壓縮檔；空 = 不是壓縮檔
    std::wstring archiveDir_;             // 它解出來的資料夾
    std::vector<std::wstring> archiveFiles_; // 解出來的檔（磁碟檔名是序號），閱讀順序
    std::vector<std::wstring> archiveNames_; // 與 archiveFiles_ 同序的壓縮檔內名字，顯示用
    InfoData infoData_;
    InfoPanelState infoState_;
    // 預先解碼（在 tempDir_ 之後宣告：先解構，背景停了才刪暫存資料夾）
    Prefetcher prefetch_;
    int lastDir_ = 1;                     // 最近一次翻頁的方向：預先解碼往這個方向準備
    float prefetchCanvasW_ = 0, prefetchCanvasH_ = 0; // 上次 schedulePrefetch 用的畫布大小；變了就再排一次

    // 檢視
    float zoom_ = 1.0f;
    Pt offset_;        // 圖片原點在畫布裡的位置（畫布左上為 0）
    bool fit_ = true;  // 跟著檢視模式走；手動縮放後 false，翻頁或改模式後又回 true
    ViewMode viewMode_ = ViewMode::FitWindow;
    bool hqBusy_ = false;   // 連續縮放中：先畫 bilinear，停下來（timer）再換高品質

    // 翻到底接著看下一本
    int armedDir_ = 0;             // 已經在底提示過一次、等再按一次的方向；0 = 沒有待命
    std::wstring armedTarget_;     // 待命要跳的那本
    std::wstring toast_;           // 畫布中央的提示文字；空 = 沒有
    bool toastSticky_ = false;

    // 兩頁並排
    ImagePtr second_;              // 同一屏的另一頁；沒有就是 nullptr
    std::wstring secondPath_;
    SpreadCache spreadCache_;      // 目前這本的配對與每頁是不是寬圖；清單或選項變了會自己重算
    bool shiftOne_ = false;        // 「錯開一頁」，不存 ini

    // 拖曳：按下時的滑鼠位置與畫面位移（平移用；其他種類的拖曳狀態在 editor_）
    POINT dragStartClient_{};
    Pt dragOffsetStart_;

    // 文字編輯的輸入框（內容與編輯中的物件在 editor_）
    HWND edit_ = nullptr;
    HFONT editFont_ = nullptr;
    HBRUSH editBrush_ = nullptr;

    // 自動更新
    std::unique_ptr<ReleaseInfo> pendingUpdate_; // 背景查到、還沒問使用者的新版
    bool menuLoop_ = false;     // 選單開著（不在這時跳新版本對話框）
    bool updating_ = false;     // 換檔完成、正在關閉：WM_CLOSE 不再問存檔（已經問過）
    bool justUpdated_ = false;
    std::wstring relaunchArgs_; // 非空：WM_DESTROY 時用這些參數重啟 exe

    // 裁切模式的檢視（框本身在 editor_）
    bool cropFit_ = false;     // 裁切模式的「整張圖放進畫布」還有效；手動縮放或改檢視模式後 false
    float cropSavedZoom_ = 1.0f; // 進入裁切模式前的檢視，取消時還原
    Pt cropSavedOffset_;
    bool cropSavedFit_ = true;
};

} // namespace mp
