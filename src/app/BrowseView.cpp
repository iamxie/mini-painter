#include "app/BrowseView.h"

#include <commoncontrols.h>
#include <shellapi.h>
#include <shlobj.h>
#include <uxtheme.h>

#include <algorithm>
#include <cstring>
#include <thread>

#include "app/AppIds.h"
#include "core/Fonts.h"
#include "core/PathUtil.h"
#include "core/TextUtil.h"

namespace mp {

using Microsoft::WRL::ComPtr;

namespace {

// 版面與字（邏輯像素）。
constexpr float kListFontPx = 12;   // 縮圖下面的檔名
constexpr float kPathFontPx = 13;   // 路徑列（和檔名列的字一樣大）
constexpr float kItemPadX = 20;     // 一格比縮圖寬多少
constexpr float kItemPadY = 44;     // 一格比縮圖高多少（兩行檔名）
constexpr size_t kMaxSlots = 600;   // 畫面上暫留的縮圖最多幾張；超過就丟離畫面最遠的
constexpr int kSlotKeepBefore = 200, kSlotKeepAfter = 400;
constexpr UINT_PTR kTreeSubclass = 1;

COLORREF colorRef(D2D1_COLOR_F c) {
    auto b = [](float v) { return static_cast<BYTE>(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f); };
    return RGB(b(c.r), b(c.g), b(c.b));
}

std::wstring keyOf(const std::wstring& path) { return lowerCopy(path); }

// premultiplied BGRA → 由上往下的 32 位元 DIB（GdiAlphaBlend 要 premultiplied）。
HBITMAP dibOf(const Image& img) {
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = img.width;
    bi.bmiHeader.biHeight = -img.height;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP bmp = CreateDIBSection(nullptr, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (bmp && bits) std::memcpy(bits, img.pixels.data(), img.pixels.size());
    return bmp;
}

std::wstring fileSystemPathOf(IShellItem* item) {
    PWSTR p = nullptr;
    if (FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &p)) || !p) return L"";
    std::wstring s = p;
    CoTaskMemFree(p);
    return s;
}

std::wstring normalNameOf(IShellItem* item) {
    PWSTR p = nullptr;
    if (FAILED(item->GetDisplayName(SIGDN_NORMALDISPLAY, &p)) || !p) return L"";
    std::wstring s = p;
    CoTaskMemFree(p);
    return s;
}

// 樹不做檔案操作：右鍵選單不出來。
LRESULT CALLBACK treeProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR) {
    if (msg == WM_CONTEXTMENU || msg == WM_RBUTTONUP) return 0;
    return DefSubclassProc(hwnd, msg, wp, lp);
}

// 資料夾樹的事件與篩選：只列真的資料夾（與網路上的電腦這類通往資料夾的項目），壓縮檔（Windows 當成資料夾的 zip）、
// 控制台、資源回收筒不列。選取變了就換資料夾。
class TreeEvents final : public INameSpaceTreeControlEvents, public IShellItemFilter {
public:
    explicit TreeEvents(BrowseView* view) : view_(view) {}
    void detach() { view_ = nullptr; }

