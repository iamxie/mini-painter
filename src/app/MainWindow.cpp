#include "app/MainWindow.h"

#include <thread>

#include <commctrl.h>
#include <commdlg.h>
#include <shellapi.h>
#include <windowsx.h>

#include <algorithm>
#include <cmath>

#include "app/AboutDialog.h"
#include "app/ArchiveDialogs.h"
#include "app/Popups.h"
#include "app/SettingsDialog.h"
#include "app/UiText.h"
#include "app/UpdateDialogs.h"
#include "app/resource.h"
#include "app/version.h"
#include "core/ImageIO.h"
#include "core/ImageOps.h"
#include "core/Launch.h"
#include "core/Lang.h"
#include "core/ShellReg.h"
#include "core/Shelf.h"
#include "core/SysInfo.h"

namespace mp {

namespace {

constexpr wchar_t kClassName[] = L"MiniPainterMain";
constexpr wchar_t kAppName[] = L"mpainter";
constexpr UINT_PTR kTipTimer = 1, kTipHideTimer = 2, kBtnTipTimer = 3, kHqTimer = 4, kToastTimer = 5, kArmTimer = 6,
                   kUpdateTimer = 7, kPrefetchTimer = 8;
// 主視窗自己用的訊息。WM_APP 起的編號全專案只有這些：彈出選單關閉的 WM_MP_POPUP_CLOSED 是 WM_APP + 3（Popups.h），
// 進度對話框內部用 WM_APP + 10 起（Dialogs.cpp）。
constexpr UINT WM_MP_EDIT_LOST_FOCUS = WM_APP + 1; // 文字編輯框把焦點交回主視窗
constexpr UINT WM_MP_ASK_DEFAULT = WM_APP + 2;     // 第一次開圖後詢問預設程式
constexpr UINT WM_MP_UPDATE_FOUND = WM_APP + 4; // lParam = new 出來的 ReleaseInfo*，收到的人 delete
constexpr wchar_t kReleasesPage[] = L"https://github.com/iamxie/mini-painter/releases";

// 自動更新：請同一個 exe 的其他 mpainter 關閉（跨行程，用註冊的訊息）。回 1 = 會關，0 = 使用者在存檔詢問按了取消。
UINT closeForUpdateMessage() {
    static const UINT msg = RegisterWindowMessageW(L"mpainter.CloseForUpdate");
    return msg;
}

// 這些行程的主視窗。
std::vector<HWND> mainWindowsOf(const std::vector<unsigned long>& pids) {
    struct Ctx { const std::vector<unsigned long>* pids; std::vector<HWND> out; } ctx{&pids, {}};
    EnumWindows([](HWND h, LPARAM lp) -> BOOL {
        auto* c = reinterpret_cast<Ctx*>(lp);
        DWORD pid = 0;
        GetWindowThreadProcessId(h, &pid);
        wchar_t cls[64];
        if (std::find(c->pids->begin(), c->pids->end(), pid) != c->pids->end() && GetClassNameW(h, cls, 64) &&
            wcscmp(cls, kClassName) == 0)
            c->out.push_back(h);
        return TRUE;
    }, reinterpret_cast<LPARAM>(&ctx));
    return ctx.out;
}

// 確認按鈕一律叫「好」，所以不用 MessageBox 的 MB_OKCANCEL / MB_OK。
bool askOk(HWND owner, const std::wstring& text, PCWSTR icon, bool withCancel) {
    TASKDIALOG_BUTTON ok{IDOK, tr(L"OK")};
    TASKDIALOGCONFIG td{sizeof(td)};
    td.hwndParent = owner;
    td.dwFlags = TDF_POSITION_RELATIVE_TO_WINDOW | (withCancel ? TDF_ALLOW_DIALOG_CANCELLATION : 0);
    td.dwCommonButtons = withCancel ? TDCBF_CANCEL_BUTTON : 0;
    td.cButtons = 1;
    td.pButtons = &ok;
    td.pszWindowTitle = kAppName;
    td.pszMainIcon = icon;
    td.pszContent = text.c_str();
    int pressed = IDCANCEL;
    if (FAILED(TaskDialogIndirect(&td, &pressed, nullptr, nullptr))) return false;
    return pressed == IDOK;
}
constexpr UINT kToastMs = 2000, kArmMs = 3000;
constexpr UINT kUndoToastMs = 6000; // 復原紀錄快滿的提示比較長，多留一點時間
constexpr UINT kSettingsToastMs = 10000; // 設定檔讀不到的提示：影響整次執行，留久一點
// 啟動時設定檔讀不到：重試幾次、每次隔多久（最多多等 1 秒）
constexpr int kSettingsReadRetries = 10;
constexpr DWORD kSettingsReadRetryMs = 100;
constexpr UINT kHqDelayMs = 200;
constexpr UINT kPrefetchDelayMs = 300; // 拖曳改視窗大小時，停下來才重排預先縮圖
constexpr UINT kTipDelayMs = 500, kTipShowMs = 6000;
constexpr float kToolbarH = 44, kMarkupH = 40, kNameH = 26, kInfoW = 320, kBtnW = 36, kBtnH = 30;
constexpr float kHandleR = 5.0f;
constexpr float kHandleHitExtra = 3.0f; // 控制點比畫出來的圓多幾個像素也算點到
constexpr float kDrawStepPx = 0.5f;     // 塗鴉：滑鼠離上一點至少這麼多螢幕像素才記下一點
constexpr int kClickSlopPx = 2;         // 滑鼠按下後移動超過這麼多像素才算拖曳（不是手抖）
constexpr float kNudgePx = 1.0f, kNudgeLargePx = 10.0f; // 方向鍵微移選取的物件（圖片像素）；按著 Shift 用大的
// 裁切框（DIP）：邊的命中容差、角的命中範圍（沿邊從角算起）、L 形把手的長度
constexpr float kCropTol = 8.0f, kCropCorner = 30.0f, kCropHandleLen = 26.0f;
constexpr UINT_PTR kEditSubclass = 1;

const D2D1_COLOR_F kChromeBg = D2D1::ColorF(0.953f, 0.953f, 0.953f);
const D2D1_COLOR_F kChromeLine = D2D1::ColorF(0.80f, 0.80f, 0.80f);
const D2D1_COLOR_F kCanvasBg = D2D1::ColorF(0.90f, 0.90f, 0.90f);
const D2D1_COLOR_F kIcon = D2D1::ColorF(0.20f, 0.20f, 0.20f);
const D2D1_COLOR_F kIconDisabled = D2D1::ColorF(0.70f, 0.70f, 0.70f);
const D2D1_COLOR_F kBtnHover = D2D1::ColorF(0.87f, 0.87f, 0.87f);
const D2D1_COLOR_F kBtnOn = D2D1::ColorF(0.78f, 0.85f, 0.95f);
const D2D1_COLOR_F kHandleBlue = D2D1::ColorF(0.04f, 0.52f, 1.0f);
const D2D1_COLOR_F kHandleGreen = D2D1::ColorF(0.20f, 0.78f, 0.35f);

bool ctrlDown() { return (GetKeyState(VK_CONTROL) & 0x8000) != 0; }
bool shiftDown() { return (GetKeyState(VK_SHIFT) & 0x8000) != 0; }

bool inRect(const D2D1_RECT_F& r, POINT p) {
    return p.x >= r.left && p.x < r.right && p.y >= r.top && p.y < r.bottom;
}

// 登錄裡給使用者看的文字，跟著目前的語系。
ShellRegOptions shellRegOptions() {
    ShellRegOptions o;
    o.text.openWith = tr(L"Open with mpainter");
    o.text.fileType = tr(L"mpainter image");
    o.text.appDescription = tr(L"Lightweight image viewer and annotator");
    return o;
}

// 開「設定 > 預設應用程式」。Win11 可以直接開到某個程式的頁面；Win10 不認得 registeredAppUser，開整頁。
bool openDefaultAppsSettings() {
    const wchar_t* uri = isWindows11() ? L"ms-settings:defaultapps?registeredAppUser=mpainter" : L"ms-settings:defaultapps";
    auto r = reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr, L"open", uri, nullptr, nullptr, SW_SHOWNORMAL));
    if (r > 32) return true;
    r = reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr, L"open", L"ms-settings:defaultapps", nullptr, nullptr, SW_SHOWNORMAL));
    return r > 32;
}

} // namespace

// ---------------------------------------------------------------------------
// 建立

bool MainWindow::create(HINSTANCE inst, const std::wstring& initialPath, int nCmdShow) {
    inst_ = inst;
    if (!renderer_.init()) return false;
    editor_.setTextMeasurer([this](const Annotation& a) { return textHeight(renderer_.device(), a); });
    loadSettingsAndRegister();
    {
        // 清掉當掉的執行個體留下的壓縮檔暫存（lock 沒人持有的子資料夾）：設定的位置、exe 旁的 temp、退路的 %TEMP%。
        std::wstring root = resolveUnzipRoot(settings_.unzipDir, exeDir()), def = defaultUnzipRoot(exeDir()), fb = fallbackUnzipRoot();
        std::wstring app = exeDir();
        std::thread([root, def, fb, app] {
            sweepStaleTempDirs(root);
            if (_wcsicmp(root.c_str(), def.c_str()) != 0) sweepStaleTempDirs(def);
            sweepStaleTempDirs(fb);
            // 上次自動更新換下來的 .old：舊程式剛結束時可能還刪不掉，多試幾次。
            for (int i = 0; i < 10 && removeOldFiles(app) > 0; ++i) Sleep(500);
        }).detach();
    }

    WNDCLASSEXW wc{sizeof(wc)};
    wc.style = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;
    wc.lpfnWndProc = wndProc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = kClassName;
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(IDI_APP));
    wc.hIconSm = wc.hIcon;
    RegisterClassExW(&wc);

    buildMenu();
    hwnd_ = CreateWindowExW(0, kClassName, kAppName, WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 1100, 760,
                            nullptr, menu_, inst, this);
    if (!hwnd_) return false;

    ui_ = GetDpiForWindow(hwnd_) / 96.0f;
    editBrush_ = CreateSolidBrush(RGB(255, 255, 255));
    DragAcceptFiles(hwnd_, TRUE);
    layout();

    // 上次關閉時的位置與大小（螢幕還在才用；拔掉外接螢幕後就回預設）。
    if (settings_.winW > 0 && settings_.winH > 0) {
        RECT rc{settings_.winX, settings_.winY, settings_.winX + settings_.winW, settings_.winY + settings_.winH};
        if (MonitorFromRect(&rc, MONITOR_DEFAULTTONULL)) {
            WINDOWPLACEMENT wp{sizeof(wp)};
            wp.showCmd = SW_HIDE; // 只放位置，顯示交給 showMainWindow
            wp.rcNormalPosition = rc;
            SetWindowPlacement(hwnd_, &wp);
        }
    }

    if (!initialPath.empty()) openPath(initialPath);
    updateTitle();
    showMainWindow(nCmdShow);
    if (justUpdated_) showToast(fmt1(L"Updated to mpainter %s.", MP_VERSION_WSTRING), false);
    if (settingsUnreadable_)
        showToast(tr(L"Could not read mpainter.ini. Default settings are used this time, and settings will not be saved."), false,
                  kSettingsToastMs);
    if (settings_.autoCheckUpdate) startAutoUpdateCheck();
    return true;
}

void MainWindow::showMainWindow(int cmd) {
    if (IsWindowVisible(hwnd_)) return;
    ShowWindow(hwnd_, settings_.winMax && cmd != SW_SHOWMINIMIZED && cmd != SW_SHOWMINNOACTIVE ? SW_SHOWMAXIMIZED : cmd);
    UpdateWindow(hwnd_);
}

void MainWindow::rememberPlacement() {
    WINDOWPLACEMENT wp{sizeof(wp)};
    if (!GetWindowPlacement(hwnd_, &wp)) return;
    const RECT& rc = wp.rcNormalPosition;
    if (rc.right <= rc.left || rc.bottom <= rc.top) return;
    settings_.winX = rc.left;
    settings_.winY = rc.top;
    settings_.winW = rc.right - rc.left;
    settings_.winH = rc.bottom - rc.top;
    settings_.winMax = wp.showCmd == SW_SHOWMAXIMIZED;
}

std::wstring MainWindow::exeDir() const {
    size_t slash = exePath_.find_last_of(L"\\/");
    return slash == std::wstring::npos ? L"" : exePath_.substr(0, slash + 1);
}

bool MainWindow::preTranslate(MSG& msg) {
    // modeless 的調整顏色面板要靠 IsDialogMessage 才有 Tab / Esc / Enter。
    if (HWND panel = colorAdjustPanelHwnd()) {
        if (IsDialogMessageW(panel, &msg)) return true;
    }
    // 文字編輯框裡的 Esc / Ctrl+Enter 結束編輯，其餘交給編輯框。
    if (edit_ && msg.hwnd == edit_ && msg.message == WM_KEYDOWN) {
        if (msg.wParam == VK_ESCAPE) { endTextEdit(true); return true; }
        if (msg.wParam == VK_RETURN && ctrlDown()) { endTextEdit(true); return true; }
        if (msg.wParam == 'A' && ctrlDown()) { SendMessageW(edit_, EM_SETSEL, 0, -1); return true; }
    }
    return false;
}

LRESULT CALLBACK MainWindow::wndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    MainWindow* self;
    if (msg == WM_NCCREATE) {
        self = static_cast<MainWindow*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        self->hwnd_ = hwnd;
    } else {
        self = reinterpret_cast<MainWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }
    if (!self) return DefWindowProcW(hwnd, msg, wp, lp);
    return self->handle(msg, wp, lp);
}

LRESULT CALLBACK MainWindow::editProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR ref) {
    auto* self = reinterpret_cast<MainWindow*>(ref);
    if (msg == WM_KILLFOCUS) {
        // 焦點跑到別的頂層視窗（例如彈出選單）時不結束編輯；跑回主視窗才結束。
        HWND to = reinterpret_cast<HWND>(wp);
        if (to == self->hwnd_) PostMessageW(self->hwnd_, WM_MP_EDIT_LOST_FOCUS, 0, 0);
    }
    return DefSubclassProc(hwnd, msg, wp, lp);
}

