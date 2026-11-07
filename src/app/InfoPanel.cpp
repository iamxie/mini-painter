#include "app/InfoPanel.h"

#include <algorithm>
#include <cmath>

#include "core/Lang.h"
#include "render/Renderer.h"

namespace mp {

namespace {

std::wstring formatTime(const FILETIME& ft) {
    FILETIME local;
    SYSTEMTIME st;
    if (!FileTimeToLocalFileTime(&ft, &local) || !FileTimeToSystemTime(&local, &st)) return L"";
    wchar_t buf[64];
    swprintf(buf, 64, L"%04u/%02u/%02u %02u:%02u:%02u", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    return buf;
}

const D2D1_COLOR_F kPanelBg = D2D1::ColorF(0.97f, 0.97f, 0.97f);
const D2D1_COLOR_F kLine = D2D1::ColorF(0.82f, 0.82f, 0.82f);
const D2D1_COLOR_F kLabel = D2D1::ColorF(0.45f, 0.45f, 0.45f);
const D2D1_COLOR_F kValue = D2D1::ColorF(0.12f, 0.12f, 0.12f);
const D2D1_COLOR_F kTabOn = D2D1::ColorF(0.78f, 0.85f, 0.95f);

struct Row {
    std::wstring label, value;
};

} // namespace

void fillFileFacts(InfoData& d, const std::wstring& path) {
    d.filePath = path;
    size_t s = path.find_last_of(L"\\/");
    d.fileName = s == std::wstring::npos ? path : path.substr(s + 1);
    WIN32_FILE_ATTRIBUTE_DATA fa{};
    if (GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fa)) {
        d.fileSize = (static_cast<unsigned long long>(fa.nFileSizeHigh) << 32) | fa.nFileSizeLow;
        d.created = formatTime(fa.ftCreationTime);
        d.modified = formatTime(fa.ftLastWriteTime);
    }
}

void drawInfoPanel(Renderer& r, ID2D1RenderTarget* rt, D2D1_RECT_F rect, const InfoData& data, InfoPanelState& st,
                   float ui) {
    auto px = [ui](float v) { return v * ui; };
    st.hits.clear();
    rt->SetTransform(D2D1::Matrix3x2F::Identity());
    rt->FillRectangle(rect, r.brushFor(rt, kPanelBg));
    rt->DrawLine(D2D1::Point2F(rect.left + 0.5f, rect.top), D2D1::Point2F(rect.left + 0.5f, rect.bottom), r.brushFor(rt, kLine), 1);

    // 分頁
    float tabH = px(30), tabW = (rect.right - rect.left - px(20)) / 2;
    const wchar_t* tabNames[2] = {tr(L"File Info"), tr(L"Image Info")};
    for (int i = 0; i < 2; ++i) {
        D2D1_RECT_F tr = D2D1::RectF(rect.left + px(10) + i * tabW, rect.top + px(8), rect.left + px(10) + (i + 1) * tabW, rect.top + px(8) + tabH - px(6));
        if (st.tab == i) rt->FillRoundedRectangle(D2D1::RoundedRect(tr, px(4), px(4)), r.brushFor(rt, kTabOn));
        r.drawUiText(rt, tabNames[i], tr, px(13), kValue, DWRITE_TEXT_ALIGNMENT_CENTER);
        st.hits.push_back({tr, 0, i, L""});
    }
    float top = rect.top + px(8) + tabH + px(4);
    rt->DrawLine(D2D1::Point2F(rect.left, top), D2D1::Point2F(rect.right, top), r.brushFor(rt, kLine), 1);

    // 內容
    std::vector<std::pair<std::wstring, std::vector<Row>>> sections;
    wchar_t buf[128];
    if (st.tab == 0) {
        std::vector<Row> rows;
        rows.push_back({tr(L"File name"), data.fileName});
        rows.push_back({tr(L"Kind"), data.info.formatName});
        rows.push_back({tr(L"File size"), formatFileSize(data.fileSize)});
        rows.push_back({tr(L"Date created"), data.created});
        rows.push_back({tr(L"Date modified"), data.modified});
        sections.push_back({tr(L"General"), rows});
        rows.clear();
        swprintf(buf, 128, tr(L"%d × %d pixels"), data.width, data.height);
        rows.push_back({tr(L"Image size"), buf});
        swprintf(buf, 128, tr(L"%.0f × %.0f pixels/inch"), data.dpiX, data.dpiY);
        rows.push_back({tr(L"Image DPI"), buf});
        rows.push_back({tr(L"Color model"), data.info.colorModel});
        rows.push_back({tr(L"Color profile"), data.info.colorProfile.empty() ? tr(L"(not embedded)") : data.info.colorProfile});
        sections.push_back({tr(L"Image"), rows});
    } else {
        std::vector<Row> rows;
        rows.push_back({tr(L"Color model"), data.info.colorModel});
        rows.push_back({tr(L"Depth"), std::to_wstring(data.info.bitsPerPixel) + L" " + tr(L"bits")});
        rows.push_back({tr(L"Orientation"), std::to_wstring(data.info.orientation)});
        rows.push_back({tr(L"Pixel height"), std::to_wstring(data.height)});
        rows.push_back({tr(L"Pixel width"), std::to_wstring(data.width)});
        rows.push_back({tr(L"Color profile name"), data.info.colorProfile.empty() ? tr(L"(not embedded)") : data.info.colorProfile});
        sections.push_back({tr(L"General"), rows});
        for (const auto& s : data.info.sections) {
            std::vector<Row> rs;
            for (const auto& kv : s.items) rs.push_back({kv.first, kv.second});
            sections.push_back({s.name, rs});
        }
    }

    float contentTop = top + px(6);
    float y = contentTop - st.scroll;
    float labelW = px(110), lineH = px(22), x0 = rect.left + px(12), x1 = rect.right - px(12);
    rt->PushAxisAlignedClip(D2D1::RectF(rect.left, top + 1, rect.right, rect.bottom), D2D1_ANTIALIAS_MODE_ALIASED);
    for (const auto& sec : sections) {
        bool collapsed = st.collapsed.count(std::to_wstring(st.tab) + L"/" + sec.first) > 0;
        D2D1_RECT_F hr = D2D1::RectF(rect.left, y, rect.right, y + lineH + px(4));
        // 三角形
        float tx = x0 + px(4), ty = y + (lineH + px(4)) / 2;
        auto* br = r.brushFor(rt, kValue);
        if (collapsed) {
            rt->DrawLine(D2D1::Point2F(tx - px(2), ty - px(4)), D2D1::Point2F(tx + px(3), ty), br, px(1.5f));
            rt->DrawLine(D2D1::Point2F(tx + px(3), ty), D2D1::Point2F(tx - px(2), ty + px(4)), br, px(1.5f));
        } else {
            rt->DrawLine(D2D1::Point2F(tx - px(4), ty - px(2)), D2D1::Point2F(tx, ty + px(3)), br, px(1.5f));
            rt->DrawLine(D2D1::Point2F(tx, ty + px(3)), D2D1::Point2F(tx + px(4), ty - px(2)), br, px(1.5f));
        }
        r.drawUiText(rt, sec.first, D2D1::RectF(x0 + px(16), y, x1, y + lineH + px(4)), px(13), kValue, DWRITE_TEXT_ALIGNMENT_LEADING, true);
        st.hits.push_back({hr, 1, st.tab, sec.first});
        y += lineH + px(4);
        if (collapsed) continue;
        for (const auto& row : sec.second) {
            float h = 0;
            float valueW = x1 - (x0 + labelW);
            r.measureUiText(row.value, px(12), false, valueW, &h);
            float rowH = std::max(lineH, h + px(6));
            r.drawUiText(rt, row.label, D2D1::RectF(x0, y, x0 + labelW - px(6), y + rowH), px(12), kLabel, DWRITE_TEXT_ALIGNMENT_TRAILING, false,
                         DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
            // 值可能很長，允許折行
            IDWriteTextFormat* fmt = r.uiFormat(px(12), false);
            fmt->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
            fmt->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
            fmt->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
            rt->DrawText(row.value.c_str(), static_cast<UINT32>(row.value.size()), fmt,
                         D2D1::RectF(x0 + labelW, y + px(3), x1, y + rowH), r.brushFor(rt, kValue), D2D1_DRAW_TEXT_OPTIONS_CLIP);
            y += rowH;
        }
        y += px(6);
    }
    rt->PopAxisAlignedClip();
    st.contentHeight = (y + st.scroll) - contentTop;
    float viewH = rect.bottom - contentTop;
    if (st.contentHeight > viewH) {
        // 簡單的捲軸
        float trackH = viewH - px(4);
        float thumbH = std::max(px(20), trackH * viewH / st.contentHeight);
        float thumbY = contentTop + px(2) + (trackH - thumbH) * (st.scroll / (st.contentHeight - viewH));
        rt->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(rect.right - px(8), thumbY, rect.right - px(3), thumbY + thumbH), px(2), px(2)),
                                 r.brushFor(rt, D2D1::ColorF(0, 0, 0, 0.25f)));
    }
}

bool infoPanelClick(D2D1_RECT_F, POINT pt, InfoPanelState& st) {
    for (const auto& h : st.hits) {
        if (pt.x >= h.rect.left && pt.x < h.rect.right && pt.y >= h.rect.top && pt.y < h.rect.bottom) {
            if (h.kind == 0) {
                if (st.tab != h.tab) { st.tab = h.tab; st.scroll = 0; }
                return true;
            }
            std::wstring key = std::to_wstring(h.tab) + L"/" + h.section;
            if (st.collapsed.count(key)) st.collapsed.erase(key);
            else st.collapsed.insert(key);
            return true;
        }
    }
    return false;
}

bool infoPanelScroll(D2D1_RECT_F rect, float delta, InfoPanelState& st) {
    float viewH = rect.bottom - rect.top - 60;
    float maxScroll = std::max(0.0f, st.contentHeight - viewH);
    float ns = std::clamp(st.scroll + delta, 0.0f, maxScroll);
    if (ns == st.scroll) return false;
    st.scroll = ns;
    return true;
}

} // namespace mp
