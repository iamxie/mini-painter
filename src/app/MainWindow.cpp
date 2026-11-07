#include "app/MainWindow.h"

#include <thread>

#include <commctrl.h>
#include <commdlg.h>
#include <shellapi.h>
#include <windowsx.h>

#include <algorithm>
#include <cmath>

#include "app/Popups.h"
#include "app/resource.h"
#include "core/ImageIO.h"
#include "core/ImageOps.h"
#include "core/Lang.h"
#include "core/ShellReg.h"
#include "core/Shelf.h"

namespace mp {

namespace {

constexpr wchar_t kClassName[] = L"MiniPainterMain";
constexpr wchar_t kAppName[] = L"mpainter";
constexpr UINT_PTR kTipTimer = 1, kTipHideTimer = 2, kBtnTipTimer = 3, kHqTimer = 4, kToastTimer = 5, kArmTimer = 6;
constexpr UINT kToastMs = 2000, kArmMs = 3000;
constexpr UINT kHqDelayMs = 200;
constexpr UINT kTipDelayMs = 500, kTipShowMs = 6000;
constexpr float kToolbarH = 44, kMarkupH = 40, kNameH = 26, kInfoW = 320, kBtnW = 36, kBtnH = 30;
constexpr float kHandleR = 5.0f;
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

std::wstring fileNameOf(const std::wstring& path) {
    size_t s = path.find_last_of(L"\\/");
    return s == std::wstring::npos ? path : path.substr(s + 1);
}

// 翻譯後的格式字串填一個 %s。
std::wstring fmt1(const wchar_t* key, const std::wstring& arg) {
    const wchar_t* f = tr(key);
    std::wstring out(wcslen(f) + arg.size() + 8, L'\0');
    int n = swprintf(out.data(), out.size(), f, arg.c_str());
    out.resize(n > 0 ? static_cast<size_t>(n) : 0);
    return out;
}

// 翻譯後的格式字串填兩個 %s。
std::wstring fmt2(const wchar_t* key, const std::wstring& a, const std::wstring& b) {
    const wchar_t* f = tr(key);
    std::wstring out(wcslen(f) + a.size() + b.size() + 8, L'\0');
    int n = swprintf(out.data(), out.size(), f, a.c_str(), b.c_str());
    out.resize(n > 0 ? static_cast<size_t>(n) : 0);
    return out;
}

} // namespace

// ---------------------------------------------------------------------------
// 建立

bool MainWindow::create(HINSTANCE inst, const std::wstring& initialPath, int nCmdShow) {
    inst_ = inst;
    if (!renderer_.init()) return false;
    loadSettingsAndRegister();
    {
        // 清掉當掉的執行個體留下的壓縮檔暫存（lock 沒人持有的子資料夾）：設定的位置、exe 旁的 temp、退路的 %TEMP%。
        std::wstring root = resolveUnzipRoot(settings_.unzipDir, exeDir()), def = defaultUnzipRoot(exeDir()), fb = fallbackUnzipRoot();
        std::thread([root, def, fb] {
            sweepStaleTempDirs(root);
            if (_wcsicmp(root.c_str(), def.c_str()) != 0) sweepStaleTempDirs(def);
            sweepStaleTempDirs(fb);
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
        if (to == self->hwnd_) PostMessageW(self->hwnd_, WM_APP + 1, 0, 0);
    }
    return DefSubclassProc(hwnd, msg, wp, lp);
}

LRESULT MainWindow::handle(UINT msg, WPARAM wp, LPARAM lp) {
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
            if (drag_ == Drag::None && hoverItem_ && hitItemBox(p) == hoverItem_ && settings_.tipShown < kTipMaxShown) showTip(p);
            return 0;
        }
        if (wp == kTipHideTimer) { hideTip(); return 0; }
        if (wp == kToastTimer) { hideToast(); return 0; }
        if (wp == kArmTimer) { disarmJump(); return 0; }
        if (wp == kHqTimer) {
            KillTimer(hwnd_, kHqTimer);
            hqBusy_ = false;
            repaint();
            return 0;
        }
        if (wp == kBtnTipTimer) {
            KillTimer(hwnd_, kBtnTipTimer);
            if (drag_ == Drag::None && pressBtn_ == BTN_NONE && hoverBtn_ != BTN_NONE) showButtonTip(hoverBtn_);
            return 0;
        }
        break;
    case WM_MP_POPUP_CLOSED:
        SetFocus(edit_ ? edit_ : hwnd_);
        return 0;
    case WM_APP + 2: // 第一次開圖後詢問預設程式
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
            const Annotation* item = doc_.findItem(editingId_);
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
    case WM_APP + 1: // 編輯框失去焦點給主視窗
        if (edit_ && GetFocus() != edit_) endTextEdit(true);
        return 0;
    case WM_DROPFILES: {
        HDROP drop = reinterpret_cast<HDROP>(wp);
        wchar_t buf[MAX_PATH];
        if (DragQueryFileW(drop, 0, buf, MAX_PATH) && confirmDiscard()) openPath(buf);
        DragFinish(drop);
        return 0;
    }
    case WM_INITMENUPOPUP: updateMenu(); return 0;
    case WM_CLOSE:
        if (!confirmDiscard()) return 0;
        closePopup();
        closeColorAdjustPanel();
        rememberPlacement();
        saveSettingsQuiet();
        DestroyWindow(hwnd_);
        return 0;
    case WM_DESTROY:
        closeArchive();
        tempDir_.release();
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
        x += px(12);
        x = add(BTN_LINESTYLE, x, y, true);
        x = add(BTN_LINECOLOR, x, y, true);
        x = add(BTN_FILL, x, y, true);
        x = add(BTN_FORMAT, x, y, true);
    }
    for (auto& b : buttons_) {
        switch (b.id) {
        case BTN_ZOOM100: b.enabled = std::abs(zoom_ - 1.0f) > 1e-4f; break;
        case BTN_MARKUP: b.pressed = showMarkup_; break;
        case BTN_INFO: b.pressed = showInfo_; break;
        case BTN_FREEHAND: b.pressed = freehandMode_; break;
        case BTN_ADJCOLOR: b.pressed = adjusting_; break;
        default: break;
        }
        if (!doc_.image() && b.id != BTN_INFO && b.id != BTN_MARKUP) b.enabled = false;
        if (twoPage() && (b.id == BTN_MARKUP || b.id == BTN_ROTATE)) b.enabled = false;
    }
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
    // 快捷鍵文字不進翻譯表：翻譯者只翻名稱。
    auto item = [](HMENU m, int id, const wchar_t* key, const wchar_t* accel = nullptr) {
        std::wstring t = trs(key);
        if (accel) { t += L'\t'; t += accel; }
        AppendMenuW(m, MF_STRING, id, t.c_str());
    };
    auto sep = [](HMENU m) { AppendMenuW(m, MF_SEPARATOR, 0, nullptr); };
    HMENU file = CreatePopupMenu();
    item(file, IDM_OPEN, L"Open…", L"Ctrl+O");
    item(file, IDM_SAVE, L"Save", L"Ctrl+S");
    item(file, IDM_SAVEAS, L"Export As…", L"Ctrl+Shift+S");
    sep(file);
    item(file, IDM_EXIT, L"Exit", L"Alt+F4");
    HMENU edit = CreatePopupMenu();
    item(edit, IDM_UNDO, L"Undo", L"Ctrl+Z");
    item(edit, IDM_REDO, L"Redo", L"Ctrl+Y");
    sep(edit);
    item(edit, IDM_DELETE, L"Delete Selected Object", L"Del");
    sep(edit);
    item(edit, IDM_ROTATE, L"Rotate 90° Counterclockwise", L"Ctrl+R");
    sep(edit);
    item(edit, IDM_SETTINGS, L"Settings…");
    HMENU view = CreatePopupMenu();
    // 模式（設定集）：選一個整組套上；內容由 rebuildProfileMenu 填
    profileMenu_ = CreatePopupMenu();
    rebuildProfileMenu();
    AppendMenuW(view, MF_POPUP, reinterpret_cast<UINT_PTR>(profileMenu_), tr(L"Mode"));
    sep(view);
    item(view, IDM_ZOOMIN, L"Zoom In", L"Ctrl++");
    item(view, IDM_ZOOMOUT, L"Zoom Out", L"Ctrl+-");
    sep(view);
    // 檢視模式（單選，跨翻頁延續）
    item(view, IDM_ZOOMFIT, L"Fit to Window", L"Ctrl+9");
    item(view, IDM_FITWIDTH, L"Fit to Width");
    item(view, IDM_FITHEIGHT, L"Fit to Height");
    item(view, IDM_ZOOM100, L"Actual Size", L"Ctrl+0");
    item(view, IDM_TWOPAGE, L"Two Pages Side by Side");
    sep(view);
    item(view, IDM_UPSCALE, L"Enlarge Small Images to Fit");
    sep(view);
    // 兩頁並排的選項
    item(view, IDM_RTL, L"Right to Left (Manga)");
    item(view, IDM_FIRSTALONE, L"First Page Alone");
    item(view, IDM_SHIFTONE, L"Shift Pairing by One Page");
    sep(view);
    item(view, IDM_PREV, L"Previous Image", L"←");
    item(view, IDM_NEXT, L"Next Image", L"→");
    item(view, IDM_AUTONEXT, L"Continue to the next folder or archive at the end");
    sep(view);
    item(view, IDM_MARKUP, L"Markup Toolbar");
    item(view, IDM_INFO, L"Info Panel");
    // 語言：跟隨系統 + 內建 + lang/ 掃到的。切錯語系也找得到回來的路，所以標題兩種語言並列。
    HMENU lang = CreatePopupMenu();
    item(lang, IDM_LANG_SYSTEM, L"Follow System");
    sep(lang);
    for (size_t i = 0; i < langs_.size() && IDM_LANG_FIRST + static_cast<int>(i) <= IDM_LANG_LAST; ++i)
        AppendMenuW(lang, MF_STRING, IDM_LANG_FIRST + static_cast<UINT_PTR>(i), langs_[i].name.c_str());
    AppendMenuW(menu_, MF_POPUP, reinterpret_cast<UINT_PTR>(file), tr(L"&File"));
    AppendMenuW(menu_, MF_POPUP, reinterpret_cast<UINT_PTR>(edit), tr(L"&Edit"));
    AppendMenuW(menu_, MF_POPUP, reinterpret_cast<UINT_PTR>(view), tr(L"&View"));
    AppendMenuW(menu_, MF_POPUP, reinterpret_cast<UINT_PTR>(lang), L"語言 / &Language");
    HMENU help = CreatePopupMenu();
    item(help, IDM_ABOUT, L"About mpainter…");
    AppendMenuW(menu_, MF_POPUP, reinterpret_cast<UINT_PTR>(help), tr(L"&Help"));
}

void MainWindow::rebuildProfileMenu() {
    if (!profileMenu_) return;
    while (GetMenuItemCount(profileMenu_) > 0) RemoveMenu(profileMenu_, 0, MF_BYPOSITION);
    for (size_t i = 0; i < settings_.profiles.size() && IDM_PROFILE_FIRST + static_cast<int>(i) <= IDM_PROFILE_LAST; ++i)
        AppendMenuW(profileMenu_, MF_STRING, IDM_PROFILE_FIRST + static_cast<UINT_PTR>(i), settings_.profiles[i].name.c_str());
    AppendMenuW(profileMenu_, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(profileMenu_, MF_STRING, IDM_PROFILE_SAVE, L""); // 文字在 updateMenu 填（帶模式名）
    AppendMenuW(profileMenu_, MF_STRING, IDM_PROFILE_MANAGE, tr(L"Manage Modes…"));
}

void MainWindow::switchProfile(const Profile& p) {
    applyProfile(settings_, p);
    spreadsKey_.clear(); // 兩頁並排的選項可能變了，配對重算
    disarmJump();
    setViewMode(viewModeFromString(settings_.viewMode)); // 會存 ini、重算配對、重新符合、更新標題、重畫
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
    en(IDM_PROFILE_SAVE, modified && curProfile >= 0);
    chk(IDM_LANG_SYSTEM, settings_.language.empty());
    for (size_t i = 0; i < langs_.size(); ++i)
        chk(IDM_LANG_FIRST + static_cast<int>(i), !settings_.language.empty() && _wcsicmp(langs_[i].code.c_str(), settings_.language.c_str()) == 0);
    bool has = doc_.image() != nullptr;
    en(IDM_SAVE, has && doc_.dirty());
    en(IDM_SAVEAS, has);
    en(IDM_UNDO, doc_.canUndo());
    en(IDM_REDO, doc_.canRedo());
    en(IDM_DELETE, selected_ != 0);
    en(IDM_ROTATE, has);
    en(IDM_ZOOMIN, has);
    en(IDM_ZOOMOUT, has);
    en(IDM_ZOOM100, has);
    en(IDM_ZOOMFIT, has);
    // 開了「接著看下一本」時到底了也能按（按了會提示 / 跳本）
    bool more = settings_.autoNextBook && nav_.count() > 0;
    en(IDM_PREV, nav_.hasPrev() || more);
    en(IDM_NEXT, nav_.hasNext() || more);
    chk(IDM_AUTONEXT, settings_.autoNextBook);
    chk(IDM_MARKUP, showMarkup_);
    chk(IDM_INFO, showInfo_);
    en(IDM_FITWIDTH, has);
    en(IDM_FITHEIGHT, has);
    en(IDM_TWOPAGE, has);
    // 五種模式的 id 連續（123..129，中間沒有別的項目），用 CheckMenuRadioItem 畫圓點。
    int modeId = viewMode_ == ViewMode::FitWidth ? IDM_FITWIDTH : viewMode_ == ViewMode::FitHeight ? IDM_FITHEIGHT
               : viewMode_ == ViewMode::Actual ? IDM_ZOOM100 : viewMode_ == ViewMode::TwoPage ? IDM_TWOPAGE : IDM_ZOOMFIT;
    CheckMenuRadioItem(menu_, IDM_ZOOM100, IDM_TWOPAGE, modeId, MF_BYCOMMAND);
    chk(IDM_UPSCALE, settings_.upscaleSmall);
    chk(IDM_RTL, settings_.rightToLeft);
    chk(IDM_FIRSTALONE, settings_.firstPageAlone);
    chk(IDM_SHIFTONE, shiftOne_);
    // 兩頁並排只看不編
    if (twoPage()) {
        for (int id : {IDM_UNDO, IDM_REDO, IDM_DELETE, IDM_ROTATE, IDM_MARKUP, IDM_SAVE}) en(id, false);
        int cur = currentSpread();
        en(IDM_PREV, cur > 0 || more);
        en(IDM_NEXT, (cur >= 0 && cur + 1 < static_cast<int>(spreads_.size())) || more);
    }
}

void MainWindow::onCommand(int id) {
    if (id != IDM_PREV && id != IDM_NEXT) disarmJump(); // 待命中的跳本：做了翻頁以外的事就取消
    switch (id) {
    case IDM_OPEN: openDialog(); break;
    case IDM_SAVE: save(); break;
    case IDM_SAVEAS: exportAs(); break;
    case IDM_EXIT: PostMessageW(hwnd_, WM_CLOSE, 0, 0); break;
    case IDM_UNDO:
    case IDM_REDO: {
        endTextEdit(true);
        if (id == IDM_UNDO ? !doc_.canUndo() : !doc_.canRedo()) break;
        auto before = doc_.image();
        if (id == IDM_UNDO) doc_.undo(); else doc_.redo();
        if (!selectedItem()) selected_ = 0;
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
        settings_.upscaleSmall = !settings_.upscaleSmall;
        saveSettingsQuiet();
        if (fit_) { fitToView(); repaint(); }
        updateTitle();
        break;
    case IDM_RTL:
        settings_.rightToLeft = !settings_.rightToLeft;
        saveSettingsQuiet();
        if (twoPage()) { fit_ = true; fitToView(); }
        updateTitle();
        repaint();
        break;
    case IDM_FIRSTALONE:
    case IDM_SHIFTONE:
        if (id == IDM_FIRSTALONE) { settings_.firstPageAlone = !settings_.firstPageAlone; saveSettingsQuiet(); updateTitle(); }
        else shiftOne_ = !shiftOne_;
        if (twoPage()) {
            // 配對變了：重算，目前這頁可能變成某屏的第二頁，重開到那屏的第一頁。
            spreadsKey_.clear();
            updateSpreads();
            int cur = currentSpread();
            if (cur >= 0 && spreads_[static_cast<size_t>(cur)].first != nav_.index()) openPath(nav_.at(spreads_[static_cast<size_t>(cur)].first));
            else { loadSecond(); fit_ = true; fitToView(); updateTitle(); repaint(); }
        }
        break;
    case IDM_PREV: goPrev(); break;
    case IDM_NEXT: goNext(); break;
    case IDM_AUTONEXT:
        settings_.autoNextBook = !settings_.autoNextBook;
        disarmJump();
        saveSettingsQuiet();
        updateTitle();
        break;
    case IDM_MARKUP: onButton(BTN_MARKUP); break;
    case IDM_INFO: onButton(BTN_INFO); break;
    case IDM_LANG_SYSTEM: selectLanguage(L""); break;
    case IDM_ABOUT: endTextEdit(true); showAboutDialog(hwnd_); break;
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

bool MainWindow::openPath(const std::wstring& path) {
    endTextEdit(false);
    closePopup();
    hideTip();
    disarmJump();
    hoverItem_ = 0;
    if (adjusting_) { closeColorAdjustPanel(); cancelAdjust(); }
    selected_ = 0;
    drag_ = Drag::None;
    bool isDir = isDirectoryPath(path);
    if (!isDir && (isArchiveExtension(extensionOf(path)) ||
                   (!isSupportedExtension(extensionOf(path)) && looksLikeArchive(path))))
        return openArchive(path);
    // 不是目前壓縮檔解出來的檔就離開壓縮檔模式（刪暫存）。
    bool inArchive = insideArchive(path);
    if (!inArchive) closeArchive();
    if (isDir) {
        // 右鍵資料夾「用 mpainter 開啟」：從第一張開始。沒有散圖但有書（子資料夾 / 壓縮檔）就開第一本。
        auto files = listImageFiles(path);
        if (!files.empty()) return openPath(files.front());
        auto books = listBooks(path);
        if (!books.empty()) return openPath(books.front());
        path_ = path;
        nav_ = FolderNav{};
        infoData_ = {};
        loadError_ = tr(L"This folder has no supported images");
        doc_.reset(nullptr);
        updateTitle();
        layout();
        repaint();
        return false;
    }
    LoadResult r = loadImage(path);
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
        loadError_ = fmt1(L"Cannot open: %s", r.error);
        doc_.reset(nullptr);
        updateTitle();
        layout();
        repaint();
        return false;
    }
    if (!settings_.askedDefault && !askPending_) {
        askPending_ = true;
        PostMessageW(hwnd_, WM_APP + 2, 0, 0);
    }
    loadError_.clear();
    doc_.reset(r.image);
    second_.reset();
    secondPath_.clear();
    if (viewMode_ == ViewMode::TwoPage) {
        updateSpreads();
        int cur = currentSpread();
        if (cur >= 0 && spreads_[static_cast<size_t>(cur)].first != nav_.index()) {
            // 開到的是某屏的第二頁：改開那屏的第一頁（它會再載入這一頁當第二頁）。
            return openPath(nav_.at(spreads_[static_cast<size_t>(cur)].first));
        }
        loadSecond();
    }
    infoData_.info = r.info;
    infoData_.width = r.image->width;
    infoData_.height = r.image->height;
    infoData_.dpiX = r.image->dpiX;
    infoData_.dpiY = r.image->dpiY;
    fit_ = true;
    fitToView();
    if (scrollToEnd_) {
        scrollToEnd_ = false;
        offset_.y = -1e9f;
        clampOffset();
    }
    updateTitle();
    layout();
    repaint();
    return true;
}

// ---------------------------------------------------------------------------
// 壓縮檔

bool MainWindow::insideArchive(const std::wstring& path) const {
    if (archiveDir_.empty() || path.size() <= archiveDir_.size()) return false;
    if (_wcsnicmp(path.c_str(), archiveDir_.c_str(), archiveDir_.size()) != 0) return false;
    wchar_t sep = path[archiveDir_.size()];
    return sep == L'\\' || sep == L'/';
}

std::wstring MainWindow::archiveNameOf(const std::wstring& path) const {
    if (!insideArchive(path)) return L"";
    for (size_t i = 0; i < archiveFiles_.size() && i < archiveNames_.size(); ++i)
        if (_wcsicmp(archiveFiles_[i].c_str(), path.c_str()) == 0) return archiveNames_[i];
    return fileNameOf(path);
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

bool MainWindow::openArchive(const std::wstring& path, std::wstring* failMsg) {
    // 從檔案總管直接開加密檔時主視窗還沒顯示；先顯示，密碼與進度對話框才拿得到焦點。
    showMainWindow(SW_SHOW);
    // 目前這本（可能也是壓縮檔）要等新的解成功才關：跳本失敗時留在原本那本。
    auto fail = [&](const std::wstring& msg) {
        if (failMsg) { *failMsg = msg; return false; }
        closeArchive();
        showMarkup_ = false;
        freehandMode_ = false;
        path_ = path;
        nav_ = FolderNav{};
        infoData_ = {};
        loadError_ = msg;
        doc_.reset(nullptr);
        second_.reset();
        secondPath_.clear();
        updateTitle();
        layout();
        repaint();
        return false;
    };
    if (!archiveLibraryLoaded()) {
        size_t slash = exePath_.find_last_of(L"\\/");
        std::wstring dll = (slash == std::wstring::npos ? L"" : exePath_.substr(0, slash + 1)) + L"7z.dll";
        if (!loadArchiveLibrary(dll))
            return fail(tr(L"7z.dll was not found next to mpainter.exe, so archives cannot be opened."));
    }
    if (tempDirStale_ && archivePath_.empty()) { tempDir_.release(); tempDirStale_ = false; }
    if (!tempDir_.valid() && !tempDir_.create(resolveUnzipRoot(settings_.unzipDir, exeDir())) &&
        !tempDir_.create(fallbackUnzipRoot()))
        return fail(tr(L"Cannot create the temp folder for unpacking archives."));
    std::wstring dest = tempDir_.path() + L"\\" + std::to_wstring(++archiveSeq_);
    ExtractOptions opt;
    ArchiveResult r = extractWithProgress(hwnd_, path, dest, opt);
    bool fromList = false;
    if (r.status == ArchiveStatus::NeedPassword && !settings_.passwords.empty()) {
        // 先把密碼列表試一輪，都不對才問。
        removeDirectoryRecursive(dest);
        bool cancelled = false;
        int hit = tryPasswordsWithProgress(hwnd_, path, settings_.passwords, cancelled);
        if (cancelled) r.status = ArchiveStatus::Cancelled;
        else if (hit >= 0) {
            opt.password = settings_.passwords[static_cast<size_t>(hit)];
            fromList = true;
            r = extractWithProgress(hwnd_, path, dest, opt);
        }
    }
    while (r.status == ArchiveStatus::NeedPassword || r.status == ArchiveStatus::WrongPassword) {
        removeDirectoryRecursive(dest);
        // 列表裡的密碼試過了不算使用者打錯，當第一次問。
        bool retry = r.status == ArchiveStatus::WrongPassword && !fromList;
        fromList = false;
        std::wstring pw;
        if (!showPasswordDialog(hwnd_, fileNameOf(path), retry, pw)) { r.status = ArchiveStatus::Cancelled; break; }
        opt.password = pw;
        r = extractWithProgress(hwnd_, path, dest, opt);
    }
    if (r.status != ArchiveStatus::Ok && r.status != ArchiveStatus::Incomplete) {
        removeDirectoryRecursive(dest);
        switch (r.status) {
        case ArchiveStatus::Cancelled: return fail(L"");
        case ArchiveStatus::NoImages: return fail(tr(L"This archive has no supported images"));
        case ArchiveStatus::TooLarge: return fail(tr(L"The images in this archive are too large to unpack (limit 8 GB)."));
        case ArchiveStatus::NoSpace: return fail(tr(L"Not enough free disk space in the temp folder to unpack this archive."));
        case ArchiveStatus::NotArchive: return fail(fmt1(L"Cannot open: %s", r.error.empty() ? tr(L"not a supported archive") : r.error));
        default: return fail(fmt1(L"Cannot unpack: %s", r.error));
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
    freehandMode_ = false;
    archivePath_ = path;
    archiveDir_ = dest;
    archiveFiles_ = r.files;
    archiveNames_ = r.names;
    return openPath(r.files.front());
}

bool MainWindow::confirmDiscard() {
    endTextEdit(true);
    if (!doc_.image() || !doc_.dirty() || readOnly()) return true;
    wchar_t text[MAX_PATH + 128];
    swprintf(text, MAX_PATH + 128, tr(L"“%s” has unsaved changes. Save them?"), fileNameOf(path_).c_str());
    int r = MessageBoxW(hwnd_, text, kAppName, MB_YESNOCANCEL | MB_ICONQUESTION);
    if (r == IDCANCEL) return false;
    if (r == IDYES) return save();
    return true;
}

bool MainWindow::save() {
    if (!doc_.image()) return false;
    if (!editGuard()) return false;
    endTextEdit(true);
    if (adjusting_) { closeColorAdjustPanel(); cancelAdjust(); }
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
    Image flat = renderer_.flatten(doc_.state());
    std::wstring err;
    SetCursor(LoadCursorW(nullptr, IDC_WAIT));
    SaveOptions opt;
    opt.metadataSource = loadedFrom_; // 全部照抄，只改與像素不符的欄位
    if (!saveImage(flat, target, &err, opt)) {
        MessageBoxW(hwnd_, fmt1(L"Save failed: %s", err).c_str(), kAppName, MB_ICONERROR);
        return false;
    }
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
    ofn.lpstrFilter = L"PNG (*.png)\0*.png\0JPEG (*.jpg)\0*.jpg;*.jpeg\0BMP (*.bmp)\0*.bmp\0TIFF (*.tif)\0*.tif;*.tiff\0WebP (*.webp)\0*.webp\0";
    std::wstring ext = extensionOf(path_);
    ofn.nFilterIndex = ext == L".jpg" || ext == L".jpeg" ? 2 : ext == L".bmp" ? 3 : ext == L".tif" || ext == L".tiff" ? 4 : ext == L".webp" ? 5 : 1;
    ofn.lpstrFile = buf;
    ofn.nMaxFile = MAX_PATH;
    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    ofn.lpstrDefExt = L"png";
    if (!GetSaveFileNameW(&ofn)) return false;
    std::wstring newPath = buf;
    if (!canSaveExtension(extensionOf(newPath))) {
        static const wchar_t* exts[] = {L".png", L".jpg", L".bmp", L".tif", L".webp"};
        newPath += exts[std::clamp<int>(static_cast<int>(ofn.nFilterIndex) - 1, 0, 4)];
    }
    // 跨格式：目標裝不下的 metadata 區塊先列出來問。
    if (settings_.warnMetadataLoss) {
        auto lost = metadataLossOnSave(infoData_.info, extensionOf(loadedFrom_), extensionOf(newPath));
        if (!lost.empty()) {
            std::wstring list;
            for (size_t i = 0; i < lost.size(); ++i) list += (i ? L", " : L"") + lost[i];
            std::wstring extUpper = extensionOf(newPath).substr(1);
            for (auto& ch : extUpper) ch = static_cast<wchar_t>(towupper(ch));
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
            if (dontAsk) { settings_.warnMetadataLoss = false; saveSettingsQuiet(); updateTitle(); }
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
    path_ = newPath;
    if (!save()) return false;
    nav_.load(path_);
    fillFileFacts(infoData_, path_);
    updateTitle();
    repaint();
    return true;
}

bool MainWindow::exportAs() {
    if (!doc_.image()) return false;
    endTextEdit(true);
    std::wstring newPath;
    if (!pickSavePath(newPath, true)) return false;
    return writeFlattened(newPath); // 目前編輯的還是原檔，dirty 不變
}

void MainWindow::openDialog() {
    if (!confirmDiscard()) return;
    wchar_t buf[MAX_PATH] = L"";
    OPENFILENAMEW ofn{sizeof(ofn)};
    ofn.hwndOwner = hwnd_;
    // 篩選字串以 \0 分段，不能整串進翻譯表，分段組。
    std::wstring filter = trs(L"Image files");
    filter += L'\0';
    filter += L"*.png;*.jpg;*.jpeg;*.jpe;*.jfif;*.bmp;*.dib;*.tif;*.tiff;*.webp;*.zip;*.7z;*.rar;*.cbz;*.cb7;*.cbr";
    filter += L'\0';
    filter += trs(L"Archives (zip, 7z, rar, cbz, cb7, cbr)");
    filter += L'\0';
    filter += L"*.zip;*.7z;*.rar;*.cbz;*.cb7;*.cbr";
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

void MainWindow::step(int dir) {
    if (armedDir_ && armedDir_ != dir) disarmJump();
    int target = -1;
    if (twoPage()) {
        int cur = currentSpread();
        if (cur < 0) return;
        int n = static_cast<int>(spreads_.size());
        if (dir > 0 ? cur + 1 < n : cur > 0) target = spreads_[static_cast<size_t>(cur + dir)].first;
    } else if (nav_.index() >= 0) {
        if (dir > 0 ? nav_.hasNext() : nav_.hasPrev()) target = nav_.index() + dir;
    }
    if (target < 0) { bookEdge(dir); return; }
    if (!confirmDiscard()) return;
    openPath(nav_.at(target));
}

// 翻到底。工作模式（沒開「接著看下一本」）只說到底了：在「備選」資料夾挑圖的人不會想被帶到隔壁的「棄選」。
// 看漫畫模式第一次按先提示要去哪一本，3 秒內再按同方向才跳；跳過去再提示一次進了哪本。
void MainWindow::bookEdge(int dir) {
    if (nav_.count() == 0) return;
    if (!settings_.autoNextBook) {
        showToast(dir > 0 ? tr(L"This is the last image") : tr(L"This is the first image"), false);
        return;
    }
    if (armedDir_ == dir && !armedTarget_.empty()) {
        std::wstring target = armedTarget_;
        disarmJump();
        if (confirmDiscard()) openBook(target, dir);
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
    showToast(dir > 0 ? fmt2(L"%s is finished. Press Next again to start %s", fileNameOf(book), fileNameOf(target))
                      : fmt2(L"This is the first page of %s. Press Previous again to go back to the end of %s",
                             fileNameOf(book), fileNameOf(target)), true);
}

bool MainWindow::openBook(const std::wstring& book, int dir) {
    // 失敗（密碼對話框取消、解壓失敗、沒有圖）就留在原本這本，說一聲為什麼；再按翻頁會重新走「提示 → 再按一次」。
    std::wstring from = fileNameOf(currentBook());
    bool ok = false;
    if (isDirectoryPath(book)) {
        auto files = listImageFiles(book);
        movePinnedFirst(files, settings_.pinnedNames);
        if (files.empty()) { showToast(fmt2(L"Could not open %s: %s", fileNameOf(book), tr(L"This folder has no supported images")), false); return false; }
        ok = openPath(dir > 0 ? files.front() : files.back());
        if (!ok) showToast(fmt2(L"Could not open %s: %s", fileNameOf(book), loadError_), false);
    } else {
        std::wstring err;
        ok = openArchive(book, &err);
        if (!ok) {
            showToast(err.empty() ? fmt1(L"Cancelled. Still in %s", from) : fmt2(L"Could not open %s: %s", fileNameOf(book), err), false);
            return false;
        }
        if (dir < 0 && !archiveFiles_.empty()) ok = openPath(archiveFiles_.back());
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

void MainWindow::showToast(const std::wstring& text, bool sticky) {
    toast_ = text;
    toastSticky_ = sticky;
    KillTimer(hwnd_, kToastTimer);
    if (!sticky) SetTimer(hwnd_, kToastTimer, kToastMs, nullptr);
    repaint();
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
    renderer_.drawUiText(rt, toast_, textRc, size, D2D1::ColorF(D2D1::ColorF::White), DWRITE_TEXT_ALIGNMENT_CENTER, true,
                         DWRITE_PARAGRAPH_ALIGNMENT_CENTER, false, true);
}

// ---------------------------------------------------------------------------
// 兩頁並排

void MainWindow::updateSpreads() {
    const auto& files = nav_.files();
    std::wstring key = (files.empty() ? L"" : files.front()) + L"|" + std::to_wstring(files.size()) +
                       (settings_.firstPageAlone ? L"|A" : L"|a") + (shiftOne_ ? L"S" : L"s");
    if (key == spreadsKey_) return;
    std::vector<bool> wide;
    wide.reserve(files.size());
    for (const auto& f : files) {
        auto it = wideCache_.find(f);
        if (it == wideCache_.end()) {
            int w = 0, h = 0;
            bool isWide = readImageSize(f, w, h) && w > h;
            it = wideCache_.emplace(f, isWide).first;
        }
        wide.push_back(it->second);
    }
    spreads_ = buildSpreads(wide, settings_.firstPageAlone, shiftOne_);
    spreadsKey_ = key;
}

int MainWindow::currentSpread() const { return spreadIndexOf(spreads_, nav_.index()); }

void MainWindow::loadSecond() {
    int cur = currentSpread();
    std::wstring path = cur >= 0 ? nav_.at(spreads_[static_cast<size_t>(cur)].second) : L"";
    if (path == secondPath_ && (second_ || path.empty())) return;
    second_.reset();
    secondPath_ = path;
    if (path.empty()) return;
    LoadResult r = loadImage(path);
    if (r.image) second_ = r.image;
}

float MainWindow::viewW() const {
    auto img = doc_.image();
    if (!img) return 0;
    if (twoPage() && second_) return static_cast<float>(img->width + second_->width);
    return static_cast<float>(img->width);
}

float MainWindow::viewH() const {
    auto img = doc_.image();
    if (!img) return 0;
    if (twoPage() && second_) return static_cast<float>(std::max(img->height, second_->height));
    return static_cast<float>(img->height);
}

Rc MainWindow::pageRect(bool second) const {
    auto img = doc_.image();
    if (!img) return {};
    if (!(twoPage() && second_)) return {0, 0, static_cast<float>(img->width), static_cast<float>(img->height)};
    float h = viewH();
    float w1 = static_cast<float>(img->width), w2 = static_cast<float>(second_->width);
    // 閱讀順序左到右：第一頁在左；右到左（日漫）：第一頁在右。底對齊。
    bool firstOnLeft = !settings_.rightToLeft;
    float x1 = firstOnLeft ? 0 : w2, x2 = firstOnLeft ? w1 : 0;
    if (!second) return {x1, h - img->height, x1 + w1, h};
    return {x2, h - second_->height, x2 + w2, h};
}

void MainWindow::updateTitle() {
    std::wstring t;
    if (!path_.empty()) {
        t = displayName(path_);
        if (doc_.dirty()) t += L"*";
        if (twoPage() && second_) {
            // 依畫面上左右順序列出兩個檔名，頁碼寫成 1-2。
            std::wstring a = displayName(path_), b = displayName(secondPath_);
            t = settings_.rightToLeft ? b + L" | " + a : a + L" | " + b;
            if (doc_.dirty()) t += L"*";
            int cur = currentSpread();
            if (cur >= 0 && nav_.count() > 1) {
                const Spread& s = spreads_[static_cast<size_t>(cur)];
                t += L"  (" + std::to_wstring(s.first + 1) + L"-" + std::to_wstring(s.second + 1) + L"/" + std::to_wstring(nav_.count()) + L")";
            }
        } else if (nav_.count() > 1 && nav_.index() >= 0) {
            t += L"  (" + std::to_wstring(nav_.index() + 1) + L"/" + std::to_wstring(nav_.count()) + L")";
        }
        t += L" - ";
    }
    // 目前的模式；有臨時調整（選單改過、和模式本體不同）就加 *
    if (!settings_.profileName.empty()) t += settings_.profileName + (profileModified(settings_) ? L"* - " : L" - ");
    t += kAppName;
    SetWindowTextW(hwnd_, t.c_str());
}

// ---------------------------------------------------------------------------
// 檢視

ImagePtr MainWindow::currentImage() const {
    if (adjusting_ && adjustPreview_) return adjustPreview_;
    return doc_.image();
}

POINT MainWindow::canvasCenter() const {
    return {static_cast<LONG>((canvasRc_.left + canvasRc_.right) / 2), static_cast<LONG>((canvasRc_.top + canvasRc_.bottom) / 2)};
}

MainWindow::ViewMode MainWindow::viewModeFromString(const std::wstring& s) {
    if (s == L"fitwidth") return ViewMode::FitWidth;
    if (s == L"fitheight") return ViewMode::FitHeight;
    if (s == L"actual") return ViewMode::Actual;
    if (s == L"twopage") return ViewMode::TwoPage;
    return ViewMode::FitWindow;
}

const wchar_t* MainWindow::viewModeToString(ViewMode m) {
    switch (m) {
    case ViewMode::FitWidth: return L"fitwidth";
    case ViewMode::FitHeight: return L"fitheight";
    case ViewMode::Actual: return L"actual";
    case ViewMode::TwoPage: return L"twopage";
    default: return L"fitwindow";
    }
}

void MainWindow::setViewMode(ViewMode m) {
    bool wasTwo = viewMode_ == ViewMode::TwoPage;
    viewMode_ = m;
    settings_.viewMode = viewModeToString(m);
    saveSettingsQuiet();
    if (m == ViewMode::TwoPage) {
        // 只看不編：收掉編輯狀態與標示工具列。
        endTextEdit(true);
        if (adjusting_) { closeColorAdjustPanel(); cancelAdjust(); }
        closePopup();
        selected_ = 0;
        freehandMode_ = false;
        pickingWhite_ = false;
        showMarkup_ = false;
        layout();
        if (doc_.image()) {
            updateSpreads();
            int cur = currentSpread();
            if (cur >= 0 && spreads_[static_cast<size_t>(cur)].first != nav_.index()) { openPath(nav_.at(spreads_[static_cast<size_t>(cur)].first)); return; }
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
}

// 依檢視模式算 zoom。符合類模式預設不放大小圖，開了「小圖放大」才放大；1:1 永遠不放大。
void MainWindow::fitToView(bool resetOffset) {
    auto img = doc_.image();
    if (!img) return;
    float cw = canvasRc_.right - canvasRc_.left, ch = canvasRc_.bottom - canvasRc_.top;
    float vw = viewW(), vh = viewH();
    float z;
    switch (viewMode_) {
    case ViewMode::FitWidth: z = cw / vw; break;
    case ViewMode::FitHeight: z = ch / vh; break;
    case ViewMode::TwoPage: z = std::min(cw / vw, ch / vh); break; // 兩頁並排：符合高度，兩頁都放不下時再縮
    case ViewMode::Actual: z = 1.0f; break;
    default: z = std::min(cw / vw, ch / vh); break;
    }
    if (!settings_.upscaleSmall || viewMode_ == ViewMode::Actual) z = std::min(1.0f, z);
    zoom_ = std::max(0.01f, z);
    if (resetOffset) offset_ = {0, 0}; // 從左上開始，放得下的方向由 clampOffset 置中
    clampOffset();
    buildButtons();
}

void MainWindow::clampOffset() {
    auto img = doc_.image();
    if (!img) return;
    float cw = canvasRc_.right - canvasRc_.left, ch = canvasRc_.bottom - canvasRc_.top;
    float iw = viewW() * zoom_, ih = viewH() * zoom_;
    if (iw <= cw) offset_.x = (cw - iw) / 2;
    else offset_.x = std::clamp(offset_.x, cw - iw, 0.0f);
    if (ih <= ch) offset_.y = (ch - ih) / 2;
    else offset_.y = std::clamp(offset_.y, ch - ih, 0.0f);
    // 對齊整數像素：高品質縮放的點陣圖才會 1:1 貼上，不被 bilinear 再糊一次。
    offset_.x = std::round(offset_.x);
    offset_.y = std::round(offset_.y);
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
    if (!settings_.scrollFlip) return;
    if (down) goNext();
    else { scrollToEnd_ = true; goPrev(); scrollToEnd_ = false; }
}

void MainWindow::setZoom(float z, POINT anchor) {
    if (!doc_.image()) return;
    z = std::clamp(z, 0.02f, 64.0f);
    Pt ip = toImage(anchor);
    zoom_ = z;
    offset_.x = (anchor.x - canvasRc_.left) - ip.x * zoom_;
    offset_.y = (anchor.y - canvasRc_.top) - ip.y * zoom_;
    fit_ = false;
    clampOffset();
    buildButtons();
    positionEdit();
    // 連續縮放時先用快的畫，停下來再算高品質。
    if (settings_.highQuality) { hqBusy_ = true; SetTimer(hwnd_, kHqTimer, kHqDelayMs, nullptr); }
    repaint();
}

void MainWindow::zoomStep(int dir) {
    float f = dir > 0 ? 1.25f : 1.0f / 1.25f;
    setZoom(zoom_ * f, canvasCenter());
}

Pt MainWindow::toImage(POINT c) const {
    return {(c.x - canvasRc_.left - offset_.x) / zoom_, (c.y - canvasRc_.top - offset_.y) / zoom_};
}

POINT MainWindow::toClient(Pt p) const {
    return {static_cast<LONG>(std::lround(canvasRc_.left + offset_.x + p.x * zoom_)),
            static_cast<LONG>(std::lround(canvasRc_.top + offset_.y + p.y * zoom_))};
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
            infoData_.dpiX = img->dpiX;
            infoData_.dpiY = img->dpiY;
        }
        drawInfoPanel(renderer_, rt, infoRc_, infoData_, infoState_, ui_);
    }
    HRESULT hr = rt->EndDraw();
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
        name = settings_.rightToLeft ? b + L"   |   " + a : a + L"   |   " + b;
    }
    renderer_.drawUiText(rt, name, nameRc_, px(13), kIcon, DWRITE_TEXT_ALIGNMENT_CENTER);
    for (const auto& b : buttons_) drawButton(b);
}

void MainWindow::drawButton(const Button& b) {
    auto* rt = renderer_.rt();
    float r = px(5);
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
        renderer_.drawUiText(rt, L"1:1", b.rect, px(12), c, DWRITE_TEXT_ALIGNMENT_CENTER, true);
        break;
    case BTN_ROTATE: {
        // 逆時針的弧加箭頭
        ComPtr<ID2D1PathGeometry> geo;
        renderer_.d2d()->CreatePathGeometry(&geo);
        ComPtr<ID2D1GeometrySink> sink;
        geo->Open(&sink);
        float r = px(7);
        sink->BeginFigure(P(cx + r * 0.0f, cy - r), D2D1_FIGURE_BEGIN_HOLLOW);
        sink->AddArc(D2D1::ArcSegment(P(cx - r * 0.7f, cy + r * 0.7f), D2D1::SizeF(r, r), 0, D2D1_SWEEP_DIRECTION_COUNTER_CLOCKWISE, D2D1_ARC_SIZE_LARGE));
        sink->EndFigure(D2D1_FIGURE_END_OPEN);
        sink->Close();
        rt->DrawGeometry(geo.Get(), br, s);
        rt->DrawLine(P(cx, cy - r), P(cx - px(4), cy - r - px(3)), br, s);
        rt->DrawLine(P(cx, cy - r), P(cx - px(4), cy - r + px(3)), br, s);
        break;
    }
    case BTN_MARKUP: {
        // 筆頭
        ComPtr<ID2D1PathGeometry> geo;
        renderer_.d2d()->CreatePathGeometry(&geo);
        ComPtr<ID2D1GeometrySink> sink;
        geo->Open(&sink);
        sink->BeginFigure(P(cx + px(6), cy - px(7)), D2D1_FIGURE_BEGIN_FILLED);
        sink->AddLine(P(cx + px(8), cy - px(5)));
        sink->AddLine(P(cx - px(4), cy + px(7)));
        sink->AddLine(P(cx - px(8), cy + px(8)));
        sink->AddLine(P(cx - px(7), cy + px(4)));
        sink->EndFigure(D2D1_FIGURE_END_CLOSED);
        sink->Close();
        rt->DrawGeometry(geo.Get(), br, s);
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
        ComPtr<ID2D1PathGeometry> geo;
        renderer_.d2d()->CreatePathGeometry(&geo);
        ComPtr<ID2D1GeometrySink> sink;
        geo->Open(&sink);
        sink->BeginFigure(P(cx - px(9), cy + px(5)), D2D1_FIGURE_BEGIN_HOLLOW);
        sink->AddBezier(D2D1::BezierSegment(P(cx - px(6), cy - px(12)), P(cx - px(2), cy + px(12)), P(cx + px(1), cy - px(2))));
        sink->AddBezier(D2D1::BezierSegment(P(cx + px(3), cy - px(10)), P(cx + px(5), cy + px(8)), P(cx + px(9), cy - px(4))));
        sink->EndFigure(D2D1_FIGURE_END_OPEN);
        sink->Close();
        rt->DrawGeometry(geo.Get(), br, s);
        break;
    }
    case BTN_SHAPES:
        rt->DrawRectangle(D2D1::RectF(cx - px(9), cy - px(7), cx - px(1), cy + px(3)), br, s);
        rt->DrawEllipse(D2D1::Ellipse(P(cx + px(4), cy + px(1)), px(5), px(5)), br, s);
        break;
    case BTN_TEXT:
        renderer_.drawUiText(rt, L"A", b.rect, px(18), c, DWRITE_TEXT_ALIGNMENT_CENTER, false);
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
        Color sc = currentStroke().stroke;
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
        Color fc = currentStroke().fill;
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
        renderer_.drawUiText(rt, L"Aa", D2D1::RectF(b.rect.left, b.rect.top, b.rect.right - px(5), b.rect.bottom), px(14), c,
                             DWRITE_TEXT_ALIGNMENT_CENTER, true);
        break;
    default: break;
    }
}

const std::vector<Annotation>& MainWindow::visibleItems() const {
    return drag_ == Drag::Move || drag_ == Drag::Handle ? liveItems_ : doc_.items();
}

void MainWindow::drawCanvas() {
    auto* rt = renderer_.rt();
    rt->SetTransform(D2D1::Matrix3x2F::Identity());
    rt->FillRectangle(canvasRc_, renderer_.brush(kCanvasBg));
    auto img = currentImage();
    if (!img) {
        if (!loadError_.empty())
            renderer_.drawUiText(rt, loadError_, canvasRc_, px(14), kIcon, DWRITE_TEXT_ALIGNMENT_CENTER);
        return;
    }
    rt->PushAxisAlignedClip(canvasRc_, D2D1_ANTIALIAS_MODE_ALIASED);
    // 高品質縮放：不在連續縮放中、不在調色預覽中、倍率不是 1 時，用 Lanczos3 先縮好的點陣圖。
    auto bitmapFor = [&](const ImagePtr& im) {
        const ImageBitmap* b = nullptr;
        if (settings_.highQuality && !hqBusy_ && !adjusting_ && std::abs(zoom_ - 1.0f) > 1e-4f) {
            int tw = std::max(1, static_cast<int>(std::lround(im->width * zoom_)));
            int th = std::max(1, static_cast<int>(std::lround(im->height * zoom_)));
            b = renderer_.scaledBitmap(im, tw, th);
        }
        if (!b) b = renderer_.imageBitmap(im);
        return b;
    };
    auto shadow = [&](const Rc& r) {
        rt->SetTransform(D2D1::Matrix3x2F::Identity());
        POINT tl = toClient({r.left, r.top}), brp = toClient({r.right, r.bottom});
        rt->DrawRectangle(D2D1::RectF(tl.x - 0.5f, tl.y - 0.5f, brp.x + 0.5f, brp.y + 0.5f), renderer_.brush(D2D1::ColorF(0, 0, 0, 0.25f)), 1.0f);
    };
    const ImageBitmap* bmp = bitmapFor(img);
    DrawCtx ctx{rt, img.get(), bmp, &renderer_.mosaicCache()};
    // 兩頁並排：第二頁先畫（它不帶標註），再畫第一頁與它的標註。第一頁的位置由 pageRect 決定。
    Rc r1 = pageRect(false);
    if (twoPage() && second_) {
        Rc r2 = pageRect(true);
        shadow(r2);
        rt->SetTransform(D2D1::Matrix3x2F::Translation(r2.left, r2.top) * viewMatrix());
        DrawCtx ctx2{rt, second_.get(), bitmapFor(second_), nullptr};
        renderer_.drawImage(ctx2);
    }
    shadow(r1);
    rt->SetTransform(D2D1::Matrix3x2F::Translation(r1.left, r1.top) * viewMatrix());
    renderer_.drawImage(ctx);
    const auto& items = visibleItems();
    for (const auto& a : items) {
        if (a.id == editingId_) {
            Annotation copy = a;
            copy.textValue.clear();
            renderer_.drawAnnotation(ctx, copy);
        } else {
            renderer_.drawAnnotation(ctx, a);
        }
    }
    if (drag_ == Drag::Draw && !drawPath_.empty()) renderer_.drawAnnotation(ctx, makeFreehand(drawPath_, stroke_));
    rt->SetTransform(D2D1::Matrix3x2F::Identity());
    drawSelection();
    rt->PopAxisAlignedClip();
}

void MainWindow::drawSelection() {
    const Annotation* a = selectedItem();
    if (!a || drag_ == Drag::Draw) return;
    auto* rt = renderer_.rt();
    float r = px(kHandleR);
    if (drag_ == Drag::Handle) {
        // 綠點按下時畫它的可動軌跡：繞圈的畫圈，直線的畫線。
        HandleGuide g = handleGuide(*a, dragHandle_);
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
    switch (id) {
    case BTN_ZOOMOUT: zoomStep(-1); break;
    case BTN_ZOOM100: setViewMode(ViewMode::Actual); break;
    case BTN_ZOOMIN: zoomStep(1); break;
    case BTN_ROTATE: rotateImage(); break;
    case BTN_MARKUP:
        if (!editGuard()) break;
        showMarkup_ = !showMarkup_;
        if (!showMarkup_) freehandMode_ = false;
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
        freehandMode_ = !freehandMode_;
        if (freehandMode_) selected_ = 0;
        buildButtons();
        repaint();
        break;
    case BTN_SHAPES:
        showShapesPopup(hwnd_, renderer_, anchor(), ui_, [this](Kind k) { addShape(k); });
        break;
    case BTN_TEXT: addText(); break;
    case BTN_ADJCOLOR:
        if (!editGuard()) break;
        if (!adjusting_ && doc_.image()) {
            endTextEdit(true);
            adjusting_ = true;
            adjustBase_ = doc_.image();
            adjustParams_ = {};
            adjustPreview_.reset();
            buildButtons();
            showColorAdjustPanel(hwnd_, this, ui_);
            repaint();
        }
        break;
    case BTN_ADJSIZE: adjustSize(); break;
    case BTN_LINESTYLE:
        showLineStylePopup(hwnd_, renderer_, anchor(), ui_, currentStroke(), [this](const StrokeStyle& s) { applyStrokeStyle(s); });
        break;
    case BTN_LINECOLOR:
        showColorPopup(hwnd_, renderer_, anchor(), ui_, currentStroke().stroke, true, [this](Color c) {
            StrokeStyle s = currentStroke();
            s.stroke = c;
            applyStrokeStyle(s);
        });
        break;
    case BTN_FILL:
        showColorPopup(hwnd_, renderer_, anchor(), ui_, currentStroke().fill, true, [this](Color c) {
            StrokeStyle s = currentStroke();
            s.fill = c;
            applyStrokeStyle(s);
        });
        break;
    case BTN_FORMAT:
        showFormatPopup(hwnd_, renderer_, anchor(), ui_, currentText(), [this](const TextStyle& t) { applyTextStyle(t); });
        break;
    default: break;
    }
}

// ---------------------------------------------------------------------------
// 滑鼠

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

    Pt ip = toImage(pt);
    if (twoPage()) {
        dragStartClient_ = pt;
        dragMoved_ = false;
        drag_ = Drag::Pan;
        dragOffsetStart_ = offset_;
        SetCapture(hwnd_);
        return;
    }
    if (pickingWhite_ && adjustBase_) {
        pickingWhite_ = false;
        setWhiteBalanceFrom(adjustParams_, *adjustBase_, static_cast<int>(ip.x), static_cast<int>(ip.y));
        colorAdjustPanelSetParams(adjustParams_);
        previewChanged(adjustParams_);
        return;
    }
    dragStartImg_ = ip;
    dragStartClient_ = pt;
    dragMoved_ = false;
    if (freehandMode_) {
        drag_ = Drag::Draw;
        drawPath_ = {ip};
        SetCapture(hwnd_);
        return;
    }
    // 控制點
    if (const Annotation* sel = selectedItem()) {
        for (const auto& h : handles(*sel)) {
            POINT c = toClient(h.pos);
            if (std::hypot(static_cast<float>(c.x - pt.x), static_cast<float>(c.y - pt.y)) <= px(kHandleR + 3)) {
                drag_ = Drag::Handle;
                dragHandle_ = h.id;
                liveItems_ = doc_.items();
                SetCapture(hwnd_);
                return;
            }
        }
    }
    // 物件（上層優先）
    const auto& items = doc_.items();
    for (int i = static_cast<int>(items.size()) - 1; i >= 0; --i) {
        if (hitTest(items[i], ip, imageTol())) {
            select(items[i].id);
            drag_ = Drag::Move;
            liveItems_ = items;
            SetCapture(hwnd_);
            return;
        }
    }
    if (selected_) { selected_ = 0; repaint(); }
    drag_ = Drag::Pan;
    dragOffsetStart_ = offset_;
    SetCapture(hwnd_);
}

void MainWindow::onLButtonDblClk(POINT pt) {
    if (!inRect(canvasRc_, pt) || !doc_.image() || freehandMode_ || twoPage()) { onLButtonDown(pt); return; }
    Pt ip = toImage(pt);
    const auto& items = doc_.items();
    for (int i = static_cast<int>(items.size()) - 1; i >= 0; --i) {
        if (items[i].kind == Kind::Text && hitTest(items[i], ip, imageTol())) {
            select(items[i].id);
            beginTextEdit(items[i].id, false);
            return;
        }
    }
    onLButtonDown(pt);
}

void MainWindow::onMouseMove(POINT pt, WPARAM) {
    if (!trackingMouse_) {
        TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, hwnd_, 0};
        TrackMouseEvent(&tme);
        trackingMouse_ = true;
    }
    if (drag_ == Drag::None) {
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
        uint32_t item = (edit_ || freehandMode_ || pickingWhite_ || twoPage()) ? 0 : hitItemBox(pt);
        if (item != hoverItem_) {
            hoverItem_ = item;
            hideTip();
        }
        if (item && !tipVisible_ && item != tipShownFor_ && settings_.tipShown < kTipMaxShown) SetTimer(hwnd_, kTipTimer, kTipDelayMs, nullptr);
        else KillTimer(hwnd_, kTipTimer);
        return;
    }
    Pt ip = toImage(pt);
    if (std::abs(pt.x - dragStartClient_.x) > 2 || std::abs(pt.y - dragStartClient_.y) > 2) dragMoved_ = true;
    switch (drag_) {
    case Drag::Pan:
        offset_ = dragOffsetStart_ + Pt{static_cast<float>(pt.x - dragStartClient_.x), static_cast<float>(pt.y - dragStartClient_.y)};
        clampOffset();
        positionEdit();
        break;
    case Drag::Draw:
        if (drawPath_.empty() || dist(drawPath_.back(), ip) >= 0.5f / zoom_) drawPath_.push_back(ip);
        break;
    case Drag::Move: {
        int i = doc_.indexOf(selected_);
        if (i >= 0) {
            liveItems_[i] = doc_.items()[i];
            moveBy(liveItems_[i], ip.x - dragStartImg_.x, ip.y - dragStartImg_.y);
        }
        break;
    }
    case Drag::Handle: {
        int i = doc_.indexOf(selected_);
        if (i >= 0) {
            dragHandle(liveItems_[i], dragHandle_, ip);
            if (liveItems_[i].kind == Kind::Text)
                liveItems_[i].rect.bottom = liveItems_[i].rect.top + renderer_.textHeight(liveItems_[i]);
        }
        break;
    }
    default: break;
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
    if (drag_ == Drag::None) return;
    ReleaseCapture();
    finishDrag();
}

void MainWindow::finishDrag() {
    Drag d = drag_;
    drag_ = Drag::None;
    switch (d) {
    case Drag::Draw: {
        if (!drawPath_.empty()) {
            Annotation a = makeFreehand(drawPath_, stroke_);
            a.id = doc_.nextId();
            auto items = doc_.items();
            items.push_back(a);
            doc_.commitItems(std::move(items));
            updateTitle();
        }
        drawPath_.clear();
        break;
    }
    case Drag::Move:
    case Drag::Handle:
        if (dragMoved_) {
            doc_.commitItems(liveItems_);
            updateTitle();
        }
        liveItems_.clear();
        break;
    default: break;
    }
    repaint();
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
    if (pickingWhite_) { SetCursor(LoadCursorW(nullptr, IDC_CROSS)); return true; }
    if (freehandMode_ || drag_ == Drag::Draw) { SetCursor(LoadCursorW(nullptr, IDC_CROSS)); return true; }
    if (drag_ == Drag::Pan) { SetCursor(LoadCursorW(nullptr, IDC_SIZEALL)); return true; }
    if (drag_ == Drag::Move || drag_ == Drag::Handle) { SetCursor(LoadCursorW(nullptr, IDC_SIZEALL)); return true; }
    if (const Annotation* sel = selectedItem()) {
        for (const auto& h : handles(*sel)) {
            POINT c = toClient(h.pos);
            if (std::hypot(static_cast<float>(c.x - pt.x), static_cast<float>(c.y - pt.y)) <= px(kHandleR + 3)) {
                SetCursor(LoadCursorW(nullptr, IDC_SIZEALL));
                return true;
            }
        }
    }
    // 物件的外框方框內：手指，告訴使用者這裡是物件。
    if (!edit_ && !twoPage() && hitItemBox(pt)) { SetCursor(LoadCursorW(nullptr, IDC_HAND)); return true; }
    return false;
}

uint32_t MainWindow::hitItemBox(POINT pt) const {
    if (!inRect(canvasRc_, pt) || !doc_.image()) return 0;
    Pt ip = toImage(pt);
    const auto& items = doc_.items();
    for (int i = static_cast<int>(items.size()) - 1; i >= 0; --i)
        if (bounds(items[i]).contains(ip)) return items[i].id;
    return 0;
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
    default: return L"";
    }
}

void MainWindow::showButtonTip(int id) {
    const Button* b = nullptr;
    for (const auto& x : buttons_) if (x.id == id) b = &x;
    if (!b) return;
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
    loadSettings(settingsPath_, settings_);
    nav_.setPinnedNames(settings_.pinnedNames);
    settingsWritable_ = isPathWritable(settingsPath_);
    viewMode_ = viewModeFromString(settings_.viewMode);
    initLanguage(); // 要在登錄之前：右鍵選單的文字跟語系走
    // 模式清單：第一次執行（或舊版 ini）補內建的兩個。名字要翻譯，所以在 initLanguage 之後。
    if (ensureProfiles(settings_, tr(L"General mode"), tr(L"Comic mode"))) saveSettingsQuiet();
    // portable：exe 搬了位置就把登錄改成現在的路徑。
    if (settings_.contextMenu && _wcsicmp(contextMenuExePath().c_str(), exePath_.c_str()) != 0) registerContextMenu(exePath_);
    std::wstring prog = progIdExePath();
    if (!prog.empty() && _wcsicmp(prog.c_str(), exePath_.c_str()) != 0) registerProgId(exePath_);
}

void MainWindow::saveSettingsQuiet() {
    // 寫不進去（唯讀位置）就算了，本次執行內的狀態還在。
    settingsWritable_ = saveSettings(settingsPath_, settings_);
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
    // 重開自己並開回目前的檔案。
    std::wstring args = path_.empty() ? L"" : L"\"" + path_ + L"\"";
    ShellExecuteW(nullptr, L"open", exePath_.c_str(), args.c_str(), nullptr, SW_SHOWNORMAL);
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
    registerProgId(exePath_);
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
    if (s.contextMenu != settings_.contextMenu) {
        if (s.contextMenu) registerContextMenu(exePath_);
        else unregisterContextMenu();
    }
    bool pinnedChanged = s.pinnedNames != settings_.pinnedNames;
    bool unzipChanged = _wcsicmp(resolveUnzipRoot(s.unzipDir, exeDir()).c_str(), resolveUnzipRoot(settings_.unzipDir, exeDir()).c_str()) != 0;
    pendingLang = s.language;
    // 只抄面板管的欄位；目前的檢視值不是面板改的
    settings_.contextMenu = s.contextMenu;
    settings_.askedDefault = s.askedDefault;
    settings_.tipShown = s.tipShown;
    settings_.unzipDir = s.unzipDir;
    settings_.pinnedNames = s.pinnedNames;
    settings_.passwords = s.passwords;
    // 模式：換了模式、或正在用的那個內容被改了，就重新套用（目前的值 = 模式本體）
    const Profile* np = findProfile(s, s.profileName);
    const Profile* op = findProfile(settings_, settings_.profileName);
    bool reapply = np && (!op || _wcsicmp(np->name.c_str(), op->name.c_str()) != 0 || !samePrefs(*np, *op));
    settings_.profiles = s.profiles;
    settings_.profileName = s.profileName;
    rebuildProfileMenu();
    if (reapply) switchProfile(*np);
    nav_.setPinnedNames(settings_.pinnedNames);
    if (pinnedChanged) applyPinnedNow();
    if (unzipChanged) {
        // 正在看壓縮檔就等它關掉再搬（看到一半不搬）；沒在看就現在把舊的刪掉，下次開在新位置建。
        tempDirStale_ = true;
        if (archivePath_.empty()) { tempDir_.release(); tempDirStale_ = false; }
    }
    disarmJump();
    updateTitle();
    saveSettingsQuiet();
}

void MainWindow::applyPinnedNow() {
    if (path_.empty() || nav_.count() == 0) return;
    if (!archivePath_.empty()) {
        movePinnedFirst(archiveFiles_, archiveNames_, settings_.pinnedNames);
        nav_.setFiles(archiveFiles_, path_);
    } else {
        nav_.load(path_);
    }
    if (twoPage()) { spreadsKey_.clear(); updateSpreads(); loadSecond(); fit_ = true; fitToView(); }
    updateTitle();
    repaint();
}

bool MainWindow::onKeyDown(WPARAM vk) {
    bool ctrl = ctrlDown(), shift = shiftDown();
    if (ctrl) {
        switch (vk) {
        case 'Z': onCommand(shift ? IDM_REDO : IDM_UNDO); return true;
        case 'Y': onCommand(IDM_REDO); return true;
        case 'S': if (shift) exportAs(); else save(); return true;
        case 'O': openDialog(); return true;
        case 'R': if (!twoPage()) rotateImage(); return true;
        case VK_OEM_PLUS: case VK_ADD: zoomStep(1); return true;
        case VK_OEM_MINUS: case VK_SUBTRACT: zoomStep(-1); return true;
        case '0': case VK_NUMPAD0: onCommand(IDM_ZOOM100); return true;
        case '9': onCommand(IDM_ZOOMFIT); return true;
        default: return false;
        }
    }
    // 待命中的跳本：按了翻頁以外的鍵就取消
    if (armedDir_ && vk != VK_LEFT && vk != VK_RIGHT && vk != VK_UP && vk != VK_DOWN && vk != VK_PRIOR && vk != VK_NEXT)
        disarmJump();
    switch (vk) {
    case VK_LEFT: case VK_RIGHT: case VK_UP: case VK_DOWN: {
        if (const Annotation* sel = selectedItem()) {
            float step = shift ? 10.0f : 1.0f;
            Annotation a = *sel;
            moveBy(a, vk == VK_LEFT ? -step : vk == VK_RIGHT ? step : 0, vk == VK_UP ? -step : vk == VK_DOWN ? step : 0);
            commitSelectedChange(a);
            return true;
        }
        if (vk == VK_LEFT) goPrev();
        else if (vk == VK_RIGHT) goNext();
        else scrollKey(static_cast<int>(vk));
        return true;
    }
    case VK_PRIOR: goPrev(); return true;
    case VK_NEXT: goNext(); return true;
    case VK_DELETE: case VK_BACK: deleteSelected(); return true;
    case VK_ESCAPE:
        if (pickingWhite_) { pickingWhite_ = false; return true; }
        if (selected_) { selected_ = 0; repaint(); return true; }
        if (freehandMode_) { freehandMode_ = false; buildButtons(); repaint(); return true; }
        return true;
    case VK_SPACE: case VK_RETURN:
        if (const Annotation* sel = selectedItem()) {
            if (sel->kind == Kind::Text && vk == VK_RETURN) { beginTextEdit(sel->id, false); return true; }
        }
        return false;
    default: return false;
    }
}

// ---------------------------------------------------------------------------
// 標註

const Annotation* MainWindow::selectedItem() const {
    if (!selected_) return nullptr;
    const auto& items = visibleItems();
    for (const auto& a : items)
        if (a.id == selected_) return &a;
    return nullptr;
}

void MainWindow::select(uint32_t id) {
    selected_ = id;
    buildButtons();
    repaint();
}

StrokeStyle MainWindow::currentStroke() const {
    if (const Annotation* a = doc_.findItem(selected_)) return a->stroke;
    return stroke_;
}

TextStyle MainWindow::currentText() const {
    if (const Annotation* a = doc_.findItem(selected_)) {
        if (a->kind == Kind::Text) return a->text;
    }
    return text_;
}

void MainWindow::commitSelectedChange(const Annotation& updated) {
    int i = doc_.indexOf(updated.id);
    if (i < 0) return;
    auto items = doc_.items();
    items[i] = updated;
    if (items[i].kind == Kind::Text) items[i].rect.bottom = items[i].rect.top + renderer_.textHeight(items[i]);
    doc_.commitItems(std::move(items));
    updateTitle();
    positionEdit();
    repaint();
}

void MainWindow::deleteSelected() {
    if (edit_) return;
    int i = doc_.indexOf(selected_);
    if (i < 0) return;
    auto items = doc_.items();
    items.erase(items.begin() + i);
    doc_.commitItems(std::move(items));
    selected_ = 0;
    updateTitle();
    repaint();
}

void MainWindow::addShape(Kind kind) {
    if (!doc_.image() || !editGuard()) return;
    endTextEdit(true);
    freehandMode_ = false;
    Pt c = toImage(canvasCenter());
    float cw = canvasRc_.right - canvasRc_.left, ch = canvasRc_.bottom - canvasRc_.top;
    float size = std::min(cw, ch) / zoom_;
    Annotation a = makeShape(kind, c, size, stroke_);
    a.id = doc_.nextId();
    auto items = doc_.items();
    items.push_back(a);
    doc_.commitItems(std::move(items));
    select(a.id);
    updateTitle();
}

void MainWindow::addText() {
    if (!doc_.image() || !editGuard()) return;
    endTextEdit(true);
    freehandMode_ = false;
    Pt c = toImage(canvasCenter());
    float cw = canvasRc_.right - canvasRc_.left, ch = canvasRc_.bottom - canvasRc_.top;
    Annotation a = makeText(c, std::min(cw, ch) / zoom_, stroke_, text_);
    a.id = doc_.nextId();
    a.rect.bottom = a.rect.top + renderer_.textHeight(a);
    auto items = doc_.items();
    items.push_back(a);
    doc_.commitItems(std::move(items));
    select(a.id);
    updateTitle();
    beginTextEdit(a.id, true);
}

void MainWindow::applyStrokeStyle(const StrokeStyle& s) {
    const Annotation* sel = doc_.findItem(selected_);
    // 文字方塊的外框預設無色，改它的外框不要影響之後新畫的形狀。
    if (!sel || sel->kind != Kind::Text) stroke_ = s;
    if (sel) {
        Annotation a = *sel;
        a.stroke = s;
        commitSelectedChange(a);
        if (edit_) InvalidateRect(edit_, nullptr, TRUE);
    }
    buildButtons();
    repaint();
}

void MainWindow::applyTextStyle(const TextStyle& t) {
    text_ = t;
    if (const Annotation* sel = doc_.findItem(selected_)) {
        if (sel->kind == Kind::Text) {
            Annotation a = *sel;
            a.text = t;
            commitSelectedChange(a);
            updateEditFont();
        }
    }
    repaint();
}

void MainWindow::rotateImage() {
    auto img = doc_.image();
    if (!img) return; // 壓縮檔內也能轉：當成檢視動作，翻頁就重置
    endTextEdit(true);
    if (adjusting_) { closeColorAdjustPanel(); cancelAdjust(); }
    DocState next;
    next.image = std::make_shared<Image>(rotateCCW90(*img));
    next.items = doc_.items();
    for (auto& a : next.items) {
        rotateCCW90(a, img->width);
        if (a.kind == Kind::Text) a.rect.bottom = a.rect.top + renderer_.textHeight(a);
    }
    doc_.commit(std::move(next));
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
    endTextEdit(true);
    ResizeResult r;
    if (!showResizeDialog(hwnd_, img->width, img->height, img->dpiX, r)) return;
    DocState next;
    next.items = doc_.items();
    if (r.resample && (r.width != img->width || r.height != img->height)) {
        auto out = std::make_shared<Image>(resample(*img, r.width, r.height));
        out->dpiX = out->dpiY = r.dpi;
        float sx = static_cast<float>(r.width) / img->width, sy = static_cast<float>(r.height) / img->height;
        for (auto& a : next.items) {
            scaleBy(a, sx, sy);
            if (a.kind == Kind::Text) a.rect.bottom = a.rect.top + renderer_.textHeight(a);
        }
        next.image = out;
    } else {
        auto out = std::make_shared<Image>(*img);
        out->dpiX = out->dpiY = r.dpi;
        next.image = out;
    }
    doc_.commit(std::move(next));
    infoData_.width = doc_.image()->width;
    infoData_.height = doc_.image()->height;
    infoData_.dpiX = infoData_.dpiY = r.dpi;
    fit_ = true;
    fitToView();
    updateTitle();
    repaint();
}

// ---------------------------------------------------------------------------
// 文字編輯（用系統 EDIT 控制項蓋在文字方塊上）

void MainWindow::beginTextEdit(uint32_t id, bool selectAll) {
    const Annotation* a = doc_.findItem(id);
    if (!a || a->kind != Kind::Text) return;
    if (edit_) endTextEdit(true);
    editingId_ = id;
    editOriginal_ = a->textValue;
    editText_ = a->textValue;
    DWORD align = a->text.align == TextAlign::Center ? ES_CENTER : a->text.align == TextAlign::Right ? ES_RIGHT : ES_LEFT;
    edit_ = CreateWindowExW(0, L"EDIT", a->textValue.c_str(),
                            WS_CHILD | WS_VISIBLE | ES_MULTILINE | ES_AUTOVSCROLL | ES_WANTRETURN | align, 0, 0, 10, 10,
                            hwnd_, nullptr, inst_, nullptr);
    SetWindowSubclass(edit_, editProc, kEditSubclass, reinterpret_cast<DWORD_PTR>(this));
    updateEditFont();
    positionEdit();
    SetFocus(edit_);
    if (selectAll) SendMessageW(edit_, EM_SETSEL, 0, -1);
    else SendMessageW(edit_, EM_SETSEL, static_cast<WPARAM>(editText_.size()), static_cast<LPARAM>(editText_.size()));
    repaint();
}

void MainWindow::updateEditFont() {
    if (!edit_) return;
    const Annotation* a = doc_.findItem(editingId_);
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
    const Annotation* a = doc_.findItem(editingId_);
    if (!a) return;
    float pad = 4.0f;
    POINT tl = toClient({a->rect.left + pad, a->rect.top + pad});
    POINT brp = toClient({a->rect.right - pad, a->rect.bottom - pad});
    int w = std::max(10l, brp.x - tl.x), h = std::max(10l, brp.y - tl.y);
    // 不超出畫布
    SetWindowPos(edit_, HWND_TOP, tl.x, tl.y, w, h, SWP_NOACTIVATE);
}

void MainWindow::onEditChanged() {
    if (!edit_) return;
    int n = GetWindowTextLengthW(edit_);
    std::wstring text(static_cast<size_t>(n), L'\0');
    GetWindowTextW(edit_, text.data(), n + 1);
    // 把 CRLF 換成 LF，DirectWrite 兩種都認得，但統一比較好比對。
    std::wstring clean;
    clean.reserve(text.size());
    for (wchar_t ch : text) if (ch != L'\r') clean += ch;
    editText_ = clean;
    // 高度跟著內容長：直接更新文件裡的物件（不進 undo，結束編輯時才一次 commit）。
    const Annotation* a = doc_.findItem(editingId_);
    if (!a) return;
    Annotation tmp = *a;
    tmp.textValue = editText_;
    float newBottom = tmp.rect.top + renderer_.textHeight(tmp);
    if (std::abs(newBottom - a->rect.bottom) > 0.5f) {
        doc_.itemsMutable()[doc_.indexOf(editingId_)].rect.bottom = newBottom;
        positionEdit();
        repaint();
    }
}

void MainWindow::endTextEdit(bool commit) {
    if (!edit_) return;
    HWND e = edit_;
    edit_ = nullptr;
    uint32_t id = editingId_;
    editingId_ = 0;
    RemoveWindowSubclass(e, editProc, kEditSubclass);
    DestroyWindow(e);
    SetFocus(hwnd_);
    int i = doc_.indexOf(id);
    if (i < 0) { repaint(); return; }
    if (commit) {
        auto items = doc_.items();
        if (editText_.empty()) {
            items.erase(items.begin() + i);
            if (selected_ == id) selected_ = 0;
            doc_.commitItems(std::move(items));
        } else if (editText_ != editOriginal_) {
            items[i].textValue = editText_;
            items[i].rect.bottom = items[i].rect.top + renderer_.textHeight(items[i]);
            doc_.commitItems(std::move(items));
        }
    }
    updateTitle();
    repaint();
}

// ---------------------------------------------------------------------------
// 調整顏色（ColorAdjustHost）

const Image& MainWindow::baseImage() { return *adjustBase_; }

void MainWindow::previewChanged(const ColorAdjustParams& p) {
    adjustParams_ = p;
    if (!adjustBase_) return;
    SetCursor(LoadCursorW(nullptr, IDC_WAIT));
    adjustPreview_ = p.isIdentity() ? adjustBase_ : std::make_shared<Image>(applyColorAdjust(*adjustBase_, p));
    repaint();
}

void MainWindow::commitAdjust(const ColorAdjustParams& p) {
    adjustParams_ = p;
    if (adjustBase_ && !p.isIdentity()) {
        ImagePtr out = adjustPreview_ && adjustPreview_ != adjustBase_ ? adjustPreview_
                                                                        : std::make_shared<Image>(applyColorAdjust(*adjustBase_, p));
        doc_.commitImage(out);
        updateTitle();
    }
    adjusting_ = false;
    pickingWhite_ = false;
    adjustPreview_.reset();
    adjustBase_.reset();
    buildButtons();
    repaint();
}

void MainWindow::cancelAdjust() {
    adjusting_ = false;
    pickingWhite_ = false;
    adjustPreview_.reset();
    adjustBase_.reset();
    buildButtons();
    repaint();
}

void MainWindow::beginPickWhite() {
    pickingWhite_ = true;
    SetFocus(hwnd_);
}

} // namespace mp