LRESULT MainWindow::handle(UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == closeForUpdateMessage()) {
        // 另一個 mpainter 要裝更新。這邊正忙（對話框或進度框開著、選單開著、拖曳中）就不關，回 0，那邊會取消更新：
        // 這時關視窗，堆疊上還沒做完的流程回來之後會在已經銷毀的視窗上繼續跑。
        if (!canInterrupt()) {
            AllowSetForegroundWindow(static_cast<DWORD>(wp));
            return 0;
        }
        // 有沒存的修改先問（拉到前面讓使用者看得到），按取消就回 0。
        if (doc_.image() && doc_.dirty() && !readOnly()) {
            if (IsIconic(hwnd_)) ShowWindow(hwnd_, SW_RESTORE);
            SetForegroundWindow(hwnd_);
        }
        if (!confirmDiscard()) {
            AllowSetForegroundWindow(static_cast<DWORD>(wp)); // wParam = 要更新的那個行程，讓它能跳到前面說「已取消」
            return 0;
        }
        // 使用者回答存檔詢問的期間，那邊可能已經按取消、不更新了（lParam 是那邊的進度框，取消後就不在了）：那就不關。
        if (lp && !IsWindow(reinterpret_cast<HWND>(lp))) return 0;
        updating_ = true; // WM_CLOSE 不再問
        PostMessageW(hwnd_, WM_CLOSE, 0, 0);
        return 1;
    }
    switch (msg) {
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        BeginPaint(hwnd_, &ps);
        paint();
        EndPaint(hwnd_, &ps);
        return 0;
    }
    case WM_SIZE:
        renderer_.resize(LOWORD(lp), HIWORD(lp));
        layout();
        if (fit_) fitToView(false); else clampOffset();
        positionEdit();
        return 0;
    case WM_DPICHANGED: {
        ui_ = HIWORD(wp) / 96.0f;
        const RECT* r = reinterpret_cast<const RECT*>(lp);
        SetWindowPos(hwnd_, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
        layout();
        updateEditFont();
        return 0;
    }
    case WM_GETMINMAXINFO: {
        auto* mmi = reinterpret_cast<MINMAXINFO*>(lp);
        mmi->ptMinTrackSize = {static_cast<LONG>(px(520)), static_cast<LONG>(px(320))};
        return 0;
    }
    case WM_LBUTTONDOWN: disarmJump(); onLButtonDown({GET_X_LPARAM(lp), GET_Y_LPARAM(lp)}); return 0;
    case WM_RBUTTONDOWN:
    case WM_MBUTTONDOWN: disarmJump(); break; // 待命中的跳本：滑鼠做了別的事就取消
    case WM_LBUTTONDBLCLK: onLButtonDblClk({GET_X_LPARAM(lp), GET_Y_LPARAM(lp)}); return 0;
    case WM_MOUSEMOVE: onMouseMove({GET_X_LPARAM(lp), GET_Y_LPARAM(lp)}, wp); return 0;
    case WM_LBUTTONUP: onLButtonUp({GET_X_LPARAM(lp), GET_Y_LPARAM(lp)}); return 0;
    case WM_CAPTURECHANGED:
        // 拖到一半滑鼠被別的視窗拿走（跳出對話框、切到別的程式）：這次拖曳不算，不然會一直卡在拖曳中。
        if (reinterpret_cast<HWND>(lp) != hwnd_) endDrag(false);
        return 0;
    case WM_MOUSELEAVE:
        trackingMouse_ = false;
        if (hoverBtn_ != BTN_NONE) { hoverBtn_ = BTN_NONE; repaint(); }
        hoverItem_ = 0;
        KillTimer(hwnd_, kTipTimer);
        KillTimer(hwnd_, kBtnTipTimer);
        hideTip();
        return 0;
    case WM_TIMER:
        if (wp == kTipTimer) {
            KillTimer(hwnd_, kTipTimer);
            POINT p;
            GetCursorPos(&p);
            ScreenToClient(hwnd_, &p);
            if (!editor_.dragging() && hoverItem_ && hitItemBox(p) == hoverItem_ && settings_.tipShown < kTipMaxShown) showTip(p);
            return 0;
        }
        if (wp == kTipHideTimer) { hideTip(); return 0; }
        if (wp == kToastTimer) { hideToast(); return 0; }
        if (wp == kArmTimer) { disarmJump(); return 0; }
        if (wp == kUpdateTimer) { KillTimer(hwnd_, kUpdateTimer); offerPendingUpdate(); return 0; }
        if (wp == kPrefetchTimer) { schedulePrefetch(); return 0; }
        if (wp == kHqTimer) {
            KillTimer(hwnd_, kHqTimer);
            hqBusy_ = false;
            repaint();
            return 0;
        }
        if (wp == kBtnTipTimer) {
            KillTimer(hwnd_, kBtnTipTimer);
            if (!editor_.dragging() && pressBtn_ == BTN_NONE && hoverBtn_ != BTN_NONE) showButtonTip(hoverBtn_);
            return 0;
        }
        break;
    case WM_MP_POPUP_CLOSED:
        SetFocus(edit_ ? edit_ : hwnd_);
        return 0;
    case WM_MP_UPDATE_FOUND: {
        std::unique_ptr<ReleaseInfo> r(reinterpret_cast<ReleaseInfo*>(lp));
        if (_wcsicmp(r->version.c_str(), settings_.skipVersion.c_str()) == 0) return 0; // 使用者選過略過這一版
        pendingUpdate_ = std::move(r);
        offerPendingUpdate();
        return 0;
    }
    case WM_ENTERMENULOOP: menuLoop_ = true; break;
    case WM_EXITMENULOOP: menuLoop_ = false; break;
    case WM_MP_ASK_DEFAULT:
        askPending_ = false;
        if (!settings_.askedDefault) askDefaultProgram(false);
        return 0;
    case WM_MOUSEWHEEL:
        disarmJump();
        onMouseWheel({GET_X_LPARAM(lp), GET_Y_LPARAM(lp)}, GET_WHEEL_DELTA_WPARAM(wp), GET_KEYSTATE_WPARAM(wp));
        return 0;
    case WM_MOUSEHWHEEL: {
        disarmJump();
        POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        ScreenToClient(hwnd_, &pt);
        if (inRect(canvasRc_, pt)) { offset_.x -= GET_WHEEL_DELTA_WPARAM(wp); clampOffset(); positionEdit(); repaint(); }
        return 0;
    }
    case WM_SETCURSOR:
        if (LOWORD(lp) == HTCLIENT && onSetCursor()) return TRUE;
        break;
    case WM_KEYDOWN:
        if (onKeyDown(wp)) return 0;
        break;
    case WM_COMMAND:
        if (lp && reinterpret_cast<HWND>(lp) == edit_) {
            if (HIWORD(wp) == EN_CHANGE) onEditChanged();
            return 0;
        }
        onCommand(LOWORD(wp));
        return 0;
    case WM_CTLCOLOREDIT:
        if (reinterpret_cast<HWND>(lp) == edit_) {
            const Annotation* item = doc_.findItem(editor_.editingId());
            Color bg = item && !item->stroke.fill.none() ? item->stroke.fill : Color::rgb(255, 255, 255);
            Color fg = item ? item->text.color : Color::rgb(0, 0, 0);
            HDC dc = reinterpret_cast<HDC>(wp);
            SetBkColor(dc, RGB(bg.r, bg.g, bg.b));
            SetTextColor(dc, RGB(fg.r, fg.g, fg.b));
            if (editBrush_) DeleteObject(editBrush_);
            editBrush_ = CreateSolidBrush(RGB(bg.r, bg.g, bg.b));
            return reinterpret_cast<LRESULT>(editBrush_);
        }
        break;
    case WM_MP_EDIT_LOST_FOCUS:
        if (edit_ && GetFocus() != edit_) endTextEdit(true);
        return 0;
    case WM_DROPFILES: {
        HDROP drop = reinterpret_cast<HDROP>(wp);
        wchar_t buf[MAX_PATH];
        if (DragQueryFileW(drop, 0, buf, MAX_PATH)) openConfirmed(buf);
        DragFinish(drop);
        return 0;
    }
    case WM_INITMENUPOPUP: updateMenu(); return 0;
    case WM_CLOSE:
        // 模態對話框或進度框開著（主視窗被停用）時不關：別的程式送來的 WM_CLOSE（例如 taskkill）也會走到這裡。
        if (!IsWindowEnabled(hwnd_)) return 0;
        if (!updating_ && !confirmDiscard()) return 0; // 更新前已經問過存檔
        closePopup();
        closeColorAdjustPanel();
        rememberPlacement();
        saveSettingsQuiet();
        DestroyWindow(hwnd_);
        return 0;
    case WM_DESTROY:
        prefetch_.shutdown();
        closeArchive();
        tempDir_.release();
        if (!relaunchArgs_.empty()) {
            // 自動更新：檔案已換好，用新的 exe 重開（它會等這個行程結束才開始）。
            std::wstring cmd = quoteArg(exePath_) + L" " + relaunchArgs_;
            STARTUPINFOW si{sizeof(si)};
            PROCESS_INFORMATION pi{};
            if (CreateProcessW(exePath_.c_str(), cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
                AllowSetForegroundWindow(pi.dwProcessId);
                CloseHandle(pi.hThread);
                CloseHandle(pi.hProcess);
            } else {
                MessageBoxW(nullptr, tr(L"mpainter has been updated. Please start it again."), kAppName, MB_ICONINFORMATION);
            }
        }
        if (editFont_) DeleteObject(editFont_);
        if (editBrush_) DeleteObject(editBrush_);
        PostQuitMessage(0);
        return 0;
    default: break;
    }
    return DefWindowProcW(hwnd_, msg, wp, lp);
}

// ---------------------------------------------------------------------------
// 版面

void MainWindow::layout() {
    RECT rc;
    GetClientRect(hwnd_, &rc);
    float w = static_cast<float>(rc.right), h = static_cast<float>(rc.bottom);
    float y = 0;
    toolbarRc_ = D2D1::RectF(0, y, w, y + px(kToolbarH));
    y += px(kToolbarH);
    markupRc_ = showMarkup_ ? D2D1::RectF(0, y, w, y + px(kMarkupH)) : D2D1::RectF(0, y, w, y);
    y = markupRc_.bottom;
    nameRc_ = D2D1::RectF(0, y, w, y + px(kNameH));
    y = nameRc_.bottom;
    float infoW = showInfo_ ? px(kInfoW) : 0;
    canvasRc_ = D2D1::RectF(0, y, w - infoW, h);
    infoRc_ = D2D1::RectF(w - infoW, y, w, h);
    buildButtons();
    if (doc_.image() && (canvasRc_.right - canvasRc_.left != prefetchCanvasW_ || canvasRc_.bottom - canvasRc_.top != prefetchCanvasH_))
        SetTimer(hwnd_, kPrefetchTimer, kPrefetchDelayMs, nullptr);
}

void MainWindow::buildButtons() {
    buttons_.clear();
    float bw = px(kBtnW), bh = px(kBtnH), gap = px(4);
    auto add = [&](int id, float x, float y, bool dropdown = false) {
        Button b;
        b.id = id;
        b.rect = D2D1::RectF(x, y, x + bw, y + bh);
        b.dropdown = dropdown;
        buttons_.push_back(b);
        return x + bw + gap;
    };
    float y = toolbarRc_.top + (px(kToolbarH) - bh) / 2;
    float x = px(10);
    x = add(BTN_ZOOMOUT, x, y);
    x = add(BTN_ZOOM100, x, y);
    x = add(BTN_ZOOMIN, x, y);
    x += px(12);
    x = add(BTN_ROTATE, x, y);
    float right = toolbarRc_.right - px(10) - bw;
    add(BTN_INFO, right, y);
    add(BTN_MARKUP, right - bw - gap, y);

    if (showMarkup_) {
        y = markupRc_.top + (px(kMarkupH) - bh) / 2;
        x = px(10);
        x = add(BTN_FREEHAND, x, y);
        x = add(BTN_SHAPES, x, y, true);
        x = add(BTN_TEXT, x, y);
        x += px(12);
        x = add(BTN_ADJCOLOR, x, y);
        x = add(BTN_ADJSIZE, x, y);
        x = add(BTN_CROP, x, y);
        x += px(12);
        x = add(BTN_LINESTYLE, x, y, true);
        x = add(BTN_LINECOLOR, x, y, true);
        x = add(BTN_FILL, x, y, true);
        x = add(BTN_FORMAT, x, y, true);
        if (editor_.cropping()) {
            Button b;
            b.id = BTN_CROP_APPLY;
            float w = renderer_.measureUiText(tr(L"Crop"), px(13)) + px(28);
            float r = markupRc_.right - px(10);
            b.rect = D2D1::RectF(r - w, y, r, y + bh);
            buttons_.push_back(b);
        }
    }
    CommandState state = commandState();
    state.dragging = false; // 按鈕的灰色不跟著拖曳變（拖曳中本來就按不到）
    for (auto& b : buttons_) {
        switch (b.id) {
        case BTN_ZOOM100: b.enabled = std::abs(zoom_ - 1.0f) > 1e-4f; break;
        case BTN_MARKUP: b.pressed = showMarkup_; break;
        case BTN_INFO: b.pressed = showInfo_; break;
        case BTN_FREEHAND: b.pressed = editor_.freehand(); break;
        case BTN_ADJCOLOR: b.pressed = editor_.adjusting(); break;
        case BTN_CROP: b.pressed = editor_.cropping(); break;
        default: break;
        }
        // 對應到指令的按鈕用指令的可用條件（和選單、快捷鍵同一份）；其他的工具按鈕要有圖才能按。
        const int command = b.id == BTN_ZOOMIN ? IDM_ZOOMIN : b.id == BTN_ZOOMOUT ? IDM_ZOOMOUT : b.id == BTN_ROTATE ? IDM_ROTATE
                          : b.id == BTN_MARKUP ? IDM_MARKUP : b.id == BTN_INFO ? IDM_INFO : IDM_NONE;
        if (command != IDM_NONE) b.enabled = commandEnabled(command, state);
        else if (!doc_.image()) b.enabled = false;
    }
}

CommandState MainWindow::commandState() const {
    CommandState st;
    st.hasImage = doc_.image() != nullptr;
    st.dirty = doc_.dirty();
    st.canUndo = doc_.canUndo();
    st.canRedo = doc_.canRedo();
    st.hasSelection = editor_.selected() != 0;
    st.twoPage = twoPage();
    st.hasPages = nav_.count() > 0;
    st.cropping = editor_.cropping();
    st.profileModified = profileModified(settings_) && findProfile(settings_, settings_.profileName) != nullptr;
    st.dragging = editor_.dragging();
    return st;
}

RECT MainWindow::buttonScreenRect(int id) const {
    RECT r{};
    for (const auto& b : buttons_) {
        if (b.id != id) continue;
        r = {static_cast<LONG>(b.rect.left), static_cast<LONG>(b.rect.top), static_cast<LONG>(b.rect.right),
             static_cast<LONG>(b.rect.bottom)};
        MapWindowPoints(hwnd_, nullptr, reinterpret_cast<POINT*>(&r), 2);
    }
    return r;
}

int MainWindow::hitButton(POINT pt, const Button** out) const {
    for (const auto& b : buttons_) {
        if (inRect(b.rect, pt)) {
            if (out) *out = &b;
            return b.id;
        }
    }
    return BTN_NONE;
}

// ---------------------------------------------------------------------------
// 選單

void MainWindow::buildMenu() {
    menu_ = CreateMenu();
    // 名稱與快捷鍵文字照指令表（core/Commands.cpp）。快捷鍵文字不進翻譯表：翻譯者只翻名稱。
    auto item = [](HMENU m, int id) {
        const CommandInfo* c = commandInfo(id);
        std::wstring t = c ? trs(c->name) : L"";
        if (c && c->shortcut) { t += L'\t'; t += c->shortcut; }
        AppendMenuW(m, MF_STRING, id, t.c_str());
    };
    auto sep = [](HMENU m) { AppendMenuW(m, MF_SEPARATOR, 0, nullptr); };
    HMENU file = CreatePopupMenu();
    item(file, IDM_OPEN);
    item(file, IDM_SAVE);
    item(file, IDM_SAVEAS);
    sep(file);
    item(file, IDM_EXIT);
    HMENU edit = CreatePopupMenu();
    item(edit, IDM_UNDO);
    item(edit, IDM_REDO);
    sep(edit);
    item(edit, IDM_DELETE);
    sep(edit);
    item(edit, IDM_ROTATE);
    sep(edit);
    item(edit, IDM_SETTINGS);
    HMENU view = CreatePopupMenu();
    // 模式（設定集）：選一個整組套上；內容由 rebuildProfileMenu 填
    profileMenu_ = CreatePopupMenu();
    rebuildProfileMenu();
    AppendMenuW(view, MF_POPUP, reinterpret_cast<UINT_PTR>(profileMenu_), tr(L"Mode"));
    sep(view);
    item(view, IDM_ZOOMIN);
    item(view, IDM_ZOOMOUT);
    sep(view);
    // 檢視模式（單選，跨翻頁延續）
    item(view, IDM_ZOOMFIT);
    item(view, IDM_FITWIDTH);
    item(view, IDM_FITHEIGHT);
    item(view, IDM_ZOOM100);
    item(view, IDM_TWOPAGE);
    sep(view);
    item(view, IDM_UPSCALE);
    sep(view);
    // 兩頁並排的選項
    item(view, IDM_RTL);
    item(view, IDM_FIRSTALONE);
    item(view, IDM_SHIFTONE);
    sep(view);
    item(view, IDM_PREV);
    item(view, IDM_NEXT);
    item(view, IDM_AUTONEXT);
    sep(view);
    item(view, IDM_MARKUP);
    item(view, IDM_INFO);
    // 語言：跟隨系統 + 內建 + lang/ 掃到的。切錯語系也找得到回來的路，所以標題兩種語言並列。
    HMENU lang = CreatePopupMenu();
    item(lang, IDM_LANG_SYSTEM);
    sep(lang);
    for (size_t i = 0; i < langs_.size() && IDM_LANG_FIRST + static_cast<int>(i) <= IDM_LANG_LAST; ++i)
        AppendMenuW(lang, MF_STRING, IDM_LANG_FIRST + static_cast<UINT_PTR>(i), langs_[i].name.c_str());
    AppendMenuW(menu_, MF_POPUP, reinterpret_cast<UINT_PTR>(file), tr(L"&File"));
    AppendMenuW(menu_, MF_POPUP, reinterpret_cast<UINT_PTR>(edit), tr(L"&Edit"));
    AppendMenuW(menu_, MF_POPUP, reinterpret_cast<UINT_PTR>(view), tr(L"&View"));
    AppendMenuW(menu_, MF_POPUP, reinterpret_cast<UINT_PTR>(lang), L"語言 / &Language");
    HMENU help = CreatePopupMenu();
    item(help, IDM_ABOUT);
    AppendMenuW(menu_, MF_POPUP, reinterpret_cast<UINT_PTR>(help), tr(L"&Help"));
}

