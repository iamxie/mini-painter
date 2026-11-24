#pragma once
#include <windows.h>
#include <commctrl.h>

#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "app/BrowseView.h"
#include "app/ImageDialogs.h"
#include "app/AppSettings.h"
#include "app/InfoPanel.h"
#include "app/Popups.h"
#include "app/Theme.h"
#include "app/Toolbar.h"
#include "app/UpdateFlow.h"
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
#include "core/Plugins.h"
#include "core/Prefetch.h"
#include "core/Settings.h"
#include "core/Spreads.h"
#include "core/TempDir.h"
#include "core/ThumbCache.h"
#include "core/Thumbnails.h"
#include "core/Update.h"
#include "core/ViewFit.h"
#include "core/ViewState.h"
#include "core/Viewport.h"
#include "render/Renderer.h"

namespace mp {

class MainWindow : public ColorAdjustHost, private UpdateHost, private BrowseHost {
public:
    // 剛被自動更新重啟（main 收到 --after-update）：開好視窗後提示「已更新」。create 前呼叫。
    void setJustUpdated(bool v) { justUpdated_ = v; }
    bool create(HINSTANCE inst, const std::wstring& initialPath, int nCmdShow);
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
    // handle 裡比較長的幾個訊息
    LRESULT onCloseForUpdate(DWORD pid, HWND progress);
    void onSize(); // 照目前的工作區大小調 render target、重排版面與檢視（WM_SIZE，或重畫時發現版面還是舊的）
    void onDpiChanged(unsigned dpi, const RECT& suggested);
    bool onTimer(UINT_PTR id);
    HBRUSH editColors(HDC dc);
    void onClose();
    void onDestroy();

    // 版面
    void layout();
    void buildButtons(); // 工具列重排：按鈕的可用與亮起跟著目前的狀態（狀態變了就要呼叫）

    // 檔案
    // 開一個路徑（圖片、資料夾或壓縮檔），換掉目前的圖。呼叫前要先問過使用者（confirmLeaveImage）：
    // 這裡不問，因為有些呼叫端已經問過、或是剛啟動沒有東西可問。還沒問過的入口用 openConfirmed。
    // landAtBottom：開好後捲到底（用 ↑ 翻到上一張時）。
    bool openPath(const std::wstring& path, bool landAtBottom = false);
    bool openConfirmed(const std::wstring& path); // confirmLeaveImage 通過才 openPath
    bool showPage(const std::wstring& path, bool inArchive, bool landAtBottom); // openPath 的後半：解碼並換上這一張圖
    int spreadStartIfSecond() const; // 兩頁並排：目前這頁是某屏的第二頁時，那屏第一頁的位置；否則 -1
    void undoRedo(bool undo);
    void togglePairing(bool& flag, bool saved); // 兩頁並排的配對選項開 / 關
    // 開不了：把訊息畫在畫布上，目前的圖清掉。resetList：連翻頁清單與檔案資訊也清掉（資料夾沒圖、壓縮檔打不開）。
    // link：缺 Microsoft Store 擴充時那個擴充的網址，訊息下面畫成可以點的連結；空的就不畫。
    void showLoadError(const std::wstring& path, const std::wstring& message, bool resetList, const std::wstring& link = {});
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
    // 唯讀：壓縮檔裡的圖（還能旋轉）、plugin 讀進來的圖與寫不出來的格式（只能輸出成）。規則在 core/Commands 的 EditLock。
    EditLock editLock() const { return editLockFor(!archivePath_.empty(), pluginImage_, viewOnlyFormat_); }
    bool readOnly() const { return editLock() != EditLock::None; }
    bool editGuard();                            // 唯讀時提示並回傳 false
    // 整張圖的動作（裁切、旋轉、調整尺寸、調色）。記憶體不夠時 Editor 丟 std::bad_alloc、文件不動：說一聲，回 false。
    bool runImageOp(const std::function<void()>& op);
    std::wstring displayName(const std::wstring& path) const; // 檔名；壓縮檔內的檔顯示「壓縮檔名 › 內部路徑」
    std::wstring archiveNameOf(const std::wstring& path) const; // 壓縮檔內的名字（/ 分隔）；不是壓縮檔內的檔回空字串

