#include "app/InfoPanel.h"

#include <algorithm>
#include <cmath>

#include "app/UiText.h"
#include "core/Lang.h"
#include "render/Surface.h"

namespace mp {

namespace {

// 版面（邏輯像素）。
constexpr float kTabTop = 8;        // 分頁列離面板頂端
constexpr float kTabRowH = 30;      // 分頁列的高度（分頁本身矮 kTabInset）
constexpr float kTabInset = 6;
constexpr float kTabSide = 10;      // 分頁列左右留白
constexpr float kTabRadius = 4;
constexpr float kTabGap = 4;        // 分頁列到分隔線
constexpr float kContentGap = 6;    // 分隔線到內容；每個展開的區塊後面也空這麼多
constexpr float kSide = 12;         // 內容左右留白
constexpr float kLabelW = 110;      // 標籤欄的寬度
constexpr float kLabelGap = 6;      // 標籤和值之間
constexpr float kLineH = 22;        // 一列的最小高度
constexpr float kHeaderExtra = 4;   // 區塊標題比一列多高一點
constexpr float kRowPad = 6;        // 折行的值上下總共多留
constexpr float kValueTop = 3;
constexpr float kTitleIndent = 16;  // 區塊標題在三角形右邊
constexpr float kTitleSize = 13, kRowSize = 12;
constexpr float kArrowStroke = 1.5f;
// 捲軸
constexpr float kThumbMinH = 20, kThumbRight = 3, kThumbLeft = 8, kThumbRadius = 2, kThumbMargin = 2;

std::wstring formatTime(const FILETIME& ft) {
    FILETIME local;
    SYSTEMTIME st;
    if (!FileTimeToLocalFileTime(&ft, &local) || !FileTimeToSystemTime(&local, &st)) return L"";
    wchar_t buf[64];
    swprintf(buf, 64, L"%04u/%02u/%02u %02u:%02u:%02u", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    return buf;
}

bool inRect(const D2D1_RECT_F& r, POINT pt) { return pt.x >= r.left && pt.x < r.right && pt.y >= r.top && pt.y < r.bottom; }

bool sameRect(const D2D1_RECT_F& a, const D2D1_RECT_F& b) {
    return a.left == b.left && a.top == b.top && a.right == b.right && a.bottom == b.bottom;
}

// 區塊標題左邊的三角形：折疊時朝右，展開時朝下。(x, y) 是它的中心。
void drawArrow(Surface& s, UiScale px, float x, float y, bool collapsed) {
    ID2D1RenderTarget* rt = s.rt();
    auto* br = s.brush(theme::kPanelText);
    auto P = [](float px_, float py_) { return D2D1::Point2F(px_, py_); };
    if (collapsed) {
        rt->DrawLine(P(x - px(2), y - px(4)), P(x + px(3), y), br, px(kArrowStroke));
        rt->DrawLine(P(x + px(3), y), P(x - px(2), y + px(4)), br, px(kArrowStroke));
    } else {
        rt->DrawLine(P(x - px(4), y - px(2)), P(x, y + px(3)), br, px(kArrowStroke));
        rt->DrawLine(P(x, y + px(3)), P(x + px(4), y - px(2)), br, px(kArrowStroke));
    }
}

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

std::vector<InfoSection> buildInfoSections(const InfoData& d, InfoTab tab) {
    std::vector<InfoSection> out;
    const std::wstring profile = d.info.colorProfile.empty() ? tr(L"(not embedded)") : d.info.colorProfile;
    if (tab == InfoTab::File) {
        wchar_t buf[128];
        InfoSection general{tr(L"General"), {}};
        general.rows.push_back({tr(L"File name"), d.fileName});
        general.rows.push_back({tr(L"Kind"), tr(imageFormatName(d.info.format))});
        general.rows.push_back({tr(L"File size"), fileSizeText(d.fileSize)});
        general.rows.push_back({tr(L"Date created"), d.created});
        general.rows.push_back({tr(L"Date modified"), d.modified});
        out.push_back(std::move(general));
        InfoSection image{tr(L"Image"), {}};
        swprintf(buf, 128, tr(L"%d × %d pixels"), d.width, d.height);
        image.rows.push_back({tr(L"Image size"), buf});
        swprintf(buf, 128, tr(L"%.0f × %.0f pixels/inch"), d.dpiX, d.dpiY);
        image.rows.push_back({tr(L"Image DPI"), buf});
        image.rows.push_back({tr(L"Color model"), tr(colorModelName(d.info.colorModel))});
        image.rows.push_back({tr(L"Color profile"), profile});
        out.push_back(std::move(image));
        return out;
    }
    InfoSection general{tr(L"General"), {}};
    general.rows.push_back({tr(L"Color model"), tr(colorModelName(d.info.colorModel))});
    general.rows.push_back({tr(L"Depth"), std::to_wstring(d.info.bitsPerPixel) + L" " + tr(L"bits")});
    general.rows.push_back({tr(L"Orientation"), std::to_wstring(d.info.orientation)});
    general.rows.push_back({tr(L"Pixel height"), std::to_wstring(d.height)});
    general.rows.push_back({tr(L"Pixel width"), std::to_wstring(d.width)});
    general.rows.push_back({tr(L"Color profile name"), profile});
    out.push_back(std::move(general));
    for (const auto& s : d.info.sections) {
        InfoSection sec{tr(metaSectionName(s.id)), {}};
        for (const auto& item : s.items) sec.rows.push_back({metaLabelText(item), metaValueText(item.value)});
        out.push_back(std::move(sec));
    }
    return out;
}

// ---- 內容 ----

void InfoPanel::setData(InfoData d) {
    data_ = std::move(d);
    scroll_ = 0;
    rebuildSections();
}

void InfoPanel::setImageSize(int width, int height, double dpiX, double dpiY) {
    if (data_.width == width && data_.height == height && data_.dpiX == dpiX && data_.dpiY == dpiY) return;
    data_.width = width;
    data_.height = height;
    data_.dpiX = dpiX;
    data_.dpiY = dpiY;
    rebuildSections();
}

void InfoPanel::setFileFacts(const std::wstring& path) {
    fillFileFacts(data_, path);
    rebuildSections();
}

void InfoPanel::rebuildSections() {
    sections_ = buildInfoSections(data_, tab_);
    layoutStale_ = true;
}

std::wstring InfoPanel::collapseKey(const std::wstring& title) const {
    return std::to_wstring(static_cast<int>(tab_)) + L"/" + title;
}

// ---- 版面 ----

void InfoPanel::updateLayout(Surface& surface, D2D1_RECT_F rect, UiScale px) {
    if (!layoutStale_ && layout_.drawn && sameRect(layout_.rect, rect) && layout_.factor == px.factor) return;
    Layout l;
    l.rect = rect;
    l.factor = px.factor;
    const float tabW = (rect.right - rect.left - px(2 * kTabSide)) / 2;
    for (int i = 0; i < 2; ++i) {
        const float left = rect.left + px(kTabSide) + i * tabW, top = rect.top + px(kTabTop);
        l.tabs[i] = D2D1::RectF(left, top, left + tabW, top + px(kTabRowH - kTabInset));
    }
    const float divider = rect.top + px(kTabTop + kTabRowH + kTabGap);
    l.contentTop = divider + px(kContentGap);
    l.viewHeight = rect.bottom - l.contentTop;

    const float valueW = (rect.right - px(kSide)) - (rect.left + px(kSide) + px(kLabelW));
    float y = 0;
    for (const InfoSection& sec : sections_) {
        SectionBox box;
        box.headerY = y;
        box.collapsed = collapsed(sec.title);
        y += px(kLineH + kHeaderExtra);
        if (!box.collapsed) {
            for (const InfoRow& row : sec.rows) {
                float h = 0;
                surface.device().measureUiText(row.value, px(kRowSize), false, valueW, &h);
                const float rowH = std::max(px(kLineH), h + px(kRowPad)); // 值可能很長，折行時列跟著變高
                box.rows.push_back({y, rowH});
                y += rowH;
            }
            y += px(kContentGap);
        }
        l.sections.push_back(std::move(box));
    }
    l.contentHeight = y;
    l.drawn = true;
    layout_ = std::move(l);
    layoutStale_ = false;
    // 內容變短了（折疊、換分頁、面板變高）：捲動位置夾回範圍，不留一片空白。
    scroll_ = std::clamp(scroll_, 0.0f, maxScroll());
}

float InfoPanel::maxScroll() const { return std::max(0.0f, layout_.contentHeight - layout_.viewHeight); }

std::vector<D2D1_RECT_F> InfoPanel::headerRects() const {
    std::vector<D2D1_RECT_F> out;
    if (layoutStale_) return out;
    const float h = (kLineH + kHeaderExtra) * layout_.factor;
    for (const SectionBox& box : layout_.sections) {
        const float y = layout_.contentTop - scroll_ + box.headerY;
        out.push_back(D2D1::RectF(layout_.rect.left, y, layout_.rect.right, y + h));
    }
    return out;
}

// ---- 繪製 ----

void InfoPanel::draw(Surface& s, D2D1_RECT_F rect, UiScale px) {
    updateLayout(s, rect, px);
    ID2D1RenderTarget* rt = s.rt();
    rt->SetTransform(D2D1::Matrix3x2F::Identity());
    rt->FillRectangle(rect, s.brush(theme::kPanelBg));
    rt->DrawLine(D2D1::Point2F(rect.left + 0.5f, rect.top), D2D1::Point2F(rect.left + 0.5f, rect.bottom), s.brush(theme::kPanelLine), 1);

    // 分頁
    const wchar_t* tabNames[2] = {tr(L"File Info"), tr(L"Image Info")};
    for (int i = 0; i < 2; ++i) {
        const D2D1_RECT_F& tabRc = layout_.tabs[i];
        if (static_cast<int>(tab_) == i)
            rt->FillRoundedRectangle(D2D1::RoundedRect(tabRc, px(kTabRadius), px(kTabRadius)), s.brush(theme::kSelectedBg));
        s.drawUiText(tabNames[i], tabRc, px(kTitleSize), theme::kPanelText, DWRITE_TEXT_ALIGNMENT_CENTER);
    }
    const float divider = layout_.contentTop - px(kContentGap);
    rt->DrawLine(D2D1::Point2F(rect.left, divider), D2D1::Point2F(rect.right, divider), s.brush(theme::kPanelLine), 1);

    // 內容
    const float top = layout_.contentTop - scroll_;
    const float x0 = rect.left + px(kSide), x1 = rect.right - px(kSide);
    const float headerH = px(kLineH + kHeaderExtra);
    rt->PushAxisAlignedClip(D2D1::RectF(rect.left, divider + 1, rect.right, rect.bottom), D2D1_ANTIALIAS_MODE_ALIASED);
    for (size_t i = 0; i < sections_.size(); ++i) {
        const InfoSection& sec = sections_[i];
        const SectionBox& box = layout_.sections[i];
        const float hy = top + box.headerY;
        drawArrow(s, px, x0 + px(4), hy + headerH / 2, box.collapsed);
        s.drawUiText(sec.title, D2D1::RectF(x0 + px(kTitleIndent), hy, x1, hy + headerH), px(kTitleSize), theme::kPanelText,
                     DWRITE_TEXT_ALIGNMENT_LEADING, true);
        for (size_t r = 0; r < box.rows.size(); ++r) {
            const float y = top + box.rows[r].y, rowH = box.rows[r].height;
            s.drawUiText(sec.rows[r].label, D2D1::RectF(x0, y, x0 + px(kLabelW - kLabelGap), y + rowH), px(kRowSize), theme::kPanelLabel,
                         DWRITE_TEXT_ALIGNMENT_TRAILING, false, DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
            s.drawUiText(sec.rows[r].value, D2D1::RectF(x0 + px(kLabelW), y + px(kValueTop), x1, y + rowH), px(kRowSize),
                         theme::kPanelText, DWRITE_TEXT_ALIGNMENT_LEADING, false, DWRITE_PARAGRAPH_ALIGNMENT_NEAR, true, true);
        }
    }
    rt->PopAxisAlignedClip();

    if (layout_.contentHeight > layout_.viewHeight) {
        // 簡單的捲軸
        const float trackH = layout_.viewHeight - px(2 * kThumbMargin);
        const float thumbH = std::max(px(kThumbMinH), trackH * layout_.viewHeight / layout_.contentHeight);
        const float thumbY = layout_.contentTop + px(kThumbMargin) + (trackH - thumbH) * (scroll_ / maxScroll());
        rt->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(rect.right - px(kThumbLeft), thumbY, rect.right - px(kThumbRight), thumbY + thumbH),
                                                   px(kThumbRadius), px(kThumbRadius)),
                                 s.brush(theme::kHairline));
    }
}

// ---- 互動 ----

bool InfoPanel::click(POINT pt) {
    if (!layout_.drawn) return false;
    for (int i = 0; i < 2; ++i) {
        if (!inRect(layout_.tabs[i], pt)) continue;
        if (static_cast<int>(tab_) != i) {
            tab_ = static_cast<InfoTab>(i);
            scroll_ = 0;
            rebuildSections();
        }
        return true;
    }
    // 區塊標題：只有內容區裡看得到的才點得到（捲到分頁列後面的不算）。
    if (pt.y < layout_.contentTop - kContentGap * layout_.factor || pt.y >= layout_.rect.bottom) return false;
    const auto headers = headerRects();
    for (size_t i = 0; i < headers.size(); ++i) {
        if (!inRect(headers[i], pt)) continue;
        const std::wstring key = collapseKey(sections_[i].title);
        if (!collapsed_.erase(key)) collapsed_.insert(key);
        layoutStale_ = true;
        return true;
    }
    return false;
}

bool InfoPanel::scroll(float delta) {
    const float ns = std::clamp(scroll_ + delta, 0.0f, maxScroll());
    if (ns == scroll_) return false;
    scroll_ = ns;
    return true;
}

} // namespace mp