void MainWindow::rebuildProfileMenu() {
    if (!profileMenu_) return;
    while (GetMenuItemCount(profileMenu_) > 0) RemoveMenu(profileMenu_, 0, MF_BYPOSITION);
    for (size_t i = 0; i < settings_.profiles.size() && IDM_PROFILE_FIRST + static_cast<int>(i) <= IDM_PROFILE_LAST; ++i)
        AppendMenuW(profileMenu_, MF_STRING, IDM_PROFILE_FIRST + static_cast<UINT_PTR>(i), settings_.profiles[i].name.c_str());
    AppendMenuW(profileMenu_, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(profileMenu_, MF_STRING, IDM_PROFILE_SAVE, L""); // 文字在 updateMenu 填（帶模式名）
    AppendMenuW(profileMenu_, MF_STRING, IDM_PROFILE_MANAGE, tr(commandInfo(IDM_PROFILE_MANAGE)->name));
}

void MainWindow::switchProfile(const Profile& p) {
    Settings before = settings_;
    applyProfile(settings_, p);
    disarmJump();
    if (!setViewMode(settings_.view.viewMode)) { // 會存 ini、重算配對、重新符合、更新標題、重畫
        // 套用後要換掉正在改的圖，使用者取消：檢視值回到套用前
        settings_ = before;
        if (twoPage()) updateSpreads();
        updateTitle();
    }
}

void MainWindow::saveCurrentToProfile() {
    Profile* p = findProfile(settings_, settings_.profileName);
    if (!p) return;
    Profile cur = profileFromCurrent(settings_);
    cur.name = p->name;
    *p = cur;
    saveSettingsQuiet();
    updateTitle();
}

void MainWindow::updateMenu() {
    auto en = [&](int id, bool on) { EnableMenuItem(menu_, id, MF_BYCOMMAND | (on ? MF_ENABLED : MF_GRAYED)); };
    auto chk = [&](int id, bool on) { CheckMenuItem(menu_, id, MF_BYCOMMAND | (on ? MF_CHECKED : MF_UNCHECKED)); };
    // 模式：目前用的畫圓點，有臨時調整就在名字後面標示；「儲存目前設定到…」只在有調整時能按
    bool modified = profileModified(settings_);
    int curProfile = -1;
    for (size_t i = 0; i < settings_.profiles.size() && IDM_PROFILE_FIRST + static_cast<int>(i) <= IDM_PROFILE_LAST; ++i) {
        bool cur = _wcsicmp(settings_.profiles[i].name.c_str(), settings_.profileName.c_str()) == 0;
        if (cur) curProfile = static_cast<int>(i);
        std::wstring text = settings_.profiles[i].name;
        if (cur && modified) text += std::wstring(L" ") + tr(L"(adjusted)");
        ModifyMenuW(profileMenu_, IDM_PROFILE_FIRST + static_cast<UINT>(i), MF_BYCOMMAND | MF_STRING, IDM_PROFILE_FIRST + static_cast<UINT_PTR>(i), text.c_str());
    }
    if (curProfile >= 0) CheckMenuRadioItem(profileMenu_, IDM_PROFILE_FIRST, IDM_PROFILE_LAST, IDM_PROFILE_FIRST + curProfile, MF_BYCOMMAND);
    ModifyMenuW(profileMenu_, IDM_PROFILE_SAVE, MF_BYCOMMAND | MF_STRING, IDM_PROFILE_SAVE,
                fmt1(L"Save Current Settings to “%s”", settings_.profileName).c_str());
    chk(IDM_LANG_SYSTEM, settings_.language.empty());
    for (size_t i = 0; i < langs_.size(); ++i)
        chk(IDM_LANG_FIRST + static_cast<int>(i), !settings_.language.empty() && _wcsicmp(langs_[i].code.c_str(), settings_.language.c_str()) == 0);
    // 灰不灰照指令表的條件（快捷鍵用的是同一份，選單停用的指令快捷鍵也不動作）。
    const CommandState state = commandState();
    for (const CommandInfo& c : commands()) en(c.id, commandEnabled(c.id, state));
    chk(IDM_AUTONEXT, settings_.view.autoNextBook);
    chk(IDM_MARKUP, showMarkup_);
    chk(IDM_INFO, showInfo_);
    // 檢視模式的單選圓點：這五個 id 之間沒有別的指令（test_commands.cpp 有檢查）。
    int modeId = viewMode_ == ViewMode::FitWidth ? IDM_FITWIDTH : viewMode_ == ViewMode::FitHeight ? IDM_FITHEIGHT
               : viewMode_ == ViewMode::Actual ? IDM_ZOOM100 : viewMode_ == ViewMode::TwoPage ? IDM_TWOPAGE : IDM_ZOOMFIT;
    CheckMenuRadioItem(menu_, kViewModeCommandFirst, kViewModeCommandLast, modeId, MF_BYCOMMAND);
    chk(IDM_UPSCALE, settings_.view.upscaleSmall);
    chk(IDM_RTL, settings_.view.rightToLeft);
    chk(IDM_FIRSTALONE, settings_.view.firstPageAlone);
    chk(IDM_SHIFTONE, shiftOne_);
}

void MainWindow::onCommand(int id) {
    // 選單、快捷鍵、別的程式送來的 WM_COMMAND 都從這裡進來：不能用的指令（含拖曳到一半時的所有指令）一律不做。
    if (!commandEnabled(id, commandState())) return;
    if (id != IDM_PREV && id != IDM_NEXT) disarmJump(); // 待命中的跳本：做了翻頁以外的事就取消
    switch (id) {
    case IDM_OPEN: openDialog(); break;
    case IDM_SAVE: save(); break;
    case IDM_SAVEAS: exportAs(); break;
    case IDM_EXIT: PostMessageW(hwnd_, WM_CLOSE, 0, 0); break;
    case IDM_UNDO:
    case IDM_REDO: {
        if (editor_.cropping()) { cancelCrop(); break; } // 裁切模式中只取消裁切，不動編輯歷史
        // 調整顏色的面板也要收：它拿著復原前那張圖當底，之後按「好」會把那張圖換回來。
        endInteraction(true);
        if (id == IDM_UNDO ? !doc_.canUndo() : !doc_.canRedo()) break;
        auto before = doc_.image();
        if (id == IDM_UNDO) editor_.undo(); else editor_.redo();
        auto after = doc_.image();
        // 底圖尺寸變了（旋轉、調整尺寸）就重新符合視窗。
        if (before && after && (before->width != after->width || before->height != after->height)) {
            fit_ = true;
            fitToView();
        }
        repaint();
        updateTitle();
        break;
    }
    case IDM_DELETE: deleteSelected(); break;
    case IDM_ROTATE: rotateImage(); break;
    case IDM_SETTINGS: showSettings(0); break;
    case IDM_PROFILE_MANAGE: showSettings(1); break;
    case IDM_PROFILE_SAVE: saveCurrentToProfile(); break;
    case IDM_ZOOMIN: zoomStep(1); break;
    case IDM_ZOOMOUT: zoomStep(-1); break;
    case IDM_ZOOM100: setViewMode(ViewMode::Actual); break;
    case IDM_ZOOMFIT: setViewMode(ViewMode::FitWindow); break;
    case IDM_FITWIDTH: setViewMode(ViewMode::FitWidth); break;
    case IDM_FITHEIGHT: setViewMode(ViewMode::FitHeight); break;
    case IDM_TWOPAGE: setViewMode(ViewMode::TwoPage); break;
    case IDM_UPSCALE:
        settings_.view.upscaleSmall = !settings_.view.upscaleSmall;
        saveSettingsQuiet();
        if (fit_) { fitToView(); repaint(); }
        updateTitle();
        schedulePrefetch();
        break;
    case IDM_RTL:
        settings_.view.rightToLeft = !settings_.view.rightToLeft;
        saveSettingsQuiet();
        if (twoPage()) { fit_ = true; fitToView(); }
        updateTitle();
        repaint();
        break;
    case IDM_FIRSTALONE:
    case IDM_SHIFTONE: {
        bool& flag = id == IDM_FIRSTALONE ? settings_.view.firstPageAlone : shiftOne_;
        flag = !flag;
        if (twoPage()) {
            // 配對變了：重算，目前這頁可能變成某屏的第二頁，重開到那屏的第一頁（換圖前先問，取消就不改）。
            updateSpreads();
            if (!confirmSpreadMove()) {
                flag = !flag;
                updateSpreads();
                break;
            }
        }
        if (id == IDM_FIRSTALONE) { saveSettingsQuiet(); updateTitle(); }
        if (twoPage()) {
            int cur = currentSpread();
            if (cur >= 0 && spreads()[static_cast<size_t>(cur)].first != nav_.index()) openPath(nav_.at(spreads()[static_cast<size_t>(cur)].first));
            else { loadSecond(); fit_ = true; fitToView(); updateTitle(); repaint(); }
        }
        schedulePrefetch();
        break;
    }
    case IDM_PREV: goPrev(); break;
    case IDM_NEXT: goNext(); break;
    case IDM_AUTONEXT:
        settings_.view.autoNextBook = !settings_.view.autoNextBook;
        disarmJump();
        saveSettingsQuiet();
        updateTitle();
        schedulePrefetch();
        break;
    case IDM_MARKUP: onButton(BTN_MARKUP); break;
    case IDM_INFO: onButton(BTN_INFO); break;
    case IDM_LANG_SYSTEM: selectLanguage(L""); break;
    case IDM_ABOUT: endTextEdit(true); if (showAboutDialog(hwnd_)) checkForUpdatesNow(); break;
    default:
        if (id >= IDM_LANG_FIRST && id <= IDM_LANG_LAST && static_cast<size_t>(id - IDM_LANG_FIRST) < langs_.size())
            selectLanguage(langs_[id - IDM_LANG_FIRST].code);
        else if (id >= IDM_PROFILE_FIRST && id <= IDM_PROFILE_LAST && static_cast<size_t>(id - IDM_PROFILE_FIRST) < settings_.profiles.size())
            switchProfile(settings_.profiles[static_cast<size_t>(id - IDM_PROFILE_FIRST)]);
        break;
    }
}

// ---------------------------------------------------------------------------
// 檔案

bool MainWindow::openConfirmed(const std::wstring& path) { return confirmLeaveImage() && openPath(path); }

void MainWindow::showLoadError(const std::wstring& path, const std::wstring& message, bool resetList) {
    path_ = path;
    if (resetList) {
        nav_ = FolderNav{};
        infoData_ = {};
    }
    loadError_ = message;
    editor_.reset(nullptr);
    second_.reset();
    secondPath_.clear();
    updateTitle();
    layout();
    repaint();
}

bool MainWindow::openPath(const std::wstring& path, bool landAtBottom) {
    endInteraction(false); // 正在打的字丟掉：那張圖要被換掉了
    closePopup();
    hideTip();
    disarmJump();
    hoverItem_ = 0;
    editor_.select(0);
    // 先決定要開的是什麼（core/Shelf.h）：壓縮檔交給 openArchive；資料夾換成裡面的第一張圖或第一本書。
    const PathKind kind = classifyPath(path);
    if (kind == PathKind::Archive) return openArchive(path);
    // 不是目前壓縮檔解出來的檔就離開壓縮檔模式（刪暫存）。
    bool inArchive = insideArchive(path);
    if (!inArchive) closeArchive();
    if (kind == PathKind::Folder) {
        // 右鍵資料夾「用 mpainter 開啟」：從第一張開始。沒有散圖但有書（子資料夾 / 壓縮檔）就開第一本。
        const std::wstring first = firstInFolder(path);
        if (!first.empty()) return openPath(first, landAtBottom);
        showLoadError(path, tr(L"This folder has no supported images"), true);
        return false;
    }
    LoadResult r = loadPage(path);
    path_ = path;
    loadedFrom_ = path;
    if (inArchive) nav_.setFiles(archiveFiles_, path);
    else nav_.load(path);
    infoData_ = {};
    fillFileFacts(infoData_, path);
    if (inArchive) {
        infoData_.fileName = displayName(path);
        infoData_.filePath = archivePath_;
    }
    infoState_.scroll = 0;
    if (!r.image) {
        showLoadError(path, fmt1(L"Cannot open: %s", ioErrorText(r.error)), false);
        schedulePrefetch();
        return false;
    }
    if (!settings_.askedDefault && !askPending_) {
        askPending_ = true;
        PostMessageW(hwnd_, WM_MP_ASK_DEFAULT, 0, 0);
    }
    loadError_.clear();
    editor_.reset(r.image, r.dpi);
    second_.reset();
    secondPath_.clear();
    if (viewMode_ == ViewMode::TwoPage) {
        updateSpreads();
        int cur = currentSpread();
        if (cur >= 0 && spreads()[static_cast<size_t>(cur)].first != nav_.index()) {
            // 開到的是某屏的第二頁：改開那屏的第一頁（它會再載入這一頁當第二頁）。
            return openPath(nav_.at(spreads()[static_cast<size_t>(cur)].first), landAtBottom);
        }
        loadSecond();
    }
    infoData_.info = r.info;
    infoData_.width = r.image->width;
    infoData_.height = r.image->height;
    infoData_.dpiX = r.dpi.x;
    infoData_.dpiY = r.dpi.y;
    fit_ = true;
    fitToView();
    if (landAtBottom) {
        offset_.y = -1e9f;
        clampOffset();
    }
    updateTitle();
    layout();
    repaint();
    schedulePrefetch();
    return true;
}

LoadResult MainWindow::loadPage(const std::wstring& path) {
    if (auto r = prefetch_.get(path)) return *r;
    LoadResult r = loadImage(path);
    if (settings_.prefetchMB > 0) prefetch_.put(path, r);
    return r;
}

void MainWindow::schedulePrefetch() {
    KillTimer(hwnd_, kPrefetchTimer);
    float cw = canvasRc_.right - canvasRc_.left, ch = canvasRc_.bottom - canvasRc_.top;
    prefetchCanvasW_ = cw;
    prefetchCanvasH_ = ch;
    size_t limit = static_cast<size_t>(settings_.prefetchMB) * 1024 * 1024;
    if (limit == 0 || !doc_.image() || nav_.index() < 0) {
        prefetch_.setWanted({}, {}, std::nullopt, {}, 0);
        return;
    }
    std::vector<std::wstring> current{path_};
    if (twoPage() && second_) current.push_back(secondPath_);
    // 翻頁方向的下一屏優先，再來是上一屏（翻回來也不用等）。
    WantedPages wanted = wantedPages(nav_.files(), nav_.index(), lastDir_, twoPage() ? &spreads() : nullptr);
    std::vector<std::vector<std::wstring>> others = std::move(wanted.others);
    const bool atEdge = wanted.atEdge;
    // 到一本的盡頭、開了「接著看下一本」：先準備同一層的下一本（或上一本）。
    std::optional<BookRequest> book;
    std::wstring from = currentBook();
    if (atEdge && settings_.view.autoNextBook && !from.empty()) {
        BookRequest req;
        req.fromBook = from;
        req.dir = lastDir_;
        req.twoPage = viewMode_ == ViewMode::TwoPage;
        req.firstAlone = settings_.view.firstPageAlone;
        req.shiftOne = shiftOne_;
        req.pinned = settings_.pinnedNames;
        req.passwords = settings_.passwords;
        req.archiveRoot = prefetchArchiveRoot();
        book = std::move(req);
    }
    // 高品質模式：照翻過去之後 fitToView 會算出的倍率，先縮好。
    TargetSizes target;
    if (settings_.view.highQuality) {
        ViewMode mode = viewMode_;
        bool upscale = settings_.view.upscaleSmall;
        unsigned maxSide = renderer_.rt() ? renderer_.rt()->GetMaximumBitmapSize() : 16384;
        // 以值捕捉：這個函式在背景執行緒被呼叫。算法和畫面同一份（core/ViewFit 的 screenHqTargets）。
        target = [mode, upscale, maxSide, cw, ch](const std::vector<Size2>& pages) {
            return screenHqTargets(mode, cw, ch, upscale, maxSide, pages);
        };
    }
    prefetch_.setWanted(std::move(current), std::move(others), std::move(book), std::move(target), limit);
}

std::wstring MainWindow::prefetchArchiveRoot() {
    if (tempDirStale_) return L""; // 暫存位置改了、還沒搬：這一本先不預開
    if (!ensureArchiveLibrary() || !ensureTempDir()) return L"";
    return tempDir_.path();
}

// 7z.dll 放在 exe 旁邊，第一次用到壓縮檔時才載入。
bool MainWindow::ensureArchiveLibrary() {
    archiveLoad_ = loadArchiveLibrary(exeDir() + L"7z.dll"); // 載過就直接回 Ok
    return archiveLoad_.ok();
}

std::wstring MainWindow::archiveLibraryProblem() const {
    if (archiveLoad_.status == LibraryLoad::LoadFailed) {
        // 系統的說明裡有 %1（「%1 不是有效的 Win32 應用程式」）：填上檔名。
        std::wstring why = systemErrorText(archiveLoad_.systemError);
        if (size_t pos = why.find(L"%1"); pos != std::wstring::npos) why.replace(pos, 2, L"7z.dll");
        return fmt1(L"7z.dll next to mpainter.exe could not be loaded: %s", why);
    }
    return tr(L"7z.dll was not found next to mpainter.exe, so archives cannot be opened.");
}

// 這個執行個體的解壓暫存資料夾：照設定的位置建，建不了（exe 在唯讀的地方）退到 %TEMP%。
bool MainWindow::ensureTempDir() {
    return tempDir_.valid() || tempDir_.create(resolveUnzipRoot(settings_.unzipDir, exeDir())) ||
           tempDir_.create(fallbackUnzipRoot());
}

// ---------------------------------------------------------------------------
// 壓縮檔

bool MainWindow::insideArchive(const std::wstring& path) const { return isInsideDir(archiveDir_, path); }

std::wstring MainWindow::archiveNameOf(const std::wstring& path) const {
    return archiveEntryName(archiveDir_, archiveFiles_, archiveNames_, path);
}

std::wstring MainWindow::displayName(const std::wstring& path) const {
    if (!insideArchive(path)) return fileNameOf(path);
    return fileNameOf(archivePath_) + L" › " + archiveNameOf(path);
}

void MainWindow::closeArchive() {
    if (archivePath_.empty()) return;
    archivePath_.clear();
    archiveFiles_.clear();
    archiveNames_.clear();
    std::wstring dir = archiveDir_;
    archiveDir_.clear();
    removeDirectoryRecursive(dir);
    // 設定面板改過暫存位置（tempDirStale_）的話，下一次 openArchive 開始時才搬：這裡不能動 tempDir_，
    // 因為跳本時新的那本已經解在裡面了。
}

bool MainWindow::editGuard() {
    if (!readOnly()) return true;
    endTextEdit(false);
    MessageBoxW(hwnd_, tr(L"This image is inside an archive and is read-only.\n\nExtract the archive first if you want to edit it."),
                kAppName, MB_OK | MB_ICONINFORMATION);
    return false;
}

bool MainWindow::openArchive(const std::wstring& path, std::wstring* failMsg, bool openLast) {
    // 從檔案總管直接開加密檔時主視窗還沒顯示；先顯示，密碼與進度對話框才拿得到焦點。
    showMainWindow(SW_SHOW);
    // 目前這本（可能也是壓縮檔）要等新的解成功才關：跳本失敗時留在原本那本。
    auto fail = [&](const std::wstring& msg) {
        if (failMsg) { *failMsg = msg; return false; }
        closeArchive();
        showMarkup_ = false;
        editor_.leaveFreehand();
        showLoadError(path, msg, true);
        return false;
    };
    if (!ensureArchiveLibrary()) return fail(archiveLibraryProblem());
    if (tempDirStale_ && archivePath_.empty()) { tempDir_.release(); tempDirStale_ = false; }
    if (!ensureTempDir()) return fail(tr(L"Cannot create the temp folder for unpacking archives."));
    // 換本預備：背景預開過這一本就等它解完直接用（含密碼列表試出來的密碼），不再解一次。
    std::optional<BookPlan> pre;
    runUnpackWithProgress(hwnd_, path, [&](const std::function<bool(uint64_t, uint64_t)>& progress) {
        pre = prefetch_.takeBook(path, progress);
    });
    if (pre && pre->extracted.status == ArchiveStatus::Cancelled) return fail(L"");
    bool adopted = pre && pre->usable();
    if (pre && !adopted && !pre->dest.empty()) removeDirectoryRecursive(pre->dest);
    std::wstring dest = adopted ? pre->dest : tempDir_.path() + L"\\" + std::to_wstring(++archiveSeq_);
    ExtractOptions opt;
    ArchiveResult r;
    bool fromList = false; // 用的是密碼列表裡的密碼：它解不開時不算使用者打錯
    if (adopted) {
        opt.password = pre->password;
        for (const auto& [f, wide] : pre->wide) spreadCache_.remember(f, wide); // 每頁寬高讀過了，updateSpreads 不用再讀檔頭
        r = pre->extracted;
    } else {
        // 先不帶密碼解，要密碼就把密碼列表試一輪（core 的 extractWithPasswords）。背景預開時已經試過、都不對的話不再試一次。
        static const std::vector<std::wstring> kNoList;
        const bool listTried = pre && pre->passwordsTried;
        PasswordExtract x = extractWithProgress(hwnd_, path, dest, opt, listTried ? kNoList : settings_.passwords);
        r = std::move(x.result);
        opt.password = x.password;
        fromList = !x.password.empty();
    }
    while (r.status == ArchiveStatus::NeedPassword || r.status == ArchiveStatus::WrongPassword) {
        removeDirectoryRecursive(dest);
        // 列表裡的密碼試過了不算使用者打錯，當第一次問。
        bool retry = r.status == ArchiveStatus::WrongPassword && !fromList;
        fromList = false;
        std::wstring pw;
        if (!showPasswordDialog(hwnd_, fileNameOf(path), retry, pw)) { r.status = ArchiveStatus::Cancelled; break; }
        opt.password = pw;
        r = extractWithProgress(hwnd_, path, dest, opt).result;
    }
    if (r.status != ArchiveStatus::Ok && r.status != ArchiveStatus::Incomplete) {
        removeDirectoryRecursive(dest);
        switch (r.status) {
        case ArchiveStatus::Cancelled: return fail(L"");
        case ArchiveStatus::NoImages: return fail(tr(L"This archive has no supported images"));
        case ArchiveStatus::TooLarge: return fail(fmt1(L"The images in this archive are too large to unpack (limit %s).", fileSizeText(kArchiveMaxTotalBytes)));
        case ArchiveStatus::NoSpace: return fail(tr(L"Not enough free disk space in the temp folder to unpack this archive."));
        case ArchiveStatus::NotArchive:
            return fail(fmt1(L"Cannot open: %s", r.error.empty() ? tr(L"not a supported archive") : archiveErrorText(r.error)));
        default: return fail(fmt1(L"Cannot unpack: %s", archiveErrorText(r.error)));
        }
    }
    if (!opt.password.empty() &&
        std::find(settings_.passwords.begin(), settings_.passwords.end(), opt.password) == settings_.passwords.end()) {
        settings_.passwords.push_back(opt.password);
        saveSettingsQuiet();
    }
    if (r.status == ArchiveStatus::Incomplete) {
        // 不默默開：先說少了幾張，再開剩下的。
        wchar_t msg[512];
        swprintf(msg, 512, tr(L"Unpacking was incomplete: only %d of %d images were written. The missing ones are skipped."),
                 r.written, r.expected);
        MessageBoxW(hwnd_, msg, kAppName, MB_OK | MB_ICONWARNING);
        if (r.files.empty()) { removeDirectoryRecursive(dest); return fail(tr(L"This archive has no supported images")); }
    }
    movePinnedFirst(r.files, r.names, settings_.pinnedNames);
    closeArchive();
    showMarkup_ = false;
    editor_.leaveFreehand();
    archivePath_ = path;
    archiveDir_ = dest;
    archiveFiles_ = r.files;
    archiveNames_ = r.names;
    return openPath(openLast ? r.files.back() : r.files.front());
}

bool MainWindow::confirmDiscard(bool leavingImage) {
    endTextEdit(true);
    if (!doc_.image() || !doc_.dirty() || readOnly()) return true;
    wchar_t text[MAX_PATH + 128];
    swprintf(text, MAX_PATH + 128, tr(L"“%s” has unsaved changes. Save them?"), fileNameOf(path_).c_str());
    std::wstring msg = text;
    if (leavingImage) msg += std::wstring(L"\n\n") + tr(L"Either way, the undo history of this image is cleared.");
    int r = MessageBoxW(hwnd_, msg.c_str(), kAppName, MB_YESNOCANCEL | MB_ICONQUESTION);
    if (r == IDCANCEL) return false;
    if (r == IDYES) return save();
    return true;
}

// 壓縮檔內唯讀：旋轉當成看圖動作，換圖不問。
bool MainWindow::confirmLeaveImage() {
    endTextEdit(true);
    if (!doc_.image() || readOnly()) return true;
    switch (doc_.leavePrompt()) {
    case LeavePrompt::AskSave: return confirmDiscard(true);
    case LeavePrompt::WarnHistory:
        return askOk(hwnd_, tr(L"Moving to another image clears the undo history of this one, so these changes can no longer be undone. Continue?"),
                     TD_WARNING_ICON, true);
    default: return true;
    }
}

bool MainWindow::save() {
    if (!doc_.image()) return false;
    if (!editGuard()) return false;
    endInteraction(true);
    if (!canSaveExtension(extensionOf(path_))) return saveAs();
    if (!writeFlattened(path_)) return false;
    loadedFrom_ = path_;
    doc_.markSaved();
    fillFileFacts(infoData_, path_);
    updateTitle();
    repaint();
    return true;
}

bool MainWindow::writeFlattened(const std::wstring& target) {
    SaveOptions opt;
    opt.metadataSource = loadedFrom_; // 全部照抄，只改與像素不符的欄位
    opt.dpi = doc_.dpi();
    SetCursor(LoadCursorW(nullptr, IDC_WAIT));
    SaveResult saved;
    if (doc_.items().empty()) {
        saved = saveImage(*doc_.image(), target, opt); // 沒有標註：底圖就是要存的像素
    } else {
        Image flat;
        if (!flatten(renderer_.device(), doc_.state(), flat)) {
            // 標註畫不上去（多半是圖太大、記憶體不夠）：不能把少了標註的圖當成存好了。
            MessageBoxW(hwnd_, fmt1(L"Save failed: %s", tr(L"Could not draw the annotations onto the image")).c_str(), kAppName,
                        MB_ICONERROR);
            return false;
        }
        saved = saveImage(std::move(flat), target, opt);
    }
    if (!saved) {
        MessageBoxW(hwnd_, fmt1(L"Save failed: %s", ioErrorText(saved.error)).c_str(), kAppName, MB_ICONERROR);
        return false;
    }
    // 圖存好了，但原檔的 metadata 有一部分寫不進去：存檔不擋，事後說一聲。
    if (saved.metadataDropped) showToast(tr(L"Saved, but some of the original metadata could not be written to the file."), false);
    // 寬高可能變了（旋轉、裁切、調整尺寸）：兩頁並排的配對要照存好的檔重算。
    spreadCache_.invalidateFile(target);
    return true;
}

bool MainWindow::pickSavePath(std::wstring& out, bool exporting) {
    // 壓縮檔內的檔預設輸出到壓縮檔旁邊、用內部檔名（磁碟上的是序號），不能是暫存資料夾。
    std::wstring initial = path_;
    if (insideArchive(path_)) {
        std::wstring name = sanitizeFileName(fileNameOf(archiveNameOf(path_)));
        if (extensionOf(name).empty()) name += extensionOf(path_);
        initial = archivePath_.substr(0, archivePath_.find_last_of(L"\\/") + 1) + name;
    }
    wchar_t buf[MAX_PATH];
    wcsncpy_s(buf, initial.c_str(), _TRUNCATE);
    OPENFILENAMEW ofn{sizeof(ofn)};
    ofn.hwndOwner = hwnd_;
    if (exporting) ofn.lpstrTitle = tr(L"Export As");
    // 篩選、預設選哪一種、沒打副檔名時補什麼，都照 core 的格式表。
    const std::vector<ImageFormat>& formats = saveDialogFormats();
    const std::wstring filter = saveDialogFilter();
    ofn.lpstrFilter = filter.c_str();
    auto current = std::find(formats.begin(), formats.end(), imageFormatOfExtension(extensionOf(path_)));
    ofn.nFilterIndex = current == formats.end() ? 1 : static_cast<DWORD>(current - formats.begin()) + 1;
    ofn.lpstrFile = buf;
    ofn.nMaxFile = MAX_PATH;
    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    ofn.lpstrDefExt = imageFormatInfo(formats.front())->extensions.front() + 1; // 不含點
    if (!GetSaveFileNameW(&ofn)) return false;
    std::wstring newPath = buf;
    if (!canSaveExtension(extensionOf(newPath))) {
        int picked = std::clamp<int>(static_cast<int>(ofn.nFilterIndex) - 1, 0, static_cast<int>(formats.size()) - 1);
        newPath += imageFormatInfo(formats[static_cast<size_t>(picked)])->extensions.front();
    }
    std::wstring extUpper = extensionOf(newPath).substr(1);
    for (auto& ch : extUpper) ch = static_cast<wchar_t>(towupper(ch));
    // 透明圖存成 JPEG / BMP：存檔時會鋪白底，先說。只看底圖，標註剛好蓋住所有透明處時也會問。
    if (!extensionKeepsTransparency(extensionOf(newPath)) && hasTransparency(*doc_.image())) {
        if (!askOk(hwnd_, fmt1(L"This image has transparent areas. %s cannot keep transparency, so they will become white. Continue?", extUpper),
                   TD_WARNING_ICON, true))
            return false;
    }
    // 跨格式：目標裝不下的 metadata 區塊先列出來問。
    if (settings_.view.warnMetadataLoss) {
        // 來源的格式看檔案內容（infoData_.info.format），不看 loadedFrom_ 的副檔名：兩者可能不一樣。
        auto lost = metadataLossOnSave(infoData_.info, imageFormatOfExtension(extensionOf(newPath)));
        if (!lost.empty()) {
            std::wstring list;
            for (size_t i = 0; i < lost.size(); ++i) list += (i ? L", " : L"") + trs(lost[i].c_str());
            wchar_t body[512];
            swprintf(body, 512, tr(L"Saving as %s will drop this metadata:\n%s\n\nThe pixels are kept. Continue?"), extUpper.c_str(), list.c_str());
            TASKDIALOGCONFIG td{sizeof(td)};
            td.hwndParent = hwnd_;
            td.hInstance = inst_;
            td.dwFlags = TDF_ALLOW_DIALOG_CANCELLATION | TDF_POSITION_RELATIVE_TO_WINDOW;
            td.dwCommonButtons = TDCBF_OK_BUTTON | TDCBF_CANCEL_BUTTON;
            td.pszWindowTitle = kAppName;
            td.pszMainIcon = TD_WARNING_ICON;
            td.pszContent = body;
            td.pszVerificationText = tr(L"Don't ask again");
            int pressed = 0;
            BOOL dontAsk = FALSE;
            if (FAILED(TaskDialogIndirect(&td, &pressed, nullptr, &dontAsk))) pressed = IDOK;
            if (dontAsk) { settings_.view.warnMetadataLoss = false; saveSettingsQuiet(); updateTitle(); }
            if (pressed != IDOK) return false;
        }
    }
    out = newPath;
    return true;
}

bool MainWindow::saveAs() {
    if (!doc_.image()) return false;
    if (!editGuard()) return false;
    endTextEdit(true);
    std::wstring newPath;
    if (!pickSavePath(newPath, false)) return false;
    // save() 寫的是 path_。寫不進去的話要換回來：不然之後編輯的對象變成一個不存在的檔。
    const std::wstring original = path_;
    path_ = newPath;
    if (!save()) {
        path_ = original;
        return false;
    }
    nav_.load(path_);
    fillFileFacts(infoData_, path_);
    updateTitle();
    repaint();
    return true;
}

bool MainWindow::exportAs() {
    if (!doc_.image()) return false;
    endTextEdit(true);
    cancelCrop();
    std::wstring newPath;
    if (!pickSavePath(newPath, true)) return false;
    return writeFlattened(newPath); // 目前編輯的還是原檔，dirty 不變
}

void MainWindow::openDialog() {
    if (!confirmLeaveImage()) return;
    wchar_t buf[MAX_PATH] = L"";
    OPENFILENAMEW ofn{sizeof(ofn)};
    ofn.hwndOwner = hwnd_;
    // 篩選字串以 \0 分段，不能整串進翻譯表，分段組。
    std::wstring filter = trs(L"Image files");
    filter += L'\0';
    std::wstring archivePatterns;
    for (const auto& e : archiveExtensions()) archivePatterns += (archivePatterns.empty() ? L"*" : L";*") + e;
    filter += allImagePatterns() + L";" + archivePatterns;
    filter += L'\0';
    filter += trs(L"Archives (zip, 7z, rar, cbz, cb7, cbr)");
    filter += L'\0';
    filter += archivePatterns;
    filter += L'\0';
    filter += trs(L"All files");
    filter += L'\0';
    filter += L"*.*";
    filter += L'\0';
    ofn.lpstrFilter = filter.c_str();
    ofn.lpstrFile = buf;
    ofn.nMaxFile = MAX_PATH;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_NOCHANGEDIR;
    if (GetOpenFileNameW(&ofn)) openPath(buf);
}

void MainWindow::goPrev() { step(-1); }
void MainWindow::goNext() { step(+1); }

void MainWindow::step(int dir, bool landAtBottom) {
    if (armedDir_ && armedDir_ != dir) disarmJump();
    if (twoPage() && currentSpread() < 0) return; // 目前這頁不在配對裡（清單讀不到）：不知道往哪翻
    const int target = pageTurnTarget(dir, nav_.index(), nav_.count(), twoPage() ? &spreads() : nullptr);
    if (target < 0) { bookEdge(dir); return; }
    if (!confirmLeaveImage()) return;
    lastDir_ = dir;
    openPath(nav_.at(target), landAtBottom);
}

// 翻到底。工作模式（沒開「接著看下一本」）只說到底了：在「備選」資料夾挑圖的人不會想被帶到隔壁的「棄選」。
// 看漫畫模式第一次按先提示要去哪一本，3 秒內再按同方向才跳；跳過去再提示一次進了哪本。
void MainWindow::bookEdge(int dir) {
    if (nav_.count() == 0) return;
    if (!settings_.view.autoNextBook) {
        showToast(dir > 0 ? tr(L"This is the last image") : tr(L"This is the first image"), false);
        return;
    }
    if (armedDir_ == dir && !armedTarget_.empty()) {
        std::wstring target = armedTarget_;
        disarmJump();
        if (confirmLeaveImage()) openBook(target, dir);
        return;
    }
    disarmJump();
    std::wstring book = currentBook();
    std::wstring target = book.empty() ? L"" : neighborBook(book, dir);
    if (target.empty()) {
        showToast(dir > 0 ? tr(L"This is the last image. There is no next folder or archive here.")
                          : tr(L"This is the first image. There is no previous folder or archive here."), false);
        return;
    }
    armedDir_ = dir;
    armedTarget_ = target;
    SetTimer(hwnd_, kArmTimer, kArmMs, nullptr);
    // 剛從另一個方向翻過來（例如從下一本退回這本的最後一屏）時，下一本還沒準備：趁待命的這段時間在背景準備。
    if (lastDir_ != dir) {
        lastDir_ = dir;
        schedulePrefetch();
    }
    showToast(dir > 0 ? fmt2(L"%s is finished. Press Next again to start %s", fileNameOf(book), fileNameOf(target))
                      : fmt2(L"This is the first page of %s. Press Previous again to go back to the end of %s",
                             fileNameOf(book), fileNameOf(target)), true);
}

bool MainWindow::openBook(const std::wstring& book, int dir) {
    // 失敗（密碼對話框取消、解壓失敗、沒有圖）就留在原本這本，說一聲為什麼；再按翻頁會重新走「提示 → 再按一次」。
    std::wstring from = fileNameOf(currentBook());
    bool ok = false;
    lastDir_ = dir;
    if (isDirectoryPath(book)) {
        // 背景準備過這一本的話，每頁寬高已經讀過，updateSpreads 不用再讀檔頭。
        if (auto plan = prefetch_.takeBook(book, nullptr); plan && plan->usable())
            for (const auto& [f, wide] : plan->wide) spreadCache_.remember(f, wide);
        auto files = listImageFiles(book);
        movePinnedFirst(files, settings_.pinnedNames);
        if (files.empty()) { showToast(fmt2(L"Could not open %s: %s", fileNameOf(book), tr(L"This folder has no supported images")), false); return false; }
        ok = openPath(dir > 0 ? files.front() : files.back());
        if (!ok) showToast(fmt2(L"Could not open %s: %s", fileNameOf(book), loadError_), false);
    } else {
        std::wstring err;
        // 往回跳直接開最後一頁（換本預備解好的是最後一屏），不先開第一頁。
        ok = openArchive(book, &err, dir < 0);
        // 壓縮檔開了、只是那一頁解不開時，已經在新的這本（錯誤畫在畫布上），不是「取消」。
        if (!ok && archivePath_ != book) {
            showToast(err.empty() ? fmt1(L"Cancelled. Still in %s", from) : fmt2(L"Could not open %s: %s", fileNameOf(book), err), false);
            return false;
        }
    }
    if (ok) showToast(dir > 0 ? fmt1(L"Now in %s", fileNameOf(book)) : fmt1(L"Back in %s", fileNameOf(book)), false);
    return ok;
}

std::wstring MainWindow::currentBook() const {
    if (!archivePath_.empty()) return archivePath_;
    if (path_.empty()) return L"";
    size_t slash = path_.find_last_of(L"\\/");
    return slash == std::wstring::npos ? L"" : path_.substr(0, slash);
}

void MainWindow::disarmJump() {
    if (!armedDir_ && !toastSticky_) return;
    armedDir_ = 0;
    armedTarget_.clear();
    KillTimer(hwnd_, kArmTimer);
    if (toastSticky_) hideToast();
}

void MainWindow::showToast(const std::wstring& text, bool sticky, UINT ms) {
    toast_ = text;
    toastSticky_ = sticky;
    KillTimer(hwnd_, kToastTimer);
    if (!sticky) SetTimer(hwnd_, kToastTimer, ms ? ms : kToastMs, nullptr);
    repaint();
}

void MainWindow::checkUndoRoom() {
    if (!doc_.canUndo()) { undoWarned_ = false; return; } // 新開的圖、或 undo 到底：之後滿了再提示
    if (undoWarned_ || !doc_.nextCommitMayDropUndo()) return;
    undoWarned_ = true;
    showToast(tr(L"Undo history is almost full: one more edit may make the earliest step impossible to undo. You can change the limits in Edit > Settings > General."), false, kUndoToastMs);
}

void MainWindow::hideToast() {
    KillTimer(hwnd_, kToastTimer);
    if (toast_.empty()) return;
    toast_.clear();
    toastSticky_ = false;
    repaint();
}

void MainWindow::drawToast() {
    if (toast_.empty()) return;
    auto* rt = renderer_.rt();
    float size = px(15);
    float padX = px(20), padY = px(14);
    float maxW = std::max(px(120), (canvasRc_.right - canvasRc_.left) - px(64) - 2 * padX);
    float h = 0;
    float w = renderer_.measureUiText(toast_, size, true, maxW, &h);
    float cx = (canvasRc_.left + canvasRc_.right) / 2, cy = (canvasRc_.top + canvasRc_.bottom) / 2;
    D2D1_RECT_F box = {cx - w / 2 - padX, cy - h / 2 - padY, cx + w / 2 + padX, cy + h / 2 + padY};
    rt->FillRoundedRectangle(D2D1::RoundedRect(box, px(8), px(8)), renderer_.brush(D2D1::ColorF(0.12f, 0.12f, 0.12f, 0.88f)));
    D2D1_RECT_F textRc = {box.left + padX, box.top + padY, box.right - padX, box.bottom - padY};
    renderer_.drawUiText(toast_, textRc, size, D2D1::ColorF(D2D1::ColorF::White), DWRITE_TEXT_ALIGNMENT_CENTER, true,
                         DWRITE_PARAGRAPH_ALIGNMENT_CENTER, false, true);
}

// ---------------------------------------------------------------------------
// 兩頁並排

void MainWindow::updateSpreads() {
    spreadCache_.get(nav_.files(), settings_.view.firstPageAlone, shiftOne_, isWidePage);
}

int MainWindow::currentSpread() const { return spreadIndexOf(spreads(), nav_.index()); }

bool MainWindow::confirmSpreadMove() {
    int cur = currentSpread();
    if (cur < 0 || spreads()[static_cast<size_t>(cur)].first == nav_.index()) return true;
    return confirmLeaveImage();
}

void MainWindow::loadSecond() {
    int cur = currentSpread();
    std::wstring path = cur >= 0 ? nav_.at(spreads()[static_cast<size_t>(cur)].second) : L"";
    if (path == secondPath_ && (second_ || path.empty())) return;
    second_.reset();
    secondPath_ = path;
    if (path.empty()) return;
    LoadResult r = loadPage(path);
    if (r.image) second_ = r.image;
}

namespace {
PageSize pageSizeOf(const Image& img) { return {img.width, img.height}; }
} // namespace

float MainWindow::viewW() const {
    auto img = doc_.image();
    if (!img) return 0;
    PageSize second = second_ ? pageSizeOf(*second_) : PageSize{};
    return spreadSize(pageSizeOf(*img), twoPage() && second_ ? &second : nullptr).x;
}

float MainWindow::viewH() const {
    auto img = doc_.image();
    if (!img) return 0;
    PageSize second = second_ ? pageSizeOf(*second_) : PageSize{};
    return spreadSize(pageSizeOf(*img), twoPage() && second_ ? &second : nullptr).y;
}

Rc MainWindow::pageRect(bool second) const {
    auto img = doc_.image();
    if (!img) return {};
    PageSize other = second_ ? pageSizeOf(*second_) : PageSize{};
    const bool two = twoPage() && second_;
    // 單頁時不管問哪一頁都是整張圖。
    return pageRectInSpread(pageSizeOf(*img), two ? &other : nullptr, two && second, settings_.view.rightToLeft);
}

void MainWindow::updateTitle() {
    TitleInfo t;
    if (!path_.empty()) {
        t.name = displayName(path_);
        t.dirty = doc_.dirty();
        t.index = nav_.index();
        t.count = nav_.count();
        if (twoPage() && second_) {
            t.secondName = displayName(secondPath_);
            t.rightToLeft = settings_.view.rightToLeft;
            int cur = currentSpread();
            if (cur >= 0) {
                t.spreadFirst = spreads()[static_cast<size_t>(cur)].first;
                t.spreadSecond = spreads()[static_cast<size_t>(cur)].second;
            }
        }
    }
    // 目前的模式；有臨時調整（選單改過、和模式本體不同）就加 *
    t.profile = settings_.profileName;
    t.profileModified = profileModified(settings_);
    t.appName = kAppName;
    SetWindowTextW(hwnd_, composeTitle(t).c_str());
    checkUndoRoom();
}

// ---------------------------------------------------------------------------
// 檢視

ImagePtr MainWindow::currentImage() const { return editor_.displayImage(); }

POINT MainWindow::canvasCenter() const {
    return {static_cast<LONG>((canvasRc_.left + canvasRc_.right) / 2), static_cast<LONG>((canvasRc_.top + canvasRc_.bottom) / 2)};
}

bool MainWindow::setViewMode(ViewMode m) {
    if (m == ViewMode::TwoPage && doc_.image()) {
        // 目前這頁會變成某屏的第二頁時，要改開那屏的第一頁：先照換圖的規則問。
        updateSpreads();
        if (!confirmSpreadMove()) return false;
    }
    bool wasTwo = viewMode_ == ViewMode::TwoPage;
    viewMode_ = m;
    settings_.view.viewMode = m;
    saveSettingsQuiet();
    cropFit_ = false;
    if (m == ViewMode::TwoPage) {
        // 只看不編：收掉編輯狀態與標示工具列。
        endInteraction(true);
        closePopup();
        editor_.select(0);
        editor_.leaveFreehand();
        showMarkup_ = false;
        layout();
        if (doc_.image()) {
            updateSpreads();
            int cur = currentSpread();
            if (cur >= 0 && spreads()[static_cast<size_t>(cur)].first != nav_.index()) { openPath(nav_.at(spreads()[static_cast<size_t>(cur)].first)); return true; }
            loadSecond();
        }
    } else if (wasTwo) {
        second_.reset();
        secondPath_.clear();
    }
    fit_ = true;
    fitToView();
    positionEdit();
    updateTitle();
    repaint();
    schedulePrefetch();
    return true;
}

// 依檢視模式算 zoom。符合類模式預設不放大小圖，開了「小圖放大」才放大；1:1 永遠不放大。
void MainWindow::fitToView(bool resetOffset) {
    auto img = doc_.image();
    if (!img) return;
    float cw = canvasRc_.right - canvasRc_.left, ch = canvasRc_.bottom - canvasRc_.top;
    // 裁切模式剛進入時整張圖放進畫布（不改檢視模式設定）
    ViewMode mode = editor_.cropping() && cropFit_ ? ViewMode::FitWindow : viewMode_;
    zoom_ = fitZoom(mode, cw, ch, viewW(), viewH(), settings_.view.upscaleSmall);
    if (resetOffset) offset_ = {0, 0}; // 從左上開始，放得下的方向由 clampOffset 置中
    clampOffset();
    buildButtons();
}

void MainWindow::clampOffset() {
    if (!doc_.image()) return;
    offset_ = mp::clampOffset(offset_, zoom_, viewW(), viewH(), canvasRc_.right - canvasRc_.left, canvasRc_.bottom - canvasRc_.top);
}

bool MainWindow::scrollBy(float dx, float dy) {
    Pt before = offset_;
    offset_.x += dx;
    offset_.y += dy;
    clampOffset();
    bool moved = std::abs(offset_.x - before.x) > 0.5f || std::abs(offset_.y - before.y) > 0.5f;
    if (moved) { positionEdit(); repaint(); }
    return moved;
}

// 上下鍵：每次捲畫布高度的 1/4。開了「捲到底翻頁」時，到底再按下翻下一張、到頂再按上翻上一張。
void MainWindow::scrollKey(int vk) {
    if (!doc_.image()) return;
    float step = (canvasRc_.bottom - canvasRc_.top) / 4;
    bool down = vk == VK_DOWN;
    if (scrollBy(0, down ? -step : step)) return;
    if (!settings_.view.scrollFlip) return;
    if (down) goNext();
    else this->step(-1, true); // 翻到上一張，開在底部（step 這個名字在這裡是區域變數）
}

void MainWindow::setZoom(float z, POINT anchor) {
    if (!doc_.image()) return;
    const Viewport v = zoomAbout(viewport(), z, {anchor.x - canvasRc_.left, anchor.y - canvasRc_.top});
    zoom_ = v.zoom;
    offset_ = v.offset;
    fit_ = false;
    cropFit_ = false;
    clampOffset();
    buildButtons();
    positionEdit();
    // 連續縮放時先用快的畫，停下來再算高品質。
    if (settings_.view.highQuality) { hqBusy_ = true; SetTimer(hwnd_, kHqTimer, kHqDelayMs, nullptr); }
    repaint();
}

void MainWindow::zoomStep(int dir) {
    float f = dir > 0 ? 1.25f : 1.0f / 1.25f;
    setZoom(zoom_ * f, canvasCenter());
}

Pt MainWindow::toImage(POINT c) const { return canvasToImage(viewport(), {c.x - canvasRc_.left, c.y - canvasRc_.top}); }

POINT MainWindow::toClient(Pt p) const {
    const Pt c = imageToCanvas(viewport(), p);
    return {static_cast<LONG>(std::lround(canvasRc_.left + c.x)), static_cast<LONG>(std::lround(canvasRc_.top + c.y))};
}

D2D1_MATRIX_3X2_F MainWindow::viewMatrix() const {
    return D2D1::Matrix3x2F::Scale(zoom_, zoom_) *
           D2D1::Matrix3x2F::Translation(canvasRc_.left + offset_.x, canvasRc_.top + offset_.y);
}

float MainWindow::imageTol() const { return px(4) / zoom_; }

// ---------------------------------------------------------------------------
// 繪製

void MainWindow::paint() {
    if (!renderer_.ensureHwndTarget(hwnd_)) return;
    auto* rt = renderer_.rt();
    rt->BeginDraw();
    rt->SetTransform(D2D1::Matrix3x2F::Identity());
    rt->Clear(kChromeBg);
    drawCanvas();
    drawChrome();
    drawToast();
    if (showInfo_) {
        if (auto img = doc_.image()) {
            infoData_.width = img->width;
            infoData_.height = img->height;
            infoData_.dpiX = doc_.dpi().x;
            infoData_.dpiY = doc_.dpi().y;
        }
        drawInfoPanel(renderer_.surface(), infoRc_, infoData_, infoState_, ui_);
    }
    HRESULT hr = rt->EndDraw();
    renderer_.endFrame(); // 這一幀沒畫到的點陣圖（翻走的圖、undo 歷史、舊預覽）連同來源圖一起放掉
    if (hr == D2DERR_RECREATE_TARGET) {
        renderer_.discardTarget();
        repaint();
    }
}

void MainWindow::drawChrome() {
    auto* rt = renderer_.rt();
    rt->SetTransform(D2D1::Matrix3x2F::Identity());
    rt->FillRectangle(toolbarRc_, renderer_.brush(kChromeBg));
    rt->DrawLine(D2D1::Point2F(0, toolbarRc_.bottom - 0.5f), D2D1::Point2F(toolbarRc_.right, toolbarRc_.bottom - 0.5f),
                 renderer_.brush(kChromeLine), 1.0f);
    if (showMarkup_) {
        rt->FillRectangle(markupRc_, renderer_.brush(D2D1::ColorF(0.93f, 0.93f, 0.93f)));
        rt->DrawLine(D2D1::Point2F(0, markupRc_.bottom - 0.5f), D2D1::Point2F(markupRc_.right, markupRc_.bottom - 0.5f),
                     renderer_.brush(kChromeLine), 1.0f);
    }
    rt->FillRectangle(nameRc_, renderer_.brush(kChromeBg));
    rt->DrawLine(D2D1::Point2F(0, nameRc_.bottom - 0.5f), D2D1::Point2F(nameRc_.right, nameRc_.bottom - 0.5f),
                 renderer_.brush(kChromeLine), 1.0f);
    std::wstring name = path_.empty() ? trs(L"No file open — drop an image here, or use File > Open") : displayName(path_);
    if (twoPage() && second_) {
        std::wstring a = displayName(path_), b = displayName(secondPath_);
        name = settings_.view.rightToLeft ? b + L"   |   " + a : a + L"   |   " + b;
    }
    renderer_.drawUiText(name, nameRc_, px(13), kIcon, DWRITE_TEXT_ALIGNMENT_CENTER);
    for (const auto& b : buttons_) drawButton(b);
}

void MainWindow::drawButton(const Button& b) {
    auto* rt = renderer_.rt();
    float r = px(5);
    if (b.id == BTN_CROP_APPLY) {
        // 文字按鈕：灰底圓角，像 Preview 的「裁切」
        bool hot = hoverBtn_ == b.id || pressBtn_ == b.id;
        rt->FillRoundedRectangle(D2D1::RoundedRect(b.rect, px(6), px(6)),
                                 renderer_.brush(hot ? D2D1::ColorF(0.78f, 0.78f, 0.78f) : D2D1::ColorF(0.85f, 0.85f, 0.85f)));
        renderer_.drawUiText(tr(L"Crop"), b.rect, px(13), kIcon, DWRITE_TEXT_ALIGNMENT_CENTER);
        return;
    }
    if (b.pressed) rt->FillRoundedRectangle(D2D1::RoundedRect(b.rect, r, r), renderer_.brush(kBtnOn));
    else if (b.enabled && (hoverBtn_ == b.id || pressBtn_ == b.id))
        rt->FillRoundedRectangle(D2D1::RoundedRect(b.rect, r, r), renderer_.brush(kBtnHover));
    drawIcon(b);
    if (b.dropdown) {
        float x = b.rect.right - px(7), y = b.rect.bottom - px(7);
        D2D1_COLOR_F c = b.enabled ? kIcon : kIconDisabled;
        rt->DrawLine(D2D1::Point2F(x - px(3), y - px(1.5f)), D2D1::Point2F(x, y + px(1.5f)), renderer_.brush(c), px(1.2f));
        rt->DrawLine(D2D1::Point2F(x, y + px(1.5f)), D2D1::Point2F(x + px(3), y - px(1.5f)), renderer_.brush(c), px(1.2f));
    }
}

void MainWindow::drawIcon(const Button& b) {
    auto* rt = renderer_.rt();
    D2D1_COLOR_F c = b.enabled ? kIcon : kIconDisabled;
    auto* br = renderer_.brush(c);
    float cx = (b.rect.left + b.rect.right) / 2, cy = (b.rect.top + b.rect.bottom) / 2;
    float s = px(1.6f);
    auto P = [](float x, float y) { return D2D1::Point2F(x, y); };
    switch (b.id) {
    case BTN_ZOOMOUT:
    case BTN_ZOOMIN: {
        float r = px(6);
        rt->DrawEllipse(D2D1::Ellipse(P(cx - px(1.5f), cy - px(1.5f)), r, r), br, s);
        rt->DrawLine(P(cx + px(3), cy + px(3)), P(cx + px(8), cy + px(8)), br, s * 1.4f);
        rt->DrawLine(P(cx - px(5), cy - px(1.5f)), P(cx + px(2), cy - px(1.5f)), br, s);
        if (b.id == BTN_ZOOMIN) rt->DrawLine(P(cx - px(1.5f), cy - px(5)), P(cx - px(1.5f), cy + px(2)), br, s);
        break;
    }
    case BTN_ZOOM100:
        renderer_.drawUiText(L"1:1", b.rect, px(12), c, DWRITE_TEXT_ALIGNMENT_CENTER, true);
        break;
    case BTN_ROTATE: {
        // 逆時針的弧加箭頭
        float r = px(7);
        auto geo = renderer_.device().pathGeometry([&](ID2D1GeometrySink& sink) {
            sink.BeginFigure(P(cx + r * 0.0f, cy - r), D2D1_FIGURE_BEGIN_HOLLOW);
            sink.AddArc(D2D1::ArcSegment(P(cx - r * 0.7f, cy + r * 0.7f), D2D1::SizeF(r, r), 0, D2D1_SWEEP_DIRECTION_COUNTER_CLOCKWISE, D2D1_ARC_SIZE_LARGE));
            sink.EndFigure(D2D1_FIGURE_END_OPEN);
        });
        if (geo) rt->DrawGeometry(geo.Get(), br, s);
        rt->DrawLine(P(cx, cy - r), P(cx - px(4), cy - r - px(3)), br, s);
        rt->DrawLine(P(cx, cy - r), P(cx - px(4), cy - r + px(3)), br, s);
        break;
    }
    case BTN_MARKUP: {
        // 筆頭
        auto geo = renderer_.device().pathGeometry([&](ID2D1GeometrySink& sink) {
            sink.BeginFigure(P(cx + px(6), cy - px(7)), D2D1_FIGURE_BEGIN_FILLED);
            sink.AddLine(P(cx + px(8), cy - px(5)));
            sink.AddLine(P(cx - px(4), cy + px(7)));
            sink.AddLine(P(cx - px(8), cy + px(8)));
            sink.AddLine(P(cx - px(7), cy + px(4)));
            sink.EndFigure(D2D1_FIGURE_END_CLOSED);
        });
        if (geo) rt->DrawGeometry(geo.Get(), br, s);
        break;
    }
    case BTN_INFO: {
        float r = px(8);
        rt->DrawEllipse(D2D1::Ellipse(P(cx, cy), r, r), br, s);
        rt->DrawLine(P(cx, cy - px(1)), P(cx, cy + px(5)), br, s * 1.3f);
        rt->FillEllipse(D2D1::Ellipse(P(cx, cy - px(4.5f)), s * 0.8f, s * 0.8f), br);
        break;
    }
    case BTN_FREEHAND: {
        auto geo = renderer_.device().pathGeometry([&](ID2D1GeometrySink& sink) {
            sink.BeginFigure(P(cx - px(9), cy + px(5)), D2D1_FIGURE_BEGIN_HOLLOW);
            sink.AddBezier(D2D1::BezierSegment(P(cx - px(6), cy - px(12)), P(cx - px(2), cy + px(12)), P(cx + px(1), cy - px(2))));
            sink.AddBezier(D2D1::BezierSegment(P(cx + px(3), cy - px(10)), P(cx + px(5), cy + px(8)), P(cx + px(9), cy - px(4))));
            sink.EndFigure(D2D1_FIGURE_END_OPEN);
        });
        if (geo) rt->DrawGeometry(geo.Get(), br, s);
        break;
    }
    case BTN_SHAPES:
        rt->DrawRectangle(D2D1::RectF(cx - px(9), cy - px(7), cx - px(1), cy + px(3)), br, s);
        rt->DrawEllipse(D2D1::Ellipse(P(cx + px(4), cy + px(1)), px(5), px(5)), br, s);
        break;
    case BTN_TEXT:
        renderer_.drawUiText(L"A", b.rect, px(18), c, DWRITE_TEXT_ALIGNMENT_CENTER, false);
        break;
    case BTN_ADJCOLOR: {
        for (int i = 0; i < 3; ++i) {
            float y = cy - px(6) + i * px(6);
            rt->DrawLine(P(cx - px(9), y), P(cx + px(9), y), br, s);
            float kx = cx + (i == 0 ? -px(3) : i == 1 ? px(4) : -px(6));
            rt->FillEllipse(D2D1::Ellipse(P(kx, y), px(2.2f), px(2.2f)), br);
        }
        break;
    }
    case BTN_CROP: {
        // 兩個交錯的 L
        auto geo = renderer_.device().pathGeometry([&](ID2D1GeometrySink& sink) {
            sink.BeginFigure(P(cx - px(5), cy - px(9)), D2D1_FIGURE_BEGIN_HOLLOW);
            sink.AddLine(P(cx - px(5), cy + px(5)));
            sink.AddLine(P(cx + px(9), cy + px(5)));
            sink.EndFigure(D2D1_FIGURE_END_OPEN);
            sink.BeginFigure(P(cx - px(9), cy - px(5)), D2D1_FIGURE_BEGIN_HOLLOW);
            sink.AddLine(P(cx + px(5), cy - px(5)));
            sink.AddLine(P(cx + px(5), cy + px(9)));
            sink.EndFigure(D2D1_FIGURE_END_OPEN);
        });
        if (geo) rt->DrawGeometry(geo.Get(), br, s);
        break;
    }
    case BTN_ADJSIZE:
        rt->DrawRectangle(D2D1::RectF(cx - px(9), cy - px(7), cx + px(9), cy + px(7)), br, s);
        rt->DrawLine(P(cx - px(5), cy + px(3)), P(cx + px(5), cy - px(3)), br, s);
        rt->DrawLine(P(cx + px(5), cy - px(3)), P(cx + px(1), cy - px(3)), br, s);
        rt->DrawLine(P(cx + px(5), cy - px(3)), P(cx + px(5), cy + px(1)), br, s);
        break;
    case BTN_LINESTYLE:
        for (int i = 0; i < 3; ++i) {
            float y = cy - px(6) + i * px(6);
            rt->DrawLine(P(cx - px(9), y), P(cx + px(7), y), br, px(1 + i * 1.5f));
        }
        break;
    case BTN_LINECOLOR: {
        float r = px(7);
        Color sc = editor_.currentStroke().stroke;
        if (sc.none()) {
            rt->DrawEllipse(D2D1::Ellipse(P(cx - px(1), cy), r, r), br, s);
            rt->DrawLine(P(cx - px(1) - r * 0.7f, cy + r * 0.7f), P(cx - px(1) + r * 0.7f, cy - r * 0.7f), renderer_.brush(D2D1::ColorF(0.85f, 0.1f, 0.1f)), s);
        } else {
            rt->DrawEllipse(D2D1::Ellipse(P(cx - px(1), cy), r, r), renderer_.brush(sc), px(3.5f));
        }
        break;
    }
    case BTN_FILL: {
        D2D1_RECT_F box = D2D1::RectF(cx - px(8), cy - px(7), cx + px(6), cy + px(7));
        Color fc = editor_.currentStroke().fill;
        if (fc.none()) {
            rt->DrawRectangle(box, br, s);
            rt->DrawLine(P(box.left, box.bottom), P(box.right, box.top), renderer_.brush(D2D1::ColorF(0.85f, 0.1f, 0.1f)), s);
        } else {
            rt->FillRectangle(box, renderer_.brush(fc));
            rt->DrawRectangle(box, br, px(1));
        }
        break;
    }
    case BTN_FORMAT:
        renderer_.drawUiText(L"Aa", D2D1::RectF(b.rect.left, b.rect.top, b.rect.right - px(5), b.rect.bottom), px(14), c,
                             DWRITE_TEXT_ALIGNMENT_CENTER, true);
        break;
    default: break;
    }
}

void MainWindow::drawCanvas() {
    auto* rt = renderer_.rt();
    rt->SetTransform(D2D1::Matrix3x2F::Identity());
    rt->FillRectangle(canvasRc_, renderer_.brush(kCanvasBg));
    auto img = currentImage();
    if (!img) {
        if (!loadError_.empty())
            renderer_.drawUiText(loadError_, canvasRc_, px(14), kIcon, DWRITE_TEXT_ALIGNMENT_CENTER);
        return;
    }
    rt->PushAxisAlignedClip(canvasRc_, D2D1_ANTIALIAS_MODE_ALIASED);
    // 高品質縮放：不在連續縮放中、不在調色預覽中、倍率不是 1 時，用 Lanczos3 先縮好的點陣圖。
    auto bitmapFor = [&](const ImagePtr& im) {
        const ImageBitmap* b = nullptr;
        int tw = 0, th = 0;
        if (settings_.view.highQuality && !hqBusy_ && !editor_.adjusting() &&
            hqTargetSize(im->width, im->height, zoom_, renderer_.rt()->GetMaximumBitmapSize(), tw, th))
            b = renderer_.scaledBitmap(im, tw, th, prefetch_.scaled(im, tw, th)); // 預先解碼時背景縮好的就直接用
        if (!b) b = renderer_.imageBitmap(im);
        return b;
    };
    auto shadow = [&](const Rc& r) {
        rt->SetTransform(D2D1::Matrix3x2F::Identity());
        POINT tl = toClient({r.left, r.top}), brp = toClient({r.right, r.bottom});
        rt->DrawRectangle(D2D1::RectF(tl.x - 0.5f, tl.y - 0.5f, brp.x + 0.5f, brp.y + 0.5f), renderer_.brush(D2D1::ColorF(0, 0, 0, 0.25f)), 1.0f);
    };
    PaintCtx ctx = renderer_.paintCtx(img, bitmapFor(img));
    // 兩頁並排：第二頁先畫（它不帶標註），再畫第一頁與它的標註。第一頁的位置由 pageRect 決定。
    Rc r1 = pageRect(false);
    if (twoPage() && second_) {
        Rc r2 = pageRect(true);
        shadow(r2);
        rt->SetTransform(D2D1::Matrix3x2F::Translation(r2.left, r2.top) * viewMatrix());
        PaintCtx ctx2 = renderer_.paintCtx(second_, bitmapFor(second_));
        ctx2.mosaic = nullptr; // 第二頁不帶標註
        drawImage(ctx2);
    }
    shadow(r1);
    rt->SetTransform(D2D1::Matrix3x2F::Translation(r1.left, r1.top) * viewMatrix());
    drawImage(ctx);
    for (const auto& a : editor_.visibleItems()) {
        if (a.id == editor_.editingId()) {
            Annotation copy = a;
            copy.textValue.clear();
            drawAnnotation(ctx, copy);
        } else {
            drawAnnotation(ctx, a);
        }
    }
    if (editor_.drag() == DragKind::Draw && !editor_.drawPath().empty())
        drawAnnotation(ctx, makeFreehand(editor_.drawPath(), editor_.stroke()));
    rt->SetTransform(D2D1::Matrix3x2F::Identity());
    drawSelection();
    drawCrop();
    rt->PopAxisAlignedClip();
}

void MainWindow::drawSelection() {
    const Annotation* a = editor_.selectedItem();
    if (!a || editor_.drag() == DragKind::Draw) return;
    auto* rt = renderer_.rt();
    float r = px(kHandleR);
    if (editor_.drag() == DragKind::Handle) {
        // 綠點按下時畫它的可動軌跡：繞圈的畫圈，直線的畫線。
        HandleGuide g = handleGuide(*a, editor_.dragHandleId());
        auto* gb = renderer_.brush(D2D1::ColorF(0.04f, 0.52f, 1.0f, 0.40f));
        if (g.kind == GuideKind::Circle) {
            POINT tl = toClient({g.ellipse.left, g.ellipse.top}), br = toClient({g.ellipse.right, g.ellipse.bottom});
            D2D1_ELLIPSE e = D2D1::Ellipse(D2D1::Point2F((tl.x + br.x) * 0.5f, (tl.y + br.y) * 0.5f),
                                           (br.x - tl.x) * 0.5f, (br.y - tl.y) * 0.5f);
            rt->DrawEllipse(e, gb, px(2.0f));
        } else if (g.kind == GuideKind::Line) {
            POINT p0 = toClient(g.a), p1 = toClient(g.b);
            rt->DrawLine(D2D1::Point2F(static_cast<float>(p0.x), static_cast<float>(p0.y)),
                         D2D1::Point2F(static_cast<float>(p1.x), static_cast<float>(p1.y)), gb, px(2.0f));
        }
    }
    for (const auto& h : handles(*a)) {
        POINT c = toClient(h.pos);
        D2D1_ELLIPSE e = D2D1::Ellipse(D2D1::Point2F(static_cast<float>(c.x), static_cast<float>(c.y)), r, r);
        rt->FillEllipse(e, renderer_.brush(h.kind == HandleKind::Blue ? kHandleBlue : kHandleGreen));
        rt->DrawEllipse(e, renderer_.brush(D2D1::ColorF(1, 1, 1)), px(1.5f));
    }
}

// ---------------------------------------------------------------------------
// 按鈕動作

void MainWindow::onButton(int id) {
    // 彈出選單開著時再點同一顆按鈕：選單已因失去焦點關閉，這一下視為「收起」，不要再開。
    if (id == popupBtn_ && GetTickCount64() - lastPopupCloseTick() < 300) { popupBtn_ = BTN_NONE; return; }
    auto anchor = [&]() { popupBtn_ = id; return buttonScreenRect(id); };
    // 裁切模式中按了縮放、資訊、裁切以外的按鈕：先取消裁切，再做那顆按鈕的事。
    if (editor_.cropping() && id != BTN_CROP && id != BTN_CROP_APPLY && id != BTN_ZOOMOUT && id != BTN_ZOOM100 && id != BTN_ZOOMIN &&
        id != BTN_INFO)
        cancelCrop();
    switch (id) {
    case BTN_ZOOMOUT: zoomStep(-1); break;
    case BTN_ZOOM100: setViewMode(ViewMode::Actual); break;
    case BTN_ZOOMIN: zoomStep(1); break;
    case BTN_ROTATE: rotateImage(); break;
    case BTN_MARKUP:
        if (!editGuard()) break;
        showMarkup_ = !showMarkup_;
        if (!showMarkup_) editor_.leaveFreehand();
        layout();
        if (fit_) fitToView(); else clampOffset();
        positionEdit();
        repaint();
        break;
    case BTN_INFO:
        showInfo_ = !showInfo_;
        layout();
        if (fit_) fitToView(); else clampOffset();
        positionEdit();
        repaint();
        break;
    case BTN_FREEHAND:
        if (!editGuard()) break;
        endTextEdit(true);
        editor_.toggleFreehand();
        buildButtons();
        repaint();
        break;
    case BTN_SHAPES:
        showShapesPopup(hwnd_, renderer_.device(), anchor(), ui_, [this](Kind k) { addShape(k); });
        break;
    case BTN_TEXT: addText(); break;
    case BTN_ADJCOLOR:
        if (!editGuard()) break;
        if (!editor_.adjusting() && doc_.image()) {
            endTextEdit(true);
            editor_.beginAdjust();
            buildButtons();
            showColorAdjustPanel(hwnd_, this);
            repaint();
        }
        break;
    case BTN_ADJSIZE: adjustSize(); break;
    case BTN_CROP:
        if (editor_.cropping()) cancelCrop();
        else beginCrop();
        break;
    case BTN_CROP_APPLY: applyCrop(); break;
    case BTN_LINESTYLE:
        showLineStylePopup(hwnd_, renderer_.device(), anchor(), ui_, editor_.currentStroke(), [this](const StrokeStyle& s) { applyStrokeStyle(s); });
        break;
    case BTN_LINECOLOR:
        showColorPopup(hwnd_, renderer_.device(), anchor(), ui_, editor_.currentStroke().stroke, true, [this](Color c) {
            StrokeStyle s = editor_.currentStroke();
            s.stroke = c;
            applyStrokeStyle(s);
        });
        break;
    case BTN_FILL:
        showColorPopup(hwnd_, renderer_.device(), anchor(), ui_, editor_.currentStroke().fill, true, [this](Color c) {
            StrokeStyle s = editor_.currentStroke();
            s.fill = c;
            applyStrokeStyle(s);
        });
        break;
    case BTN_FORMAT:
        showFormatPopup(hwnd_, renderer_.device(), anchor(), ui_, editor_.currentText(), [this](const TextStyle& t) { applyTextStyle(t); });
        break;
    default: break;
    }
}

// ---------------------------------------------------------------------------
// 滑鼠

Pointer MainWindow::pointerAt(POINT client) const {
    Pointer p;
    p.pos = toImage(client);
    p.tol = imageTol();
    p.handleTol = px(kHandleR + kHandleHitExtra) / zoom_;
    p.cropTol = px(kCropTol) / zoom_;
    p.cropCorner = px(kCropCorner) / zoom_;
    p.drawStep = kDrawStepPx / zoom_;
    return p;
}

void MainWindow::onLButtonDown(POINT pt) {
    const Button* b = nullptr;
    int id = hitButton(pt, &b);
    if (id != BTN_NONE) {
        // 不搶焦點：編輯文字時可以直接點格式、顏色按鈕。
        hideTip();
        KillTimer(hwnd_, kBtnTipTimer);
        if (b->enabled) { pressBtn_ = id; repaint(); }
        return;
    }
    SetFocus(hwnd_);
    hideTip();
    KillTimer(hwnd_, kTipTimer);
    if (showInfo_ && inRect(infoRc_, pt)) {
        if (infoPanelClick(infoRc_, pt, infoState_)) repaint();
        return;
    }
    if (!inRect(canvasRc_, pt) || !doc_.image()) return;
    if (edit_) endTextEdit(true);

    // 這一下做什麼（平移、取白點、拖裁切框、畫塗鴉、選物件）由 Editor 依目前的狀態決定。
    dragStartClient_ = pt;
    if (editor_.press(pointerAt(pt), twoPage()) == Editor::Press::PickedWhite) {
        colorAdjustPanelSetParams(editor_.adjustParams());
        repaint();
        return;
    }
    if (editor_.drag() == DragKind::Pan) dragOffsetStart_ = offset_;
    SetCapture(hwnd_);
    buildButtons(); // 選取可能變了
    repaint();
}

void MainWindow::onLButtonDblClk(POINT pt) {
    if (!inRect(canvasRc_, pt) || !doc_.image() || editor_.freehand() || twoPage() || editor_.cropping()) { onLButtonDown(pt); return; }
    if (uint32_t id = editor_.textItemAt(pointerAt(pt))) {
        select(id);
        beginTextEdit(id, false);
        return;
    }
    onLButtonDown(pt);
}

void MainWindow::onMouseMove(POINT pt, WPARAM) {
    if (!trackingMouse_) {
        TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, hwnd_, 0};
        TrackMouseEvent(&tme);
        trackingMouse_ = true;
    }
    if (!editor_.dragging()) {
        int id = hitButton(pt);
        if (id != hoverBtn_) {
            hoverBtn_ = id;
            repaint();
            // 按鈕名稱 tooltip：停 0.5 秒出現，每次 hover 都顯示，不計次。
            hideTip();
            if (id != BTN_NONE) SetTimer(hwnd_, kBtnTipTimer, kTipDelayMs, nullptr);
            else KillTimer(hwnd_, kBtnTipTimer);
        }
        // 教學提示：游標在物件方框裡停住半秒才出現，離開就收。
        uint32_t item = inRect(canvasRc_, pt) && doc_.image() ? editor_.tipItemAt(toImage(pt), twoPage()) : 0;
        if (item != hoverItem_) {
            hoverItem_ = item;
            hideTip();
        }
        if (item && !tipVisible_ && item != tipShownFor_ && settings_.tipShown < kTipMaxShown) SetTimer(hwnd_, kTipTimer, kTipDelayMs, nullptr);
        else KillTimer(hwnd_, kTipTimer);
        return;
    }
    if (editor_.drag() == DragKind::Pan) {
        offset_ = dragOffsetStart_ + Pt{static_cast<float>(pt.x - dragStartClient_.x), static_cast<float>(pt.y - dragStartClient_.y)};
        clampOffset();
        positionEdit();
    } else {
        const bool pastClickSlop = std::abs(pt.x - dragStartClient_.x) > kClickSlopPx || std::abs(pt.y - dragStartClient_.y) > kClickSlopPx;
        editor_.move(pointerAt(pt), pastClickSlop);
    }
    repaint();
}