    // 設定與 shell 登錄
    void loadSettingsAndRegister();
    // 存設定（和別的視窗寫的合併）。不跳對話框；寫不進去時在畫布中央提示，一次執行只提示第一次。
    void saveSettingsQuiet();
    std::wstring exeDir() const { return store_.exeDir(); } // exe 所在資料夾，結尾帶反斜線
    void applyPinnedNow();              // 置頂檔名改了：正在看的資料夾 / 壓縮檔立刻重排
    void showMainWindow(int cmd);       // 第一次顯示主視窗：上次關閉時是最大化就最大化
    void rememberPlacement();           // 關閉前把視窗位置存進 settings_
    void askDefaultProgram(bool fromSettings);
    void showSettings(int startPage = 0);                  // 設定面板；startPage 1 = 直接開「模式」tab
    // 面板按套用 / 確定。語言最後才處理，放進 pendingLang；plugin 的狀態有變就把 pluginsTouched 設成 true。
    void applySettingsFromDialog(const Settings& s, std::wstring& pendingLang, bool& pluginsTouched);
    // 讀圖 plugin：啟動時讀當掉紀錄、停用害程式當掉的與互相衝突的、載入其他的（loadSettingsAndRegister 裡）。
    // 停用了哪些、為什麼，等主視窗顯示之後再用對話框說（showPluginNotices）。
    void startPluginsAtLaunch();
    void showPluginNotices();
    // 照設定，entries 裡要啟用的 plugin 和這次啟動時試著載入的不一樣（要重新啟動才會生效）。
    bool pluginsNeedRestart(const std::vector<PluginEntry>& entries) const;
    // 模式（設定集）：「顯示 › 模式」子選單、標題列、切換與儲存
    void rebuildProfileMenu();
    void switchProfile(const Profile& p);
    void saveCurrentToProfile();

    // 新版本檢查與自動更新：流程在 updates_（app/UpdateFlow），這裡是它要主視窗幫忙的事（UpdateHost）
    HWND updateOwnerWindow() const override { return hwnd_; }
    std::wstring updateExePath() const override { return store_.exePath(); }
    std::wstring updateCheckUrl() const override { return settings_.updateUrl; }
    bool updateVersionSkipped(const std::wstring& version) const override;
    void skipUpdateVersion(const std::wstring& version) override;
    bool confirmDiscardBeforeUpdate() override { return confirmDiscard(); }
    bool loadArchiveLibraryForUpdate() override { return ensureArchiveLibrary(); }
    std::wstring reopenAfterUpdate() const override;
    void closeForRelaunch(const std::wstring& relaunchArgs) override;
    // 現在能不能打斷使用者（跳出別的視窗、把這個視窗關掉）：模態對話框或進度框開著（主視窗被停用）、選單開著、
    // 滑鼠拖到一半都不行。背景做完事要跳視窗、或別的行程要求關閉之前，都先問這個。
    bool canInterrupt() const;
    void offerPendingUpdate();                     // 有空檔（canInterrupt，而且沒有彈出選單、調色面板、文字編輯）才跳新版本對話框，否則稍後再試

    // 語言：選單切換後問是否重啟（可選的語系與啟用哪一個在 store_，啟動時決定）
    void selectLanguage(const std::wstring& code);
    // 問要不要現在重新啟動（question 是翻譯好的整句）；要的話確認存檔、重開自己並開回目前的檔案、關掉這個視窗。
    void askRestart(const std::wstring& question);

    // 提示：物件教學提示（計次）與工具列按鈕名稱（每次 hover）共用一個 tooltip 視窗
    uint32_t hitItemBox(POINT pt) const;
    void showTipAt(const wchar_t* text, POINT screenPt);
    void showTip(POINT pt);
    void showButtonTip(int id);
    void hideTip();

    // 檢視模式（看漫畫）
    using ViewMode = mp::ViewMode;
    bool setViewMode(ViewMode m); // 切到兩頁並排要換掉正在改的圖、使用者取消時回 false，什麼都不改
    bool scrollBy(float dx, float dy); // 回傳有沒有真的動
    void scrollKey(int vk);            // 上下鍵：捲動，必要時翻頁

