#pragma once
#include <windows.h>
#include <d2d1.h>

#include <set>
#include <string>
#include <vector>

#include "app/Theme.h"
#include "core/Image.h"
#include "core/Metadata.h"

namespace mp {

class Surface;

// 資訊面板需要的資料，由主視窗在開檔時準備好。
struct InfoData {
    std::wstring fileName;
    std::wstring filePath;
    unsigned long long fileSize = 0;
    std::wstring created;  // 已格式化
    std::wstring modified;
    ImageInfo info;
    int width = 0, height = 0;
    double dpiX = 96, dpiY = 96;
};

// 從檔案系統補上名稱、大小、日期。
void fillFileFacts(InfoData& d, const std::wstring& path);

// 面板上的一列（標籤與值）與一個可折疊的區塊。字都是翻譯好、可以直接畫的。
struct InfoRow {
    std::wstring label, value;
};
struct InfoSection {
    std::wstring title;
    std::vector<InfoRow> rows;
};

enum class InfoTab { File = 0, Image = 1 }; // 檔案資訊、影像 metadata

// 某個分頁要顯示的區塊：資料 → 畫面上的字。
std::vector<InfoSection> buildInfoSections(const InfoData& d, InfoTab tab);

// 右側的資訊面板。三件事分開：
//   內容（buildInfoSections）只在資料或分頁換了才重組；
//   版面（每一列的位置與高度、內容總高、可視高度）只在內容、面板大小、DPI、折疊狀態變了才重算；
//   draw 只照版面畫。
// 捲動範圍、點擊命中和繪製用的是同一份版面，所以不會對不上。
class InfoPanel {
public:
    // 換一個檔案的資料：內容重組，捲回最上面。
    void setData(InfoData d);
    const InfoData& data() const { return data_; }
    // 編輯改了圖的尺寸或解析度（旋轉、裁切、調整尺寸、undo）。和目前的一樣就什麼都不做。
    void setImageSize(int width, int height, double dpiX, double dpiY);
    // 存檔後檔案的大小、日期變了：只更新這些，捲動位置不動。
    void setFileFacts(const std::wstring& path);

    // 畫在 rect（視窗座標）裡。
    void draw(Surface& surface, D2D1_RECT_F rect, UiScale px);
    // 點擊（視窗座標）：切分頁、折疊或展開區塊。回傳要不要重畫。用的是上一次 draw 的版面。
    bool click(POINT pt);
    // 捲動 delta 個實際像素（正的往下）。回傳有沒有動。
    bool scroll(float delta);

    InfoTab tab() const { return tab_; }
    float scrollPos() const { return scroll_; }
    // 上一次 draw 算出來的：內容總高、內容區看得到的高度（實際像素）。
    float contentHeight() const { return layout_.contentHeight; }
    float viewHeight() const { return layout_.viewHeight; }
    // 上一次 draw 時各區塊標題在視窗裡的位置（含捲出畫面外的），順序同 sections()。內容換了還沒重畫時是空的。
    std::vector<D2D1_RECT_F> headerRects() const;
    const std::vector<InfoSection>& sections() const { return sections_; }
    bool collapsed(const std::wstring& title) const { return collapsed_.count(collapseKey(title)) > 0; }

private:
    struct RowBox {
        float y = 0, height = 0; // 相對於內容的頂端
    };
    struct SectionBox {
        float headerY = 0; // 相對於內容的頂端
        bool collapsed = false;
        std::vector<RowBox> rows; // 折疊時是空的
    };
    struct Layout {
        D2D1_RECT_F rect{};
        float factor = 0;      // 算這份版面時的 DPI 倍率
        D2D1_RECT_F tabs[2]{}; // 兩個分頁的位置（視窗座標）
        float contentTop = 0;  // 內容區的頂端（視窗座標）
        float viewHeight = 0;
        float contentHeight = 0;
        std::vector<SectionBox> sections;
        bool drawn = false; // 畫過了：分頁的位置可以拿來判斷點擊
    };

    std::wstring collapseKey(const std::wstring& title) const;
    void rebuildSections();
    void updateLayout(Surface& surface, D2D1_RECT_F rect, UiScale px);
    float maxScroll() const;

    InfoData data_;
    InfoTab tab_ = InfoTab::File;
    float scroll_ = 0; // 實際像素
    std::set<std::wstring> collapsed_; // 「分頁/區塊標題」
    std::vector<InfoSection> sections_;
    Layout layout_;
    bool layoutStale_ = true; // 內容或折疊狀態變了：下一次 draw 要重算每一列的位置（在那之前區塊標題點不到）
};

} // namespace mp