void MainWindow::onLButtonUp(POINT pt) {
    if (pressBtn_ != BTN_NONE) {
        int id = pressBtn_;
        pressBtn_ = BTN_NONE;
        repaint();
        if (hitButton(pt) == id) onButton(id);
        return;
    }
    endDrag(true);
}

void MainWindow::endDrag(bool apply) {
    if (!editor_.dragging()) return;
    if (apply) {
        if (editor_.release()) updateTitle();
    } else {
        editor_.cancelDrag();
    }
    repaint();
    // 最後才放掉滑鼠：ReleaseCapture 會立刻送 WM_CAPTURECHANGED 進來，那時已經不在拖曳中。
    if (GetCapture() == hwnd_) ReleaseCapture();
}

void MainWindow::endInteraction(bool commitText) {
    endDrag(false);
    endTextEdit(commitText);
    cancelCrop();
    if (editor_.adjusting()) { closeColorAdjustPanel(); cancelAdjust(); }
}

void MainWindow::onMouseWheel(POINT ptScreen, int delta, WPARAM keys) {
    POINT pt = ptScreen;
    ScreenToClient(hwnd_, &pt);
    if (showInfo_ && inRect(infoRc_, pt)) {
        if (infoPanelScroll(infoRc_, -delta * px(0.5f), infoState_)) repaint();
        return;
    }
    if (!inRect(canvasRc_, pt) || !doc_.image()) return;
    if (keys & MK_CONTROL) {
        setZoom(zoom_ * std::pow(1.1f, delta / 120.0f), pt);
        return;
    }
    if (keys & MK_SHIFT) offset_.x += delta;
    else offset_.y += delta;
    clampOffset();
    positionEdit();
    repaint();
}

