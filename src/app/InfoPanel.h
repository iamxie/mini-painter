#pragma once
#include <windows.h>
#include <d2d1.h>

#include <set>
#include <string>
#include <vector>

#include "core/Image.h"
#include "core/Metadata.h"

namespace mp {

class Renderer;

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

struct InfoPanelState {
    int tab = 0;            // 0 檔案資訊、1 影像 metadata
    float scroll = 0;       // 目前捲動量（像素）
    float contentHeight = 0;
    std::set<std::wstring> collapsed;

    struct Hit {
        D2D1_RECT_F rect;
        int kind; // 0 tab、1 區塊標題
        int tab;
        std::wstring section;
    };
    std::vector<Hit> hits; // 上次繪製時記錄的可點區域（面板座標）
};

// 畫面板。rect 是面板在視窗裡的範圍。
void drawInfoPanel(Renderer& r, ID2D1RenderTarget* rt, D2D1_RECT_F rect, const InfoData& data, InfoPanelState& st,
                   float uiScale);

// 點擊。pt 是視窗座標。回傳 true 表示要重畫。
bool infoPanelClick(D2D1_RECT_F rect, POINT pt, InfoPanelState& st);

// 捲動。delta 是像素。回傳 true 表示有變化。
bool infoPanelScroll(D2D1_RECT_F rect, float delta, InfoPanelState& st);

} // namespace mp