    // IUnknown
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** out) override {
        if (!out) return E_POINTER;
        if (riid == __uuidof(IUnknown) || riid == __uuidof(INameSpaceTreeControlEvents)) *out = static_cast<INameSpaceTreeControlEvents*>(this);
        else if (riid == __uuidof(IShellItemFilter)) *out = static_cast<IShellItemFilter*>(this);
        else { *out = nullptr; return E_NOINTERFACE; }
        AddRef();
        return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return static_cast<ULONG>(InterlockedIncrement(&refs_)); }
    ULONG STDMETHODCALLTYPE Release() override {
        const LONG n = InterlockedDecrement(&refs_);
        if (n == 0) delete this;
        return static_cast<ULONG>(n);
    }

    // IShellItemFilter
    HRESULT STDMETHODCALLTYPE IncludeItem(IShellItem* item) override {
        SFGAOF a = 0;
        if (FAILED(item->GetAttributes(SFGAO_FOLDER | SFGAO_STREAM | SFGAO_FILESYSTEM | SFGAO_FILESYSANCESTOR, &a))) return S_FALSE;
        const bool folder = (a & SFGAO_FOLDER) != 0, stream = (a & SFGAO_STREAM) != 0;
        const bool real = (a & (SFGAO_FILESYSTEM | SFGAO_FILESYSANCESTOR)) != 0;
        return folder && !stream && real ? S_OK : S_FALSE;
    }
    HRESULT STDMETHODCALLTYPE GetEnumFlagsForItem(IShellItem*, SHCONTF* flags) override {
        *flags = SHCONTF_FOLDERS;
        return S_OK;
    }

    // INameSpaceTreeControlEvents
    HRESULT STDMETHODCALLTYPE OnItemClick(IShellItem*, NSTCEHITTEST, NSTCECLICKTYPE) override { return S_FALSE; }
    HRESULT STDMETHODCALLTYPE OnPropertyItemCommit(IShellItem*) override { return S_FALSE; }
    HRESULT STDMETHODCALLTYPE OnItemStateChanging(IShellItem*, NSTCITEMSTATE, NSTCITEMSTATE) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnItemStateChanged(IShellItem*, NSTCITEMSTATE, NSTCITEMSTATE) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnSelectionChanged(IShellItemArray* items) override {
        ComPtr<IShellItem> item;
        if (view_ && items && SUCCEEDED(items->GetItemAt(0, &item))) view_->treeSelected(item.Get());
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnKeyboardInput(UINT msg, WPARAM wp, LPARAM) override {
        return view_ && view_->treeKey(msg, wp) ? S_OK : S_FALSE;
    }
    HRESULT STDMETHODCALLTYPE OnBeforeExpand(IShellItem*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnAfterExpand(IShellItem*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnBeginLabelEdit(IShellItem*) override { return E_FAIL; } // 不能改名
    HRESULT STDMETHODCALLTYPE OnEndLabelEdit(IShellItem*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnGetToolTip(IShellItem*, LPWSTR, int) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE OnBeforeItemDelete(IShellItem*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE OnItemAdded(IShellItem*, BOOL) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE OnItemDeleted(IShellItem*, BOOL) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE OnBeforeContextMenu(IShellItem*, REFIID, void** ppv) override {
        if (ppv) *ppv = nullptr;
        return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE OnAfterContextMenu(IShellItem*, IContextMenu*, REFIID, void** ppv) override {
        if (ppv) *ppv = nullptr;
        return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE OnBeforeStateImageChange(IShellItem*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnGetDefaultIconIndex(IShellItem*, int*, int*) override { return E_NOTIMPL; }

private:
    ~TreeEvents() = default;
    BrowseView* view_;
    LONG refs_ = 1;
};

} // namespace

BrowseView::BrowseView() = default;
BrowseView::~BrowseView() { destroy(); }

bool BrowseView::create(HWND parent, BrowseHost* host, UiScale px, ThumbCache* cache) {
    if (created()) return true;
    parent_ = parent;
    host_ = host;
    px_ = px;
    cache_ = cache;
    HINSTANCE inst = reinterpret_cast<HINSTANCE>(GetWindowLongPtrW(parent, GWLP_HINSTANCE));
    list_ = CreateWindowExW(0, WC_LISTVIEWW, L"",
                            WS_CHILD | WS_CLIPSIBLINGS | WS_TABSTOP | LVS_ICON | LVS_SINGLESEL | LVS_SHOWSELALWAYS | LVS_OWNERDATA |
                                LVS_AUTOARRANGE,
                            0, 0, 0, 0, parent, nullptr, inst, nullptr);
    if (!list_) return false;
    SetWindowTheme(list_, L"Explorer", nullptr);
    ListView_SetExtendedListViewStyle(list_, LVS_EX_DOUBLEBUFFER | LVS_EX_INFOTIP);
    path_ = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | ES_READONLY | ES_AUTOHSCROLL | ES_CENTER, 0, 0, 0, 0, parent, nullptr, inst, nullptr);
    createTree();
    applyFonts();
    applyThumbSize();
    // 縮圖：幾條背景執行緒，算好的排在 ready_，通知主視窗來拿。
    const int threads = static_cast<int>(std::clamp(std::thread::hardware_concurrency(), 1u, 4u));
    ThumbCache* c = cache_;
    queue_.start(threads, [c](const std::wstring& path, int side) { return loadThumbnail(path, side, c, true); },
                 [this](const ThumbJob& job, ThumbResult&& r) {
                     bool first = false;
                     {
                         std::lock_guard<std::mutex> lock(readyMu_);
                         first = ready_.empty();
                         ready_.emplace_back(job, std::move(r));
                     }
                     if (first) PostMessageW(parent_, WM_MP_THUMBS_READY, 0, 0);
                 });
    queue_.reset(generation_);
    return true;
}

void BrowseView::createTree() {
    if (FAILED(CoCreateInstance(CLSID_NamespaceTreeControl, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&tree_)))) return;
    RECT rc{0, 0, 0, 0};
    const NSTCSTYLE style = NSTCS_HASEXPANDOS | NSTCS_ROOTHASEXPANDO | NSTCS_FADEINOUTEXPANDOS | NSTCS_NOINFOTIP | NSTCS_SHOWSELECTIONALWAYS |
                            NSTCS_DISABLEDRAGDROP | NSTCS_AUTOHSCROLL | NSTCS_SINGLECLICKEXPAND;
    if (FAILED(tree_->Initialize(parent_, &rc, style))) { tree_.Reset(); return; }
    auto* events = new TreeEvents(this);
    treeEvents_.Attach(static_cast<INameSpaceTreeControlEvents*>(events));
    tree_->TreeAdvise(treeEvents_.Get(), &treeCookie_);
    ComPtr<IShellItemFilter> filter;
    treeEvents_.As(&filter);
    // 根節點跟 Win10 檔案總管一樣：快速存取、OneDrive、本機、網路（使用者 2026-11-24 定）。沒有的（例如沒設定 OneDrive）略過。
    auto addRoot = [&](ComPtr<IShellItem> item, NSTCROOTSTYLE rootStyle) {
        if (!item) return;
        if (SUCCEEDED(tree_->AppendRoot(item.Get(), SHCONTF_FOLDERS, rootStyle, filter.Get()))) roots_.push_back(item);
    };
    ComPtr<IShellItem> item;
    if (SUCCEEDED(SHCreateItemFromParsingName(L"shell:::{679f85cb-0220-4080-b29b-5540cc05aab6}", nullptr, IID_PPV_ARGS(&item))))
        addRoot(item, NSTCRS_VISIBLE);
    item.Reset();
    if (SUCCEEDED(SHGetKnownFolderItem(FOLDERID_SkyDrive, KF_FLAG_DEFAULT, nullptr, IID_PPV_ARGS(&item)))) addRoot(item, NSTCRS_VISIBLE);
    item.Reset();
    if (SUCCEEDED(SHGetKnownFolderItem(FOLDERID_ComputerFolder, KF_FLAG_DEFAULT, nullptr, IID_PPV_ARGS(&item)))) addRoot(item, NSTCRS_EXPANDED);
    item.Reset();
    if (SUCCEEDED(SHGetKnownFolderItem(FOLDERID_NetworkFolder, KF_FLAG_DEFAULT, nullptr, IID_PPV_ARGS(&item)))) addRoot(item, NSTCRS_VISIBLE);
    ComPtr<IOleWindow> ow;
    if (SUCCEEDED(tree_.As(&ow))) ow->GetWindow(&treeHwnd_);
    if (treeHwnd_) {
        ShowWindow(treeHwnd_, SW_HIDE);
        SetWindowSubclass(treeHwnd_, treeProc, kTreeSubclass, 0);
        // 裡面真正的 TreeView 也要擋右鍵
        if (HWND inner = FindWindowExW(treeHwnd_, nullptr, WC_TREEVIEWW, nullptr)) SetWindowSubclass(inner, treeProc, kTreeSubclass, 0);
    }
}

void BrowseView::destroy() {
    queue_.shutdown();
    if (notifyId_) { SHChangeNotifyDeregister(notifyId_); notifyId_ = 0; }
    if (tree_) {
        if (treeCookie_) tree_->TreeUnadvise(treeCookie_);
        treeCookie_ = 0;
        tree_->RemoveAllRoots();
        tree_.Reset();
    }
    if (treeEvents_) static_cast<TreeEvents*>(static_cast<INameSpaceTreeControlEvents*>(treeEvents_.Get()))->detach();
    treeEvents_.Reset();
    roots_.clear();
    treeHwnd_ = nullptr; // 控制項放掉時視窗跟著消失
    if (list_) DestroyWindow(list_);
    if (path_) DestroyWindow(path_);
    list_ = path_ = nullptr;
    clearSlots();
    for (auto& [index, icon] : icons_) DestroyIcon(icon);
    icons_.clear();
    if (blankImages_) ImageList_Destroy(blankImages_);
    blankImages_ = nullptr;
    if (listFont_) DeleteObject(listFont_);
    if (pathFont_) DeleteObject(pathFont_);
    if (pathBrush_) DeleteObject(pathBrush_);
    listFont_ = pathFont_ = nullptr;
    pathBrush_ = nullptr;
    std::lock_guard<std::mutex> lock(readyMu_);
    ready_.clear();
}

void BrowseView::show(bool on) {
    const int cmd = on ? SW_SHOW : SW_HIDE;
    if (list_) ShowWindow(list_, cmd);
    if (path_) ShowWindow(path_, cmd);
    if (treeHwnd_) ShowWindow(treeHwnd_, cmd);
    if (!on) {
        // 離開瀏覽：排隊中的縮圖不算了，暫留的縮圖放掉。
        queue_.reset(++generation_);
        clearSlots();
        if (notifyId_) { SHChangeNotifyDeregister(notifyId_); notifyId_ = 0; }
    }
}

void BrowseView::layout(const RECT& tree, const RECT& list, const RECT& path) {
    auto place = [](HWND h, const RECT& r) {
        if (h) SetWindowPos(h, nullptr, r.left, r.top, std::max(0L, r.right - r.left), std::max(0L, r.bottom - r.top), SWP_NOZORDER | SWP_NOACTIVATE);
    };
    place(treeHwnd_, tree);
    place(list_, list);
    // 單行 EDIT 的字貼著上緣：高度只給一行字，垂直置中在檔名列裡。
    RECT p = path;
    const int lineH = px_.round(kPathFontPx + 6);
    const int mid = (path.top + path.bottom) / 2;
    p.top = mid - lineH / 2;
    p.bottom = p.top + lineH;
    p.left += px_.round(8);
    p.right -= px_.round(8);
    place(path_, p);
}

void BrowseView::setScale(UiScale px) {
    px_ = px;
    applyFonts();
    applyThumbSize();
}

void BrowseView::setThumbSize(int index) {
    if (index == sizeIndex_ || index < 0 || index >= kThumbSizeCount) return;
    sizeIndex_ = index;
    applyThumbSize();
}

int BrowseView::side() const { return px_.round(static_cast<float>(kThumbSides[sizeIndex_])); }

void BrowseView::applyFonts() {
    if (listFont_) DeleteObject(listFont_);
    if (pathFont_) DeleteObject(pathFont_);
    listFont_ = CreateFontW(-px_.round(kListFontPx), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                            CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, kUiFont);
    pathFont_ = CreateFontW(-px_.round(kPathFontPx), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                            CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, kUiFont);
    if (list_) SendMessageW(list_, WM_SETFONT, reinterpret_cast<WPARAM>(listFont_), TRUE);
    if (path_) SendMessageW(path_, WM_SETFONT, reinterpret_cast<WPARAM>(pathFont_), TRUE);
}

void BrowseView::applyThumbSize() {
    if (!list_) return;
    const int s = side();
    // ListView 照圖示清單的大小排格子；圖示本身是透明的，縮圖在 custom draw 自己畫上去。
    // 空白的那一張要整張是遮罩的透明部分：只給 alpha 是 0 的 32 位元圖，清單會當成沒有 alpha、畫成黑的（VM 走查看到的）。
    HIMAGELIST il = ImageList_Create(s, s, ILC_COLOR32 | ILC_MASK, 1, 0);
    if (il) {
        BITMAPINFO bi{};
        bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
        bi.bmiHeader.biWidth = s;
        bi.bmiHeader.biHeight = -s;
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        void* bits = nullptr;
        HBITMAP color = CreateDIBSection(nullptr, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
        const std::vector<BYTE> ones(static_cast<size_t>((s + 15) / 16 * 2) * static_cast<size_t>(s), 0xFF); // 單色點陣圖一列對齊 2 位元組
        HBITMAP mask = CreateBitmap(s, s, 1, 1, ones.data()); // 1 = 透明
        if (color && mask) ImageList_Add(il, color, mask);
        if (color) DeleteObject(color);
        if (mask) DeleteObject(mask);
    }
    ListView_SetImageList(list_, il, LVSIL_NORMAL);
    if (blankImages_) ImageList_Destroy(blankImages_);
    blankImages_ = il;
    ListView_SetIconSpacing(list_, s + px_.round(kItemPadX), s + px_.round(kItemPadY));
    queue_.reset(++generation_);
    clearSlots();
    InvalidateRect(list_, nullptr, TRUE);
}

void BrowseView::navigate(const std::wstring& dir, const std::wstring& select) {
    folder_ = dir;
    display_ = folderDisplayName(dir);
    SetWindowTextW(path_, dir.c_str());
    queue_.reset(++generation_);
    clearSlots();
    setEntries(listBrowseEntries(dir, pinned_), select);
    watchFolder();
    syncTree(dir);
    if (host_) host_->browseFolderChanged();
}

void BrowseView::setEntries(std::vector<BrowseEntry> entries, const std::wstring& select) {
    entries_ = std::move(entries);
    ListView_SetItemCountEx(list_, static_cast<int>(entries_.size()), LVSICF_NOSCROLL);
    ListView_SetItemState(list_, -1, 0, LVIS_SELECTED | LVIS_FOCUSED);
    const int sel = select.empty() ? -1 : indexOfBrowsePath(entries_, select);
    if (sel >= 0) {
        ListView_SetItemState(list_, sel, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
        ListView_EnsureVisible(list_, sel, FALSE);
    } else if (!entries_.empty()) {
        ListView_EnsureVisible(list_, 0, FALSE);
    }
    InvalidateRect(list_, nullptr, TRUE);
    if (host_) host_->browseSelectionChanged();
}

void BrowseView::refresh() {
    if (folder_.empty()) return;
    const std::wstring existing = nearestExistingFolder(folder_);
    if (existing.empty()) return;
    if (_wcsicmp(existing.c_str(), folder_.c_str()) != 0) { // 正在看的資料夾被刪掉或改名：退回還在的上一層
        navigate(existing, L"");
        return;
    }
    const int sel = selected();
    const std::wstring keep = sel >= 0 ? entries_[static_cast<size_t>(sel)].path : L"";
    const int top = ListView_GetTopIndex(list_);
    std::vector<BrowseEntry> fresh = listBrowseEntries(folder_, pinned_);
    // 改過的檔的縮圖要重算。
    for (auto it = slots_.begin(); it != slots_.end();) {
        ThumbStamp now;
        if (!fileStamp(it->first, now) || !(now == it->second.stamp)) {
            if (it->second.bmp) DeleteObject(it->second.bmp);
            it = slots_.erase(it);
        } else {
            ++it;
        }
    }
    const bool same = fresh.size() == entries_.size() &&
                      std::equal(fresh.begin(), fresh.end(), entries_.begin(), [](const BrowseEntry& a, const BrowseEntry& b) { return a.path == b.path; });
    if (same) {
        InvalidateRect(list_, nullptr, TRUE);
        return;
    }
    entries_ = std::move(fresh);
    ListView_SetItemCountEx(list_, static_cast<int>(entries_.size()), LVSICF_NOSCROLL);
    ListView_SetItemState(list_, -1, 0, LVIS_SELECTED | LVIS_FOCUSED);
    const int now = keep.empty() ? -1 : indexOfBrowsePath(entries_, keep);
    if (now >= 0) ListView_SetItemState(list_, now, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
    if (top >= 0 && top < static_cast<int>(entries_.size())) ListView_EnsureVisible(list_, top, FALSE);
    InvalidateRect(list_, nullptr, TRUE);
    if (host_ && now != sel) host_->browseSelectionChanged();
}

void BrowseView::up() {
    const std::wstring parent = parentFolder(folder_);
    if (parent.empty()) return;
    const std::wstring from = folder_;
    navigate(parent, from);
}

int BrowseView::selected() const {
    if (!list_) return -1;
    const int i = ListView_GetNextItem(list_, -1, LVNI_SELECTED);
    return i >= 0 && i < static_cast<int>(entries_.size()) ? i : -1;
}

void BrowseView::focusList() {
    if (list_) SetFocus(list_);
}

void BrowseView::cycleFocus() {
    HWND f = GetFocus();
    if (treeHwnd_ && (f == list_ || f == path_)) SetFocus(treeHwnd_);
    else focusList();
}

bool BrowseView::owns(HWND h) const {
    if (!h) return false;
    return h == list_ || h == path_ || h == treeHwnd_ || (treeHwnd_ && IsChild(treeHwnd_, h));
}

void BrowseView::syncTree(const std::wstring& dir) {
    if (!tree_ || dir.empty()) return;
    ComPtr<IShellItem> target;
    if (FAILED(SHCreateItemFromParsingName(dir.c_str(), nullptr, IID_PPV_ARGS(&target)))) return;
    // 從 dir 往上找到某個根節點，再從根往下一層一層展開，最後選中 dir。
    std::vector<ComPtr<IShellItem>> chain{target};
    int rootAt = -1;
    for (int guard = 0; guard < 256 && rootAt < 0; ++guard) {
        for (const auto& r : roots_) {
            int order = 1;
            if (SUCCEEDED(chain.back()->Compare(r.Get(), SICHINT_CANONICAL, &order)) && order == 0) { rootAt = static_cast<int>(chain.size()) - 1; break; }
        }
        if (rootAt >= 0) break;
        ComPtr<IShellItem> parent;
        if (FAILED(chain.back()->GetParent(&parent))) break;
        chain.push_back(parent);
    }
    syncing_ = true;
    if (rootAt >= 0)
        for (int i = rootAt; i >= 1; --i) tree_->SetItemState(chain[static_cast<size_t>(i)].Get(), NSTCIS_EXPANDED, NSTCIS_EXPANDED);
    tree_->SetItemState(target.Get(), NSTCIS_SELECTED, NSTCIS_SELECTED);
    tree_->EnsureItemVisible(target.Get());
    syncing_ = false;
}

void BrowseView::treeSelected(IShellItem* item) {
    if (syncing_ || !item) return;
    const std::wstring path = fileSystemPathOf(item);
    if (!path.empty() && isDirectoryPath(path)) {
        if (_wcsicmp(path.c_str(), folder_.c_str()) == 0) return;
        folder_ = browseFolderOf(path);
        display_ = folderDisplayName(folder_);
        SetWindowTextW(path_, folder_.c_str());
        queue_.reset(++generation_);
        clearSlots();
        setEntries(listBrowseEntries(folder_, pinned_), L"");
        watchFolder();
    } else {
        // 「本機」「網路」這類不是資料夾的項目：右邊空著，路徑列寫它的名字。
        folder_.clear();
        display_ = normalNameOf(item);
        SetWindowTextW(path_, display_.c_str());
        queue_.reset(++generation_);
        clearSlots();
        setEntries({}, L"");
        if (notifyId_) { SHChangeNotifyDeregister(notifyId_); notifyId_ = 0; }
    }
    if (host_) host_->browseFolderChanged();
}

bool BrowseView::treeKey(UINT msg, WPARAM vk) {
    if (msg != WM_KEYDOWN && msg != WM_SYSKEYDOWN) return false;
    const bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0, shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
    // 會做檔案操作的鍵（刪除、改名、剪下複製貼上、復原、內容、右鍵選單鍵）一律吃掉。
    if (vk == VK_DELETE || vk == VK_F2 || vk == VK_APPS) return true;
    if (vk == VK_F10 && shift) return true;
    if (ctrl && (vk == 'C' || vk == 'X' || vk == 'V' || vk == 'Z' || vk == 'Y' || vk == 'N')) return true;
    if (msg == WM_SYSKEYDOWN && vk == VK_RETURN) return true; // Alt+Enter：內容
    return false;
}

void BrowseView::watchFolder() {
    if (notifyId_) { SHChangeNotifyDeregister(notifyId_); notifyId_ = 0; }
    if (folder_.empty()) return;
    PIDLIST_ABSOLUTE pidl = ILCreateFromPathW(folder_.c_str());
    if (!pidl) return;
    SHChangeNotifyEntry entry{pidl, FALSE};
    notifyId_ = SHChangeNotifyRegister(parent_, SHCNRF_InterruptLevel | SHCNRF_ShellLevel | SHCNRF_NewDelivery,
                                       SHCNE_CREATE | SHCNE_DELETE | SHCNE_MKDIR | SHCNE_RMDIR | SHCNE_RENAMEITEM | SHCNE_RENAMEFOLDER |
                                           SHCNE_UPDATEITEM | SHCNE_UPDATEDIR,
                                       WM_MP_BROWSE_FSCHANGE, 1, &entry);
    ILFree(pidl);
}

void BrowseView::ackFsChange(WPARAM wp, LPARAM lp) {
    PIDLIST_ABSOLUTE* pidls = nullptr;
    LONG event = 0;
    if (HANDLE lock = SHChangeNotification_Lock(reinterpret_cast<HANDLE>(wp), static_cast<DWORD>(lp), &pidls, &event))
        SHChangeNotification_Unlock(lock);
}

void BrowseView::requestRange(int from, int to) {
    from = std::max(0, from);
    to = std::min(to, static_cast<int>(entries_.size()) - 1);
    // 倒著排：每個要求都插到最前面，所以最後排的（from）最先算。
    for (int i = to; i >= from; --i) {
        const BrowseEntry& e = entries_[static_cast<size_t>(i)];
        if (e.kind != BrowseKind::Image || slots_.count(keyOf(e.path))) continue;
        queue_.request(ThumbJob{generation_, i, e.path, side()});
    }
}

void BrowseView::clearSlots() {
    for (auto& [key, slot] : slots_)
        if (slot.bmp) DeleteObject(slot.bmp);
    slots_.clear();
    std::lock_guard<std::mutex> lock(readyMu_);
    ready_.clear();
}

void BrowseView::trimSlots() {
    if (slots_.size() <= kMaxSlots) return;
    const int top = ListView_GetTopIndex(list_);
    std::map<std::wstring, bool> keep;
    for (int i = std::max(0, top - kSlotKeepBefore); i < std::min(static_cast<int>(entries_.size()), top + kSlotKeepAfter); ++i)
        keep[keyOf(entries_[static_cast<size_t>(i)].path)] = true;
    for (auto it = slots_.begin(); it != slots_.end();) {
        if (keep.count(it->first)) { ++it; continue; }
        if (it->second.bmp) DeleteObject(it->second.bmp);
        it = slots_.erase(it);
    }
}

void BrowseView::onThumbsReady() {
    std::vector<std::pair<ThumbJob, ThumbResult>> got;
    {
        std::lock_guard<std::mutex> lock(readyMu_);
        got.swap(ready_);
    }
    for (auto& [job, r] : got) {
        if (job.generation != generation_) continue;
        Slot slot;
        fileStamp(job.path, slot.stamp);
        if (r.image) {
            slot.bmp = dibOf(*r.image);
            slot.w = r.image->width;
            slot.h = r.image->height;
        }
        slot.failed = !slot.bmp;
        auto& old = slots_[keyOf(job.path)];
        if (old.bmp) DeleteObject(old.bmp);
        old = slot;
        if (job.index >= 0 && job.index < static_cast<int>(entries_.size()) && _wcsicmp(entries_[static_cast<size_t>(job.index)].path.c_str(), job.path.c_str()) == 0)
            ListView_RedrawItems(list_, job.index, job.index);
    }
    trimSlots();
}

HICON BrowseView::iconFor(const BrowseEntry& e) {
    SHFILEINFOW sfi{};
    const DWORD attr = e.kind == BrowseKind::Folder ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL;
    // 只看副檔名（USEFILEATTRIBUTES），不讀磁碟：每個資料夾、同一種壓縮檔都是同一個圖示。
    if (!SHGetFileInfoW(e.path.c_str(), attr, &sfi, sizeof(sfi), SHGFI_SYSICONINDEX | SHGFI_USEFILEATTRIBUTES)) return nullptr;
    auto it = icons_.find(sfi.iIcon);
    if (it != icons_.end()) return it->second;
    HICON icon = nullptr;
    ComPtr<IImageList> list;
    if (SUCCEEDED(SHGetImageList(SHIL_JUMBO, IID_PPV_ARGS(&list)))) list->GetIcon(sfi.iIcon, ILD_TRANSPARENT, &icon);
    icons_[sfi.iIcon] = icon;
    return icon;
}

void BrowseView::drawShellIcon(HDC dc, const BrowseEntry& e, const RECT& box) {
    HICON icon = iconFor(e);
    if (!icon) return;
    const int s = std::min(box.right - box.left, box.bottom - box.top);
    DrawIconEx(dc, box.left + (box.right - box.left - s) / 2, box.top + (box.bottom - box.top - s) / 2, icon, s, s, 0, nullptr, DI_NORMAL);
}

void BrowseView::drawItem(HDC dc, int index) {
    if (index < 0 || index >= static_cast<int>(entries_.size())) return;
    const BrowseEntry& e = entries_[static_cast<size_t>(index)];
    RECT box{};
    box.left = LVIR_ICON;
    if (!SendMessageW(list_, LVM_GETITEMRECT, static_cast<WPARAM>(index), reinterpret_cast<LPARAM>(&box))) return;
    // 圖示區置中一個 side × side 的框。
    const int s = side();
    const int cx = (box.left + box.right) / 2, cy = (box.top + box.bottom) / 2;
    const RECT frame{cx - s / 2, cy - s / 2, cx - s / 2 + s, cy - s / 2 + s};
    if (e.kind != BrowseKind::Image) {
        drawShellIcon(dc, e, frame);
        return;
    }
    auto it = slots_.find(keyOf(e.path));
    if (it == slots_.end()) {
        requestRange(index, index);
        return;
    }
    const Slot& slot = it->second;
    if (slot.failed || !slot.bmp) {
        drawShellIcon(dc, e, frame);
        return;
    }
    HDC mem = CreateCompatibleDC(dc);
    HGDIOBJ old = SelectObject(mem, slot.bmp);
    BLENDFUNCTION bf{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
    const int x = cx - slot.w / 2, y = cy - slot.h / 2;
    GdiAlphaBlend(dc, x, y, slot.w, slot.h, mem, 0, 0, slot.w, slot.h, bf);
    SelectObject(mem, old);
    DeleteDC(mem);
}

bool BrowseView::onNotify(NMHDR* nm, LRESULT& result) {
    if (!list_ || nm->hwndFrom != list_) return false;
    result = 0;
    switch (nm->code) {
    case LVN_GETDISPINFOW: {
        auto* di = reinterpret_cast<NMLVDISPINFOW*>(nm);
        const int i = di->item.iItem;
        if (i < 0 || i >= static_cast<int>(entries_.size())) return true;
        if (di->item.mask & LVIF_TEXT) di->item.pszText = const_cast<wchar_t*>(entries_[static_cast<size_t>(i)].name.c_str());
        if (di->item.mask & LVIF_IMAGE) di->item.iImage = 0;
        return true;
    }
    case LVN_GETINFOTIPW: { // 滑鼠停著：完整檔名
        auto* tip = reinterpret_cast<NMLVGETINFOTIPW*>(nm);
        if (tip->iItem >= 0 && tip->iItem < static_cast<int>(entries_.size()) && tip->pszText && tip->cchTextMax > 0)
            wcsncpy_s(tip->pszText, static_cast<size_t>(tip->cchTextMax), entries_[static_cast<size_t>(tip->iItem)].name.c_str(), _TRUNCATE);
        return true;
    }
    case LVN_ODCACHEHINT: {
        auto* hint = reinterpret_cast<NMLVCACHEHINT*>(nm);
        requestRange(hint->iFrom, hint->iTo);
        return true;
    }
    case LVN_ODFINDITEMW: result = -1; return true;
    case LVN_ITEMCHANGED: {
        auto* ch = reinterpret_cast<NMLISTVIEW*>(nm);
        if ((ch->uChanged & LVIF_STATE) && ((ch->uOldState ^ ch->uNewState) & LVIS_SELECTED) && host_) host_->browseSelectionChanged();
        return true;
    }
    case LVN_ITEMACTIVATE: {
        auto* act = reinterpret_cast<NMITEMACTIVATE*>(nm);
        const int i = act->iItem >= 0 ? act->iItem : selected();
        if (i >= 0 && i < static_cast<int>(entries_.size()) && host_) host_->browseActivated(i);
        return true;
    }
    case NM_CUSTOMDRAW: {
        auto* cd = reinterpret_cast<NMLVCUSTOMDRAW*>(nm);
        switch (cd->nmcd.dwDrawStage) {
        case CDDS_PREPAINT: result = CDRF_NOTIFYITEMDRAW; return true;
        case CDDS_ITEMPREPAINT: result = CDRF_NOTIFYPOSTPAINT; return true;
        case CDDS_ITEMPOSTPAINT: drawItem(cd->nmcd.hdc, static_cast<int>(cd->nmcd.dwItemSpec)); return true;
        default: result = CDRF_DODEFAULT; return true;
        }
    }
    default: return false;
    }
}

HBRUSH BrowseView::onCtlColor(HDC dc, HWND ctl) {
    if (!path_ || ctl != path_) return nullptr;
    if (!pathBrush_) pathBrush_ = CreateSolidBrush(colorRef(theme::kChromeBg));
    SetBkColor(dc, colorRef(theme::kChromeBg));
    SetTextColor(dc, colorRef(theme::kIcon));
    return pathBrush_;
}

} // namespace mp