    // 兩頁並排：不能編輯，只看。
    bool twoPage() const { return viewMode_ == ViewMode::TwoPage && doc_.image() != nullptr; }
    // 兩頁並排模式、有開著的檔（第一頁讀不到也算）：翻頁、預先解碼、重新配對都照「屏」算。
    // 和 twoPage() 分開：第一頁讀不到時 twoPage() 是 false，照單頁翻會在同一屏來回（開第二頁又被導回第一頁）。
    bool pagingBySpread() const { return viewMode_ == ViewMode::TwoPage && !path_.empty(); }
    void updateSpreads();          // 依資料夾清單與每頁寬高算配對（結果在 spreadCache_）
    const std::vector<Spread>& spreads() const { return spreadCache_.spreads(); }
    int currentSpread() const;     // 目前這頁所在的屏，-1 表示不在清單
    bool confirmSpreadMove();      // spreads_ 算好後：目前這頁不是那屏的第一頁就要換圖，照 confirmLeaveImage 問
    void loadSecond();             // 載入同一屏的另一頁（讀不到時記下原因，畫佔位框）
    // 兩頁並排、這一屏有第二頁的位置：讀到了，或讀不到要畫佔位框。第一頁讀不到時也算（那時 twoPage() 是 false）。
    bool spreadHasSecond() const;
    // 這一屏的版面：兩頁並排時讀不到的那一頁用另一頁的大小佔位。沒有可排的（沒有圖、兩頁都讀不到）回 false。
    bool spreadLayout(PageSize& first, PageSize& second, bool& two) const;
    bool hasView() const;          // 畫布上有東西可排（spreadLayout 成功）：縮放、捲動、平移要有這個
    float viewW() const;           // 虛擬畫面（圖片座標）的寬高：單頁就是圖片，兩頁是並排後的
    float viewH() const;
    Rc pageRect(bool second) const; // 某頁在虛擬畫面裡的位置（底對齊；右到左時第一頁在右）

    // 檢視。倍率、位移、跟不跟著檢視模式走在 view_（core/ViewState.h），這裡是視窗這一側：
    // 給它當下的畫布與圖的大小（viewFrame），改完更新按鈕、文字框、畫面。
    ImagePtr currentImage() const;
    ViewFrame viewFrame() const;             // view_ 的每個函式都要的外在條件：畫布大小、虛擬畫面大小、檢視模式
    void fitToView(bool resetOffset = true); // 依目前檢視模式算 zoom，之後跟著模式走
    void reflowView(bool resetOffset);       // 畫布或版面變了：跟著模式走的重新 fit，手動縮放過的只夾位移
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
    void drawCanvas();
    // 圖開不了的訊息畫在 box 裡（置中）。url 不是空的時，訊息下面加一行可以點的連結，位置記進 canvasLinks_。
    void drawLoadProblem(const D2D1_RECT_F& box, const std::wstring& text, const std::wstring& url, bool wrap);
    const std::wstring* linkAt(POINT pt) const; // 滑鼠下的連結的網址；沒有回 nullptr
    void drawSelection();
    void repaint() { InvalidateRect(hwnd_, nullptr, FALSE); }

    // 按鈕與選單
    CommandState commandState() const; // 指令可用條件要看的狀態（core/Commands.h）；選單、工具列、鍵盤共用
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
    Viewport viewport() const { return view_.viewport(); }

    // 裁切模式：框預設整張圖，拖四角、四邊或框內調整，按「裁切」文字按鈕才真的裁。
    void beginCrop();
    void cancelCrop();  // 離開裁切模式、檢視回到進入前；不在裁切模式時什麼都不做
    void applyCrop();   // 框是整張圖時等於 cancelCrop
    void drawCrop();