bool MainWindow::onSetCursor() {
    POINT pt;
    GetCursorPos(&pt);
    ScreenToClient(hwnd_, &pt);
    if (!inRect(canvasRc_, pt) || !doc_.image()) return false;
    LPCWSTR id = nullptr;
    switch (editor_.cursorAt(pointerAt(pt), twoPage())) {
    case CursorKind::Default: return false;
    case CursorKind::Cross: id = IDC_CROSS; break;
    case CursorKind::Move: id = IDC_SIZEALL; break;
    case CursorKind::SizeNWSE: id = IDC_SIZENWSE; break;
    case CursorKind::SizeNESW: id = IDC_SIZENESW; break;
    case CursorKind::SizeWE: id = IDC_SIZEWE; break;
    case CursorKind::SizeNS: id = IDC_SIZENS; break;
    case CursorKind::Hand: id = IDC_HAND; break;
    }
    SetCursor(LoadCursorW(nullptr, id));
    return true;
}

uint32_t MainWindow::hitItemBox(POINT pt) const {
    if (!inRect(canvasRc_, pt) || !doc_.image()) return 0;
    return editor_.itemBoxAt(toImage(pt));
}

// 共用一個 TTF_TRACK 的 tooltip 視窗：物件教學提示和按鈕名稱都用它。
void MainWindow::showTipAt(const wchar_t* text, POINT screenPt) {
    if (!tip_) {
        tip_ = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, nullptr, WS_POPUP | TTS_NOPREFIX | TTS_ALWAYSTIP, 0, 0, 0, 0,
                               hwnd_, nullptr, inst_, nullptr);
        if (!tip_) return;
        tipInfo_ = {};
        tipInfo_.cbSize = sizeof(tipInfo_);
        tipInfo_.uFlags = TTF_TRACK | TTF_ABSOLUTE | TTF_IDISHWND;
        tipInfo_.hwnd = hwnd_;
        tipInfo_.uId = reinterpret_cast<UINT_PTR>(hwnd_);
        tipInfo_.lpszText = const_cast<wchar_t*>(text);
        SendMessageW(tip_, TTM_ADDTOOLW, 0, reinterpret_cast<LPARAM>(&tipInfo_));
        SendMessageW(tip_, TTM_SETMAXTIPWIDTH, 0, static_cast<LPARAM>(px(420)));
    } else {
        tipInfo_.lpszText = const_cast<wchar_t*>(text);
        SendMessageW(tip_, TTM_UPDATETIPTEXTW, 0, reinterpret_cast<LPARAM>(&tipInfo_));
    }
    SendMessageW(tip_, TTM_TRACKPOSITION, 0, MAKELPARAM(screenPt.x, screenPt.y));
    SendMessageW(tip_, TTM_TRACKACTIVATE, TRUE, reinterpret_cast<LPARAM>(&tipInfo_));
    // TTF_ABSOLUTE 不會自己避開螢幕邊緣：靠右的按鈕（標示工具列、資訊面板）提示會被切掉。
    // 顯示後量實際視窗大小，超出工作區就夾回來再放一次。
    RECT tr{};
    MONITORINFO mi{sizeof(mi)};
    if (GetWindowRect(tip_, &tr) && GetMonitorInfoW(MonitorFromPoint(screenPt, MONITOR_DEFAULTTONEAREST), &mi)) {
        LONG tw = tr.right - tr.left, th = tr.bottom - tr.top;
        LONG x = std::clamp<LONG>(screenPt.x, mi.rcWork.left, std::max<LONG>(mi.rcWork.left, mi.rcWork.right - tw));
        LONG y = std::clamp<LONG>(screenPt.y, mi.rcWork.top, std::max<LONG>(mi.rcWork.top, mi.rcWork.bottom - th));
        if (x != screenPt.x || y != screenPt.y) SendMessageW(tip_, TTM_TRACKPOSITION, 0, MAKELPARAM(x, y));
    }
    tipVisible_ = true;
}

