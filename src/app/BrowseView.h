#pragma once
#include <windows.h>
#include <commctrl.h>
#include <shobjidl.h>
#include <wrl/client.h>

#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "app/Theme.h"
#include "core/BrowseList.h"
#include "core/ThumbCache.h"
#include "core/Thumbnails.h"

namespace mp {

// 瀏覽資料夾裡，要主視窗幫忙的事。
class BrowseHost {
public:
    virtual ~BrowseHost() = default;
    virtual void browseSelectionChanged() = 0;   // 縮圖區選取的那一格變了（含變成沒有選取）
    virtual void browseActivated(int index) = 0; // 雙擊或 Enter
    virtual void browseFolderChanged() = 0;      // 換了資料夾：標題列
};

// 瀏覽資料夾（目錄瀏覽模式）的三個子視窗：左上的資料夾樹（INameSpaceTreeControl，檔案總管導覽窗格的元件）、
// 右邊的縮圖清單（系統的 ListView，owner data，縮圖自己畫）、檔名列位置的路徑（唯讀的 EDIT，可以選取複製）。
// 左下的預覽由主視窗用 Direct2D 畫。不做任何檔案操作：樹不能拖曳、改名，沒有右鍵選單。規則見 history.md 2026-11-24。
class BrowseView {
public:
    BrowseView();
    ~BrowseView();
    BrowseView(const BrowseView&) = delete;
    BrowseView& operator=(const BrowseView&) = delete;

    // 建三個子視窗（藏著）與背景算縮圖的執行緒。cache 是縮圖快取（沒開也行，就每次重算）。
    bool create(HWND parent, BrowseHost* host, UiScale px, ThumbCache* cache);
    void destroy();
    bool created() const { return list_ != nullptr; }
    void show(bool on);
    // 版面（主視窗的工作區座標）。
    void layout(const RECT& tree, const RECT& list, const RECT& path);
    void setScale(UiScale px);  // 換了 DPI：字型與縮圖大小跟著變
    void setThumbSize(int index); // 0 小、1 中、2 大（core/BrowseList.h 的 kThumbSides）
    // 置頂檔名（設定裡的清單）：下一次列資料夾時套用。
    void setPinned(std::vector<std::wstring> names) { pinned_ = std::move(names); }

    // 換到資料夾 dir，選中 select（空的就不選）。資料夾樹也跟著展開到那裡。
    void navigate(const std::wstring& dir, const std::wstring& select);
    // 重新列這個資料夾（別的程式改了裡面的檔），選取留著；資料夾不在了就退回還在的上一層。
    void refresh();
    // 上一層，選中原本的資料夾。已經是最上層就不動。
    void up();

    const std::wstring& folder() const { return folder_; }       // 檔案系統的路徑；選到「本機」這類虛擬項目時是空的
    const std::wstring& displayName() const { return display_; } // 標題列用的名字
    const std::vector<BrowseEntry>& entries() const { return entries_; }
    int selected() const; // -1 = 沒有選取

    void focusList();
    void cycleFocus();           // Tab：資料夾樹與縮圖清單之間切換
    bool owns(HWND h) const;     // h 是不是這三個子視窗（或它們裡面的視窗）
    HWND listHwnd() const { return list_; }
    HWND pathHwnd() const { return path_; }

    // 主視窗收到的訊息轉過來。
    bool onNotify(NMHDR* nm, LRESULT& result);      // 縮圖清單的通知；不是它的回 false
    HBRUSH onCtlColor(HDC dc, HWND ctl);            // 路徑列的顏色；不是它回 nullptr
    void onThumbsReady();                           // WM_MP_THUMBS_READY：背景算好的縮圖收進來
    static void ackFsChange(WPARAM wp, LPARAM lp);  // WM_MP_BROWSE_FSCHANGE：shell 的通知要 lock / unlock 一次

    // 資料夾樹的事件（TreeEvents 呼叫）。
    void treeSelected(IShellItem* item);
    bool treeKey(UINT msg, WPARAM vk); // 會做檔案操作的鍵吃掉：回 true

private:
    struct Slot {
        HBITMAP bmp = nullptr;
        int w = 0, h = 0;
        bool failed = false;
        ThumbStamp stamp;
    };

    void createTree();
    void syncTree(const std::wstring& dir);
    void setEntries(std::vector<BrowseEntry> entries, const std::wstring& select);
    void watchFolder();
    void requestRange(int from, int to);
    void clearSlots();
    void trimSlots();
    void drawItem(HDC dc, int index);
    void drawShellIcon(HDC dc, const BrowseEntry& e, const RECT& box);
    HICON iconFor(const BrowseEntry& e);
    void applyFonts();
    void applyThumbSize();
    int side() const;

    HWND parent_ = nullptr;
    BrowseHost* host_ = nullptr;
    UiScale px_;
    ThumbCache* cache_ = nullptr;
    HWND list_ = nullptr, path_ = nullptr, treeHwnd_ = nullptr;
    Microsoft::WRL::ComPtr<INameSpaceTreeControl> tree_;
    Microsoft::WRL::ComPtr<IUnknown> treeEvents_;
    DWORD treeCookie_ = 0;
    std::vector<Microsoft::WRL::ComPtr<IShellItem>> roots_;
    bool syncing_ = false;     // 程式自己在選樹上的項目：選取事件不再換資料夾
    HIMAGELIST blankImages_ = nullptr;
    HFONT listFont_ = nullptr, pathFont_ = nullptr;
    HBRUSH pathBrush_ = nullptr;
    int sizeIndex_ = 1;
    ULONG notifyId_ = 0;

    std::wstring folder_, display_;
    std::vector<std::wstring> pinned_;
    std::vector<BrowseEntry> entries_;
    std::map<std::wstring, Slot> slots_; // key 是小寫的路徑
    std::map<int, HICON> icons_;         // 系統圖示清單的編號 → 大圖示

    ThumbQueue queue_;
    uint64_t generation_ = 0;
    std::mutex readyMu_;
    std::vector<std::pair<ThumbJob, ThumbResult>> ready_;
};

} // namespace mp