    // 瀏覽資料夾（目錄瀏覽模式）：左上資料夾樹、左下預覽、右邊縮圖（子視窗在 browse_，app/BrowseView）。
    // 進入時這張圖的編輯紀錄清掉、圖不留在記憶體；壓縮檔的暫存留著，離開時回到原本那一頁。規則見 history.md 2026-11-24。
    void toggleBrowse();       // 按鈕、選單、Esc
    bool confirmEnterBrowse(); // 有沒存的修改：存檔後進入 / 放棄後進入 / 不進入；存過但還有復原紀錄：提醒會清空
    // 不問，直接進入（或換資料夾）：資料夾 folder，選中 select（空的就不選）。
    void enterBrowse(const std::wstring& folder, const std::wstring& select);
    // 離開瀏覽。reopen：回到進入前那張圖（從磁碟重開）、本來就沒開圖就回空白；false：呼叫端接著開別的檔。
    void leaveBrowse(bool reopen);
    void ensureThumbCache();         // 第一次要縮圖時開縮圖快取（暫存位置底下的 thumbs.mpainter.db）
    void reopenThumbCache();         // 暫存位置或上限改了
    void showThumbSizeMenu();        // 按鈕右邊的三角形：小 / 中 / 大
    void setThumbSize(int index);
    void layoutBrowse();             // layout 的瀏覽模式部分：三塊與兩條分隔線
    void drawBrowse();               // 預覽（畫布的位置）
    D2D1_RECT_F previewImageBox() const; // 預覽框裡放圖的範圍（扣掉留白與底下的資訊列）
    void requestPreview();           // 照目前的選取與預覽框大小解預覽圖
    int splitterAt(POINT pt) const;  // 1 = 左右之間的分隔線，2 = 樹與預覽之間的，0 = 都不是
    bool browseKey(const MSG& msg);  // 瀏覽模式時子視窗收到的按鍵（Esc、Tab、Backspace、Ctrl+快捷鍵）
    // BrowseHost
    void browseSelectionChanged() override;
    void browseActivated(int index) override;
    void browseFolderChanged() override;

    // 文字編輯
    void beginTextEdit(uint32_t id, bool selectAll);
    void endTextEdit(bool commit);
    void positionEdit();
    void updateEditFont();
    void onEditChanged();

    HINSTANCE inst_ = nullptr;
    HWND hwnd_ = nullptr;
    AppSettings store_;                  // 設定檔的讀寫、目前的值、可選的語系
    Settings& settings_ = store_.values; // 目前的設定值
    PluginStartup plugins_;              // 啟動時掃到的 plugin、載入結果、這次停用的（對話框說完就清掉 crashes / conflicts）
    bool askPending_ = false;
    bool saveFailWarned_ = false; // 這次執行已經提示過「設定存不進去」
    HWND tip_ = nullptr;
    TOOLINFOW tipInfo_{};
    uint32_t hoverItem_ = 0;
    uint32_t tipShownFor_ = 0;
    bool tipVisible_ = false;
    HMENU menu_ = nullptr;
    HMENU profileMenu_ = nullptr; // 顯示 › 模式（內容隨模式清單重建）
    UiScale px; // 邏輯像素 → 這個視窗所在螢幕的實際像素：px(10)
    Renderer renderer_;