void MainWindow::showTip(POINT pt) {
    POINT sp = pt;
    ClientToScreen(hwnd_, &sp);
    showTipAt(tr(L"Drag the blue dots to resize, drag the object to move, Delete to remove"),
              {sp.x + static_cast<int>(px(16)), sp.y + static_cast<int>(px(22))});
    if (!tipVisible_) return;
    tipShownFor_ = hoverItem_; // 同一個物件只提示一次，拖完控制點不再跳
    settings_.tipShown++;
    saveSettingsQuiet();
    SetTimer(hwnd_, kTipHideTimer, kTipShowMs, nullptr);
}

const wchar_t* MainWindow::buttonName(int id) {
    switch (id) {
    case BTN_ZOOMOUT: return tr(L"Zoom Out");
    case BTN_ZOOM100: return tr(L"Actual Size");
    case BTN_ZOOMIN: return tr(L"Zoom In");
    case BTN_ROTATE: return tr(L"Rotate 90° Counterclockwise");
    case BTN_MARKUP: return tr(L"Markup Toolbar");
    case BTN_INFO: return tr(L"Info Panel");
    case BTN_FREEHAND: return tr(L"Sketch");
    case BTN_SHAPES: return tr(L"Shapes");
    case BTN_TEXT: return tr(L"Text");
    case BTN_ADJCOLOR: return tr(L"Adjust Color");
    case BTN_ADJSIZE: return tr(L"Adjust Size");
    case BTN_LINESTYLE: return tr(L"Line Style");
    case BTN_LINECOLOR: return tr(L"Line Color");
    case BTN_FILL: return tr(L"Fill Color");
    case BTN_FORMAT: return tr(L"Text Format");
    case BTN_CROP: return tr(L"Crop");
    default: return L"";
    }
}