    // 版面矩形（視窗座標）
    D2D1_RECT_F toolbarRc_{}, markupRc_{}, nameRc_{}, canvasRc_{}, infoRc_{};
    Toolbar toolbar_; // 兩列工具列的按鈕（排版、繪製、命中）
    bool trackingMouse_ = false;
    int popupBtn_ = BTN_NONE; // 最近一次開彈出選單的按鈕
    PopupMenus popups_;       // 工具列的彈出選單（同時最多一個）
    ColorAdjustPanel adjustPanel_; // 「調整顏色」面板

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
    std::wstring loadErrorLink_; // 開不了的原因是缺 Store 擴充時，那個擴充的網址；其他是空的
    bool viewOnlyFormat_ = false; // 目前的圖是寫不出來的格式（HEIF；唯讀，只能輸出成）
    // 畫布上可以點的連結（最近一次畫的；位置是視窗座標）。每次畫畫布時重填。
    struct CanvasLink {
        D2D1_RECT_F rc;
        std::wstring url;
    };
    std::vector<CanvasLink> canvasLinks_;
    // 壓縮檔
    LibraryLoad archiveLoad_;             // 最近一次載入 7z.dll 的結果
    InstanceTempDir tempDir_;             // 本執行個體的暫存資料夾，第一次開壓縮檔（或預開下一本）時建立
    bool tempDirStale_ = false;           // 設定面板改了暫存位置：目前這本看完（closeArchive）就在新位置重建
    int archiveSeq_ = 0;                  // 每開一個壓縮檔用一個子資料夾
    std::wstring archivePath_;            // 目前開的壓縮檔；空 = 不是壓縮檔
    bool pluginImage_ = false;            // 目前的圖是 plugin 解的（唯讀，只能輸出成）
    std::wstring archiveDir_;             // 它解出來的資料夾
    std::vector<std::wstring> archiveFiles_; // 解出來的檔（磁碟檔名是序號），閱讀順序
    std::vector<std::wstring> archiveNames_; // 與 archiveFiles_ 同序的壓縮檔內名字，顯示用
    InfoPanel info_;
    // 預先解碼（在 tempDir_ 之後宣告：先解構，背景停了才刪暫存資料夾）
    Prefetcher prefetch_;
    int lastDir_ = 1;                     // 最近一次翻頁的方向：預先解碼往這個方向準備
    float prefetchCanvasW_ = 0, prefetchCanvasH_ = 0; // 上次 schedulePrefetch 用的畫布大小；變了就再排一次

    // 檢視
    ViewState view_;   // 倍率、位移、跟不跟著檢視模式走、裁切模式存的檢視
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
    std::wstring secondError_;     // 另一頁讀不到時的原因（畫在它的佔位框裡）；讀到了或沒有另一頁是空的
    std::wstring secondErrorLink_; // 另一頁的原因是缺 Store 擴充時，那個擴充的網址
    SpreadCache spreadCache_;      // 目前這本的配對與每頁是不是寬圖；清單或選項變了會自己重算
    bool shiftOne_ = false;        // 「錯開一頁」，不存 ini

    // 拖曳：按下時的滑鼠位置與畫面位移（平移用；其他種類的拖曳狀態在 editor_）
    POINT dragStartClient_{};
    Pt dragOffsetStart_;

    // 文字編輯的輸入框（內容與編輯中的物件在 editor_）
    HWND edit_ = nullptr;
    HFONT editFont_ = nullptr;
    HBRUSH editBrush_ = nullptr;

    // 瀏覽資料夾（縮圖快取宣告在 browse_ 之前：browse_ 的背景執行緒會用它，要先停）
    ThumbCache thumbCache_;          // 自己算的縮圖（SQLite）；暫存位置在網路磁碟、上限是 0 時不開
    bool thumbCacheTried_ = false;
    BrowseView browse_;
    bool browsing_ = false;
    PreviewWorker preview_;          // 預覽圖的背景解碼（只做最新的那一個）
    std::mutex previewMu_;
    struct PreviewDone {
        uint64_t ticket = 0;
        std::wstring path;
        ScaledDecode result;
    };
    std::vector<PreviewDone> previewDone_; // 背景解好、還沒收進來的
    uint64_t previewTicket_ = 0;
    ImagePtr previewImage_;
    std::wstring previewPath_, previewInfo_;
    int previewReqW_ = 0, previewReqH_ = 0; // 上次解預覽用的框大小；框變大很多就重解
    D2D1_RECT_F treeRc_{}, listRc_{}, previewRc_{}, vSplitRc_{}, hSplitRc_{};
    int splitDrag_ = 0;              // 正在拖哪條分隔線（splitterAt 的值）

    // 自動更新
    UpdateFlow updates_{*this};
    bool menuLoop_ = false;     // 選單開著（不在這時跳新版本對話框）
    bool updating_ = false;     // 換檔完成、正在關閉：WM_CLOSE 不再問存檔（已經問過）
    bool justUpdated_ = false;
    std::wstring relaunchArgs_; // 非空：WM_DESTROY 時用這些參數重啟 exe
};

} // namespace mp