void MainWindow::showButtonTip(int id) {
    const Button* b = nullptr;
    for (const auto& x : buttons_) if (x.id == id) b = &x;
    if (!b || !*buttonName(id)) return; // 文字按鈕本身就寫了名字，不另外提示
    // 放在按鈕正下方，不跟著游標跑。
    POINT sp{static_cast<LONG>(b->rect.left), static_cast<LONG>(b->rect.bottom + px(4))};
    ClientToScreen(hwnd_, &sp);
    showTipAt(buttonName(id), sp);
}

void MainWindow::hideTip() {
    KillTimer(hwnd_, kTipHideTimer);
    if (!tip_ || !tipVisible_) return;
    SendMessageW(tip_, TTM_TRACKACTIVATE, FALSE, reinterpret_cast<LPARAM>(&tipInfo_));
    tipVisible_ = false;
}

// ---------------------------------------------------------------------------
// 設定與 shell 登錄

void MainWindow::loadSettingsAndRegister() {
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    exePath_ = exe;
    settingsPath_ = settingsPathForExe(exePath_);
    SettingsLoad loaded = loadSettings(settingsPath_, settings_);
    // 設定檔在但讀不到（別的程式正獨佔它）：多半很快就放開，等一下再試。
    for (int i = 0; i < kSettingsReadRetries && loaded == SettingsLoad::Unreadable; ++i) {
        Sleep(kSettingsReadRetryMs);
        loaded = loadSettings(settingsPath_, settings_);
    }
    settingsUnreadable_ = loaded == SettingsLoad::Unreadable;
    settingsBase_ = settings_; // 這個視窗和檔案同步時的樣子：存檔時用它分辨哪些欄位是自己改的
    nav_.setPinnedNames(settings_.pinnedNames);
    doc_.setUndoLimit(settings_.undoLimit);
    doc_.setUndoMemoryLimitMB(settings_.undoMemoryMB);
    settingsWritable_ = isPathWritable(settingsPath_);
    viewMode_ = settings_.view.viewMode;
    initLanguage(); // 要在登錄之前：右鍵選單的文字跟語系走
    // 模式清單：第一次執行（或舊版 ini）補內建的兩個。名字要翻譯，所以在 initLanguage 之後。
    if (ensureProfiles(settings_, tr(L"General mode"), tr(L"Comic mode"))) saveSettingsQuiet();
    // portable：exe 搬了位置就把登錄改成現在的路徑。
    // 換了語言（選單文字不同）、新版多了副檔名，也要重新登錄。
    const ShellRegOptions reg = shellRegOptions();
    if (settings_.contextMenu && !contextMenuUpToDate(exePath_, reg)) registerContextMenu(exePath_, reg);
    if (!progIdExePath(reg).empty() && !progIdUpToDate(exePath_, reg)) registerProgId(exePath_, reg);
}

void MainWindow::saveSettingsQuiet() {
    // 啟動時設定檔讀不到：手上是預設值，存了會把原本的設定（密碼列表、模式…）蓋掉，所以這次執行都不存。
    if (settingsUnreadable_) { settingsWritable_ = false; return; }
    // 別的視窗可能也改過設定檔：只把這個視窗改過的欄位寫進去，其他的留檔案裡現在的值（mergeSettings）。
    // 寫不進去（唯讀位置）就算了，本次執行內的狀態還在。
    Settings written;
    settingsWritable_ = saveSettingsMerged(settingsPath_, settingsBase_, settings_, written);
    if (!settingsWritable_) return;
    // 別的視窗記下的東西裡，這幾樣不影響這個視窗正在顯示的畫面，直接收下：新學到的密碼馬上能用，
    // 問過的事不再問一次。其他欄位（檢視值、模式、置頂、暫存位置…）這個視窗繼續用自己的。
    settings_.passwords = written.passwords;
    settings_.askedDefault = written.askedDefault;
    settings_.tipShown = written.tipShown;
    settings_.skipVersion = written.skipVersion;
    settingsBase_ = settings_;
}

// ---------------------------------------------------------------------------
// 語言

void MainWindow::initLanguage() {
    // 內建：英文（key 本身）與繁中（RCDATA）。lang/ 底下同代碼的檔覆蓋內建，其他的加進清單。
    langs_.clear();
    LangTable en;
    en.code = L"en";
    en.name = L"English";
    langs_.push_back(en);
    if (HRSRC res = FindResourceW(inst_, MAKEINTRESOURCEW(IDR_LANG_ZHTW), RT_RCDATA)) {
        if (HGLOBAL g = LoadResource(inst_, res)) {
            const char* bytes = static_cast<const char*>(LockResource(g));
            DWORD n = SizeofResource(inst_, res);
            if (bytes && n) {
                int wn = MultiByteToWideChar(CP_UTF8, 0, bytes, static_cast<int>(n), nullptr, 0);
                std::wstring text(wn, L'\0');
                MultiByteToWideChar(CP_UTF8, 0, bytes, static_cast<int>(n), text.data(), wn);
                if (!text.empty() && text[0] == 0xFEFF) text.erase(0, 1);
                LangTable zh = parseLangText(text);
                if (!zh.code.empty()) langs_.push_back(std::move(zh));
            }
        }
    }
    size_t slash = exePath_.find_last_of(L"\\/");
    std::wstring dir = (slash == std::wstring::npos ? L"" : exePath_.substr(0, slash + 1)) + L"lang";
    for (auto& t : loadLangDir(dir)) {
        bool replaced = false;
        for (auto& b : langs_) {
            if (_wcsicmp(b.code.c_str(), t.code.c_str()) == 0) { b = t; replaced = true; break; }
        }
        if (!replaced) langs_.push_back(std::move(t));
    }
    std::vector<std::wstring> codes;
    for (const auto& t : langs_) codes.push_back(t.code);
    wchar_t locale[LOCALE_NAME_MAX_LENGTH] = L"";
    LCIDToLocaleName(MAKELCID(GetUserDefaultUILanguage(), SORT_DEFAULT), locale, LOCALE_NAME_MAX_LENGTH, 0);
    std::wstring code = pickLanguage(settings_.language, locale, codes);
    for (const auto& t : langs_) {
        if (_wcsicmp(t.code.c_str(), code.c_str()) == 0) { setActiveLang(t); break; }
    }
}

void MainWindow::selectLanguage(const std::wstring& code) {
    if (_wcsicmp(code.c_str(), settings_.language.c_str()) == 0) return;
    settings_.language = code;
    saveSettingsQuiet();
    int r = MessageBoxW(hwnd_, tr(L"The language takes effect the next time mpainter starts.\n\nRestart now?"), kAppName,
                        MB_YESNO | MB_ICONQUESTION);
    if (r != IDYES) return;
    if (!confirmDiscard()) return;
    // 重開自己並開回目前的檔案。壓縮檔用壓縮檔路徑：解出來的暫存檔在這個程式關閉時就刪了。
    std::wstring reopen = !archivePath_.empty() ? archivePath_ : path_;
    ShellExecuteW(nullptr, L"open", exePath_.c_str(), openArgs(reopen).c_str(), nullptr, SW_SHOWNORMAL);
    closePopup();
    closeColorAdjustPanel();
    doc_.markSaved(); // 已確認過，不再問
    rememberPlacement();
    saveSettingsQuiet();
    DestroyWindow(hwnd_);
}

void MainWindow::askDefaultProgram(bool fromSettings) {
    if (!fromSettings) {
        int r = MessageBoxW(hwnd_,
                            tr(L"Make mpainter the default app for opening images?\n\n"
                               L"Double-clicking an image in Explorer will then open it in mpainter.\n"
                               L"Choose “No” and it will not ask again; you can change this later in Edit > Settings."),
                            kAppName, MB_YESNO | MB_ICONQUESTION);
        settings_.askedDefault = true;
        saveSettingsQuiet();
        if (r != IDYES) return;
    }
    registerProgId(exePath_, shellRegOptions());
    const wchar_t* guide = isWindows11()
        ? tr(L"Windows does not let apps set defaults themselves; you choose it in the system Settings.\n\n"
             L"“OK” opens the mpainter page under Settings > Apps > Default apps:\n"
             L"1. The page lists file types such as .jpg and .png.\n"
             L"2. Click each file type, pick mpainter from the list and press “Set default”.\n\n"
             L"If the page does not open on mpainter, type mpainter in the search box.")
        : tr(L"Windows does not let apps set defaults themselves; you choose it in the system Settings.\n\n"
             L"“OK” opens Settings > Apps > Default apps:\n"
             L"1. Scroll to the bottom and click “Set defaults by app”.\n"
             L"2. Find mpainter in the list and click “Manage”.\n"
             L"3. Change the app next to each image format to mpainter.");
    if (MessageBoxW(hwnd_, guide, kAppName, MB_OKCANCEL | MB_ICONINFORMATION) == IDOK) openDefaultAppsSettings();
}

void MainWindow::showSettings(int startPage) {
    endTextEdit(true);
    settingsWritable_ = isPathWritable(settingsPath_);
    SettingsDialogInfo info;
    info.iniPath = settingsPath_;
    info.iniWritable = settingsWritable_;
    info.isWin11 = isWindows11();
    info.onSetDefault = [this] { askDefaultProgram(true); };
    for (const auto& t : langs_) info.languages.emplace_back(t.code, t.name);
    info.exeDir = exeDir();
    if (tempDir_.valid()) {
        size_t slash = tempDir_.path().find_last_of(L"\\/");
        info.unzipRootInUse = slash == std::wstring::npos ? tempDir_.path() : tempDir_.path().substr(0, slash);
    } else {
        info.unzipRootInUse = resolveUnzipRoot(settings_.unzipDir, exeDir());
    }
    info.startPage = startPage;
    std::wstring pendingLang = settings_.language;
    info.onApply = [&](const Settings& s) { applySettingsFromDialog(s, pendingLang); };
    showSettingsDialog(hwnd_, settings_, info);
    // 語言最後才處理：selectLanguage 會問要不要重啟，不能在面板還開著時做。
    if (_wcsicmp(pendingLang.c_str(), settings_.language.c_str()) != 0) selectLanguage(pendingLang);
}

void MainWindow::applySettingsFromDialog(const Settings& s, std::wstring& pendingLang) {
    pendingLang = s.language; // 語言最後才處理（要問要不要重新啟動）
    // 面板交回整份設定：併進來，再照「哪些變了」套用。
    const DialogChanges changed = mergeDialogSettings(settings_, s, exeDir());
    if (changed.contextMenu) {
        if (settings_.contextMenu) registerContextMenu(exePath_, shellRegOptions());
        else unregisterContextMenu(shellRegOptions());
    }
    doc_.setUndoLimit(settings_.undoLimit); // 改小時立刻丟掉最舊的步驟
    doc_.setUndoMemoryLimitMB(settings_.undoMemoryMB);
    undoWarned_ = false; // 上限改了：照新的上限重新判斷要不要提示
    // 模式：換了模式、或正在用的那個內容被改了，就重新套用（目前的值 = 模式本體）
    rebuildProfileMenu();
    if (changed.reapplyProfile)
        if (const Profile* p = findProfile(settings_, settings_.profileName)) switchProfile(Profile(*p)); // 複本：switchProfile 會動到 settings_
    nav_.setPinnedNames(settings_.pinnedNames);
    if (changed.pinnedNames) applyPinnedNow();
    if (changed.unzipRoot) {
        // 正在看壓縮檔就等它關掉再搬（看到一半不搬）；沒在看就現在把舊的刪掉，下次開在新位置建。
        prefetch_.dropBook(); // 背景可能正把下一本解到舊位置：先停下來
        tempDirStale_ = true;
        if (archivePath_.empty()) { tempDir_.release(); tempDirStale_ = false; }
    }
    disarmJump();
    updateTitle();
    saveSettingsQuiet();
    schedulePrefetch();
}

void MainWindow::applyPinnedNow() {
    if (path_.empty() || nav_.count() == 0) return;
    if (!archivePath_.empty()) {
        movePinnedFirst(archiveFiles_, archiveNames_, settings_.pinnedNames);
        nav_.setFiles(archiveFiles_, path_);
    } else {
        nav_.load(path_);
    }
    if (twoPage()) { updateSpreads(); loadSecond(); fit_ = true; fitToView(); }
    updateTitle();
    repaint();
}

bool MainWindow::onKeyDown(WPARAM vk) {
    const bool ctrl = ctrlDown(), shift = shiftDown();
    // 滑鼠正拖著東西時不接受鍵盤：這時復原、刪除會讓拖曳手上那份物件清單和文件對不上。
    if (editor_.dragging()) return true;
    const bool arrow = vk == VK_LEFT || vk == VK_RIGHT || vk == VK_UP || vk == VK_DOWN;
    if (!ctrl && arrow) {
        // 有選取物件：方向鍵微移（Shift 一次 10 像素）。沒有：← → 翻頁（照下面的指令表），↑ ↓ 捲動。
        if (editor_.selectedItem()) {
            float step = shift ? kNudgeLargePx : kNudgePx;
            if (editor_.nudgeSelected(vk == VK_LEFT ? -step : vk == VK_RIGHT ? step : 0, vk == VK_UP ? -step : vk == VK_DOWN ? step : 0))
                afterItemChange();
            return true;
        }
        if (vk == VK_UP || vk == VK_DOWN) { scrollKey(static_cast<int>(vk)); return true; }
    }
    // 指令：按鍵對應與可用條件都在 core/Commands.cpp，和選單同一份。不能用的指令 onCommand 不會做，但按鍵算處理過了。
    if (int command = commandForKey(static_cast<unsigned>(vk), ctrl, shift)) {
        onCommand(command);
        return true;
    }
    if (ctrl) return false;
    disarmJump(); // 待命中的跳本：按了翻頁以外的鍵就取消
    switch (vk) {
    case VK_ESCAPE:
        // 取消什麼由 Editor 決定（裁切、取白點、選取、塗鴉，一次一個）。
        if (editor_.cropping()) cancelCrop(); // 還要把檢視還原成進入裁切前的樣子
        else if (editor_.escape() != Editor::Escape::Nothing) { buildButtons(); repaint(); }
        return true;
    case VK_RETURN:
        if (const Annotation* sel = editor_.selectedItem()) {
            if (sel->kind == Kind::Text) { beginTextEdit(sel->id, false); return true; }
        }
        return false;
    default: return false;
    }
}

// ---------------------------------------------------------------------------
// 標註

void MainWindow::select(uint32_t id) {
    editor_.select(id);
    buildButtons();
    repaint();
}

void MainWindow::afterItemChange() {
    updateTitle();
    positionEdit();
    repaint();
}

void MainWindow::deleteSelected() {
    if (!editor_.deleteSelected()) return;
    updateTitle();
    repaint();
}

// 新物件放在畫布中央；看得到的範圍的短邊（圖片像素）決定預設大小。
float MainWindow::visibleShortSide() const {
    float cw = canvasRc_.right - canvasRc_.left, ch = canvasRc_.bottom - canvasRc_.top;
    return std::min(cw, ch) / zoom_;
}

void MainWindow::addShape(Kind kind) {
    if (!doc_.image() || !editGuard()) return;
    endTextEdit(true);
    if (!editor_.addShape(kind, toImage(canvasCenter()), visibleShortSide())) return;
    buildButtons();
    repaint();
    updateTitle();
}

void MainWindow::addText() {
    if (!doc_.image() || !editGuard()) return;
    endTextEdit(true);
    uint32_t id = editor_.addText(toImage(canvasCenter()), visibleShortSide(), tr(L"Text"));
    if (!id) return;
    buildButtons();
    repaint();
    updateTitle();
    beginTextEdit(id, true);
}

void MainWindow::applyStrokeStyle(const StrokeStyle& s) {
    if (editor_.applyStrokeStyle(s)) {
        afterItemChange();
        if (edit_) InvalidateRect(edit_, nullptr, TRUE);
    }
    buildButtons();
    repaint();
}

void MainWindow::applyTextStyle(const TextStyle& t) {
    if (editor_.applyTextStyle(t)) {
        afterItemChange();
        updateEditFont();
    }
    repaint();
}

void MainWindow::rotateImage() {
    if (!doc_.image()) return; // 壓縮檔內也能轉：當成檢視動作，翻頁就重置
    endInteraction(true);
    editor_.rotateCCW();
    if (readOnly()) doc_.markSaved(); // 唯讀：旋轉不算修改，標題不加 *
    infoData_.width = doc_.image()->width;
    infoData_.height = doc_.image()->height;
    fit_ = true;
    fitToView();
    updateTitle();
    repaint();
}

void MainWindow::adjustSize() {
    auto img = doc_.image();
    if (!img || !editGuard()) return;
    // 調整顏色的面板與裁切框都是照現在的尺寸算的：尺寸要變了，先收掉。
    endInteraction(true);
    ResizeResult r;
    if (!showResizeDialog(hwnd_, img->width, img->height, doc_.dpi().x, r)) return;
    editor_.resize(r.width, r.height, r.dpi, r.resample);
    infoData_.width = doc_.image()->width;
    infoData_.height = doc_.image()->height;
    infoData_.dpiX = infoData_.dpiY = r.dpi;
    fit_ = true;
    fitToView();
    updateTitle();
    repaint();
}

// ---------------------------------------------------------------------------
// 裁切

void MainWindow::beginCrop() {
    if (!doc_.image() || twoPage() || !editGuard()) return;
    endTextEdit(true);
    if (editor_.adjusting()) { closeColorAdjustPanel(); cancelAdjust(); }
    hideTip();
    if (!editor_.beginCrop()) return;
    // 整張圖放進畫布；原本的檢視記下來，取消時還原。
    cropSavedZoom_ = zoom_;
    cropSavedOffset_ = offset_;
    cropSavedFit_ = fit_;
    cropFit_ = true;
    fit_ = true;
    fitToView();
    repaint();
}

void MainWindow::cancelCrop() {
    const bool wasDraggingBox = editor_.drag() == DragKind::Crop;
    if (!editor_.cancelCrop()) return;
    if (wasDraggingBox && GetCapture() == hwnd_) ReleaseCapture();
    cropFit_ = false;
    zoom_ = cropSavedZoom_;
    offset_ = cropSavedOffset_;
    fit_ = cropSavedFit_;
    if (fit_) fitToView(false); else clampOffset();
    buildButtons();
    repaint();
}

void MainWindow::applyCrop() {
    if (!editor_.cropping() || !doc_.image()) return;
    // 框就是整張圖：等於取消，檢視也要還原。先用同一個框比，再決定走哪一邊。
    const CropBox box = editor_.cropBox();
    if (box == CropBox{0, 0, doc_.image()->width, doc_.image()->height}) { cancelCrop(); return; }
    if (editor_.applyCrop() != Editor::CropResult::Applied) return;
    cropFit_ = false;
    infoData_.width = doc_.image()->width;
    infoData_.height = doc_.image()->height;
    fit_ = true;
    fitToView();
    updateTitle();
    repaint();
}

void MainWindow::drawCrop() {
    auto img = doc_.image();
    if (!editor_.cropping() || !img) return;
    const CropBox& cropBox_ = editor_.cropBox();
    auto* rt = renderer_.rt();
    rt->SetTransform(D2D1::Matrix3x2F::Identity());
    POINT i0 = toClient({0, 0});
    POINT i1 = toClient({static_cast<float>(img->width), static_cast<float>(img->height)});
    POINT b0 = toClient({static_cast<float>(cropBox_.left), static_cast<float>(cropBox_.top)});
    POINT b1 = toClient({static_cast<float>(cropBox_.right), static_cast<float>(cropBox_.bottom)});
    float il = static_cast<float>(i0.x), it = static_cast<float>(i0.y), ir = static_cast<float>(i1.x), ib = static_cast<float>(i1.y);
    float bl = static_cast<float>(b0.x), bt = static_cast<float>(b0.y), br = static_cast<float>(b1.x), bb = static_cast<float>(b1.y);
    // 會被裁掉的部分蓋半透明深色：上、下兩條整寬，左、右夾在中間
    auto* dim = renderer_.brush(D2D1::ColorF(0, 0, 0, 0.5f));
    rt->FillRectangle(D2D1::RectF(il, it, ir, bt), dim);
    rt->FillRectangle(D2D1::RectF(il, bb, ir, ib), dim);
    rt->FillRectangle(D2D1::RectF(il, bt, bl, bb), dim);
    rt->FillRectangle(D2D1::RectF(br, bt, ir, bb), dim);
    rt->DrawRectangle(D2D1::RectF(bl + 0.5f, bt + 0.5f, br - 0.5f, bb - 0.5f), renderer_.brush(D2D1::ColorF(1, 1, 1, 0.9f)), 1.0f);
    // 四個角的 L 形把手畫在框內；深色底再疊白色，淺色、深色的圖上都看得到
    float len = std::min({px(kCropHandleLen), (br - bl) / 2, (bb - bt) / 2});
    float w = px(4), inset = w / 2;
    auto geo = renderer_.device().pathGeometry([&](ID2D1GeometrySink& sink) {
        auto corner = [&](float x, float y, float sx, float sy) { // (sx, sy)：往框內的方向
            float cx = x + sx * inset, cy = y + sy * inset;
            sink.BeginFigure(D2D1::Point2F(cx, y + sy * len), D2D1_FIGURE_BEGIN_HOLLOW);
            sink.AddLine(D2D1::Point2F(cx, cy));
            sink.AddLine(D2D1::Point2F(x + sx * len, cy));
            sink.EndFigure(D2D1_FIGURE_END_OPEN);
        };
        corner(bl, bt, 1, 1);
        corner(br, bt, -1, 1);
        corner(bl, bb, 1, -1);
        corner(br, bb, -1, -1);
    });
    if (geo) {
        rt->DrawGeometry(geo.Get(), renderer_.brush(D2D1::ColorF(0, 0, 0, 0.55f)), w + px(2));
        rt->DrawGeometry(geo.Get(), renderer_.brush(D2D1::ColorF(1, 1, 1)), w);
    }
    // 拖曳中：框中央顯示裁切後的像素尺寸
    if (editor_.drag() == DragKind::Crop) {
        std::wstring s = std::to_wstring(cropBox_.width()) + L" × " + std::to_wstring(cropBox_.height());
        float h = 0;
        float tw = renderer_.measureUiText(s, px(12), true, 1e6f, &h);
        float cx = (bl + br) / 2, cy = (bt + bb) / 2;
        D2D1_RECT_F pill = D2D1::RectF(cx - tw / 2 - px(8), cy - h / 2 - px(4), cx + tw / 2 + px(8), cy + h / 2 + px(4));
        rt->FillRoundedRectangle(D2D1::RoundedRect(pill, px(4), px(4)), renderer_.brush(D2D1::ColorF(0, 0, 0, 0.6f)));
        renderer_.drawUiText(s, pill, px(12), D2D1::ColorF(D2D1::ColorF::White), DWRITE_TEXT_ALIGNMENT_CENTER, true);
    }
}

// ---------------------------------------------------------------------------
// 文字編輯（用系統 EDIT 控制項蓋在文字方塊上）

void MainWindow::beginTextEdit(uint32_t id, bool selectAll) {
    if (edit_) endTextEdit(true);
    if (!editor_.beginTextEdit(id)) return;
    const Annotation* a = doc_.findItem(id);
    const std::wstring& text = editor_.editText();
    DWORD align = a->text.align == TextAlign::Center ? ES_CENTER : a->text.align == TextAlign::Right ? ES_RIGHT : ES_LEFT;
    edit_ = CreateWindowExW(0, L"EDIT", text.c_str(),
                            WS_CHILD | WS_VISIBLE | ES_MULTILINE | ES_AUTOVSCROLL | ES_WANTRETURN | align, 0, 0, 10, 10,
                            hwnd_, nullptr, inst_, nullptr);
    SetWindowSubclass(edit_, editProc, kEditSubclass, reinterpret_cast<DWORD_PTR>(this));
    updateEditFont();
    positionEdit();
    SetFocus(edit_);
    if (selectAll) SendMessageW(edit_, EM_SETSEL, 0, -1);
    else SendMessageW(edit_, EM_SETSEL, static_cast<WPARAM>(text.size()), static_cast<LPARAM>(text.size()));
    repaint();
}

void MainWindow::updateEditFont() {
    if (!edit_) return;
    const Annotation* a = doc_.findItem(editor_.editingId());
    if (!a) return;
    if (editFont_) DeleteObject(editFont_);
    int h = -static_cast<int>(std::lround(a->text.size * zoom_));
    editFont_ = CreateFontW(h, 0, 0, 0, a->text.bold ? FW_BOLD : FW_NORMAL, a->text.italic, a->text.underline, FALSE,
                            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH,
                            a->text.font.c_str());
    SendMessageW(edit_, WM_SETFONT, reinterpret_cast<WPARAM>(editFont_), TRUE);
    LONG style = GetWindowLongW(edit_, GWL_STYLE) & ~(ES_LEFT | ES_CENTER | ES_RIGHT);
    style |= a->text.align == TextAlign::Center ? ES_CENTER : a->text.align == TextAlign::Right ? ES_RIGHT : ES_LEFT;
    SetWindowLongW(edit_, GWL_STYLE, style);
    positionEdit();
    InvalidateRect(edit_, nullptr, TRUE);
}

void MainWindow::positionEdit() {
    if (!edit_) return;
    const Annotation* a = doc_.findItem(editor_.editingId());
    if (!a) return;
    float pad = 4.0f; // 和畫文字時的留白一樣（render/AnnotationPainter.cpp 的 kTextPad）
    POINT tl = toClient({a->rect.left + pad, a->rect.top + pad});
    POINT brp = toClient({a->rect.right - pad, a->rect.bottom - pad});
    int w = std::max(10l, brp.x - tl.x), h = std::max(10l, brp.y - tl.y);
    SetWindowPos(edit_, HWND_TOP, tl.x, tl.y, w, h, SWP_NOACTIVATE);
}

void MainWindow::onEditChanged() {
    if (!edit_) return;
    int n = GetWindowTextLengthW(edit_);
    std::wstring text(static_cast<size_t>(n), L'\0');
    GetWindowTextW(edit_, text.data(), n + 1);
    // 方塊的高度跟著內容長。
    if (editor_.setEditText(text)) {
        positionEdit();
        repaint();
    }
}

void MainWindow::endTextEdit(bool commit) {
    if (!edit_) return;
    HWND e = edit_;
    edit_ = nullptr;
    RemoveWindowSubclass(e, editProc, kEditSubclass);
    DestroyWindow(e);
    SetFocus(hwnd_);
    editor_.endTextEdit(commit);
    updateTitle();
    repaint();
}

// ---------------------------------------------------------------------------
// 調整顏色（ColorAdjustHost）

const Image& MainWindow::baseImage() { return *editor_.adjustBase(); }

void MainWindow::previewChanged(const ColorAdjustParams& p) {
    if (editor_.adjustBase()) SetCursor(LoadCursorW(nullptr, IDC_WAIT));
    editor_.previewAdjust(p);
    repaint();
}

void MainWindow::commitAdjust(const ColorAdjustParams& p) {
    if (editor_.commitAdjust(p)) updateTitle();
    buildButtons();
    repaint();
}

void MainWindow::cancelAdjust() {
    editor_.cancelAdjust();
    buildButtons();
    repaint();
}

void MainWindow::beginPickWhite() {
    editor_.beginPickWhite();
    SetFocus(hwnd_);
}

// ---------------------------------------------------------------------------
// 新版本檢查與自動更新

void MainWindow::startAutoUpdateCheck() {
    // 背景執行緒只拿值，不碰 this：視窗先關了也不會出事（PostMessage 失敗就自己 delete）。
    std::wstring url = settings_.updateUrl.empty() ? kDefaultUpdateUrl : settings_.updateUrl;
    HWND hwnd = hwnd_;
    std::thread([url, hwnd] {
        ReleaseInfo r;
        if (!fetchLatestRelease(url, r).ok()) return; // 自動檢查失敗不吭聲
        if (!isNewerVersion(r.version, MP_VERSION_WSTRING)) return;
        auto* p = new ReleaseInfo(std::move(r));
        if (!PostMessageW(hwnd, WM_MP_UPDATE_FOUND, 0, reinterpret_cast<LPARAM>(p))) delete p;
    }).detach();
}

bool MainWindow::canInterrupt() const {
    return IsWindowEnabled(hwnd_) && !menuLoop_ && !editor_.dragging() && GetCapture() != hwnd_;
}

void MainWindow::offerPendingUpdate() {
    if (!pendingUpdate_) return;
    // 使用者正在做事（對話框、選單、拖曳，加上彈出選單、調色面板、文字編輯）就晚點再問。
    if (!canInterrupt() || isPopupOpen() || isColorAdjustPanelOpen() || edit_) {
        SetTimer(hwnd_, kUpdateTimer, 3000, nullptr);
        return;
    }
    ReleaseInfo r = *pendingUpdate_;
    pendingUpdate_.reset();
    offerUpdate(r);
}

void MainWindow::checkForUpdatesNow() {
    std::wstring url = settings_.updateUrl.empty() ? kDefaultUpdateUrl : settings_.updateUrl;
    ReleaseInfo r;
    UpdateError err = checkUpdateWithProgress(hwnd_, url, r);
    if (!err.ok()) {
        if (!err.cancelled())
            MessageBoxW(hwnd_, fmt1(L"Could not check for updates: %s", updateErrorText(err)).c_str(), kAppName, MB_ICONWARNING);
        return;
    }
    if (!isNewerVersion(r.version, MP_VERSION_WSTRING)) {
        MessageBoxW(hwnd_, fmt1(L"mpainter %s is the latest version.", MP_VERSION_WSTRING).c_str(), kAppName, MB_ICONINFORMATION);
        return;
    }
    offerUpdate(r);
}

void MainWindow::offerUpdate(const ReleaseInfo& r) {
    endTextEdit(true);
    switch (showUpdateDialog(hwnd_, r, MP_VERSION_WSTRING)) {
    case UpdateChoice::Skip:
        settings_.skipVersion = r.version;
        saveSettingsQuiet();
        break;
    case UpdateChoice::Install: installUpdate(r); break;
    case UpdateChoice::Later: break;
    }
}

void MainWindow::manualDownloadFallback(const std::wstring& reason, const std::wstring& pageUrl) {
    std::wstring msg = reason + L"\n\n" + tr(L"Open the download page to update by hand?");
    if (MessageBoxW(hwnd_, msg.c_str(), kAppName, MB_YESNO | MB_ICONINFORMATION) == IDYES)
        ShellExecuteW(hwnd_, L"open", pageUrl.empty() ? kReleasesPage : pageUrl.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

bool MainWindow::closeOtherInstances() {
    auto pids = otherProcessesOfImage(exePath_, GetCurrentProcessId());
    if (pids.empty()) return true;
    std::vector<HANDLE> procs;
    for (auto pid : pids) {
        if (HANDLE h = OpenProcess(SYNCHRONIZE, FALSE, pid)) procs.push_back(h);
        AllowSetForegroundWindow(pid); // 讓那邊的存檔詢問能跳到前面
    }
    bool ok = closeWindowsWithProgress(hwnd_, closeForUpdateMessage(), mainWindowsOf(pids), procs);
    for (HANDLE h : procs) CloseHandle(h);
    return ok;
}

void MainWindow::installUpdate(const ReleaseInfo& r) {
    std::wstring app = exeDir();
    // 不能自動更新的情況：先講清楚原因，再給下載頁。
    if (r.zipUrl.empty() || r.shaUrl.empty()) {
        manualDownloadFallback(tr(L"This release cannot be installed automatically."), r.pageUrl);
        return;
    }
    if (!ensureArchiveLibrary()) {
        manualDownloadFallback(tr(L"7z.dll is missing next to mpainter.exe, so the update cannot be unpacked."), r.pageUrl);
        return;
    }
    // 下載之前就能知道的先看：exe 所在的資料夾能不能寫。發佈包解開之後再照實際要放的每個資料夾檢查一次。
    if (!isDirWritable(app)) {
        manualDownloadFallback(fmt1(L"mpainter cannot write to its folder:\n%s", app), r.pageUrl);
        return;
    }
    // 同一個 exe 還有別的 mpainter 開著：先說清楚全部都會被關掉。
    auto others = otherProcessesOfImage(exePath_, GetCurrentProcessId());
    if (!others.empty()) {
        wchar_t msg[640];
        swprintf(msg, 640, tr(L"%d other mpainter windows are open. Installing the update closes all of them (each asks to save unsaved changes first); only this one opens again. Continue?"),
                 static_cast<int>(others.size()));
        if (!askOk(hwnd_, msg, TD_INFORMATION_ICON, true)) return;
    }
    if (!confirmDiscard()) return;

    wchar_t tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    std::wstring work = std::wstring(tmp) + L"mpainter\\update-" + std::to_wstring(GetCurrentProcessId());
    removeDirectoryRecursive(work);
    if (!createDirectories(work)) {
        MessageBoxW(hwnd_, fmt1(L"Update failed: %s", L"cannot create " + work).c_str(), kAppName, MB_ICONWARNING);
        return;
    }
    std::wstring staging;
    UpdateError err = downloadUpdateWithProgress(hwnd_, r, work, staging);
    if (!err.ok()) {
        removeDirectoryRecursive(work);
        if (!err.cancelled()) MessageBoxW(hwnd_, fmt1(L"Update failed: %s", updateFailureText(err)).c_str(), kAppName, MB_ICONWARNING);
        return;
    }
    const std::wstring exeName = fileNameOf(exePath_);
    auto steps = planInstall(staging, app, exeName);
    if (UpdateError blocked = installTargetsWritable(steps); !blocked.ok()) {
        removeDirectoryRecursive(work);
        manualDownloadFallback(updateErrorText(blocked), r.pageUrl);
        return;
    }
    // 換檔前把其他 mpainter 關掉（下載期間新開的也算）；有一個沒關就整個取消。
    if (!closeOtherInstances()) {
        removeDirectoryRecursive(work);
        SetForegroundWindow(hwnd_); // 使用者剛在別的視窗按了取消，焦點在那邊
        askOk(hwnd_, tr(L"Another mpainter window was not closed, so the update was cancelled. Nothing was changed."),
              TD_WARNING_ICON, false);
        return;
    }
    err = applyInstall(steps);
    removeDirectoryRecursive(work);
    if (!err.ok()) {
        MessageBoxW(hwnd_, fmt1(L"Update failed: %s", updateErrorText(err) + L"\n\n" + tr(L"Nothing was changed.")).c_str(), kAppName,
                    MB_ICONWARNING);
        return;
    }
    // 重開時開回目前這本：壓縮檔用壓縮檔路徑（解出來的暫存檔關閉時就刪了）。
    std::wstring reopen = !archivePath_.empty() ? archivePath_ : path_;
    relaunchArgs_ = afterUpdateArgs(GetCurrentProcessId(), reopen);
    updating_ = true;
    PostMessageW(hwnd_, WM_CLOSE, 0, 0);
}

} // namespace mp
