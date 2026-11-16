#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "app/InfoPanel.h"
#include "core/Lang.h"
#include "render_fixture.h"
#include "test_util.h"

using namespace mp;
using namespace mp::test;
using Catch::Matchers::WithinAbs;

// 資訊面板畫在記憶體裡的 render target 上（app 層裡不必有視窗的部分）。

namespace {

// 內容一定比面板高的資料：metadata 分頁有三個區塊，共 60 列。
InfoData longData() {
    InfoData d;
    d.fileName = L"photo.jpg";
    d.fileSize = 123456;
    d.width = 4000;
    d.height = 3000;
    d.info.bitsPerPixel = 24;
    for (MetaSectionId id : {MetaSectionId::Tiff, MetaSectionId::Exif, MetaSectionId::Gps}) {
        MetaSection s;
        s.id = id;
        for (int i = 0; i < 20; ++i) {
            MetaItem item;
            item.label = L"Tag " + std::to_wstring(i);
            item.value.text = L"value " + std::to_wstring(i);
            s.items.push_back(item);
        }
        d.info.sections.push_back(s);
    }
    return d;
}

constexpr int kPanelH = 400;
// 面板在 (0, 0)，寬 320 個邏輯像素、高 400 個實際像素。
D2D1_RECT_F panelRect(float ui) { return D2D1::RectF(0, 0, 320 * ui, kPanelH); }

Image drawPanel(RenderFixture& f, InfoPanel& panel, float ui) {
    auto base = whiteImage(static_cast<int>(320 * ui), kPanelH);
    return f.frameOn(base, [&](ID2D1RenderTarget*) { panel.draw(f.r.surface(), panelRect(ui), UiScale{ui}); });
}

// 面板裡內容區的高度：分頁列（含上下留白）以下的部分。分頁列高 48 個邏輯像素。
float expectedViewHeight(float ui) { return kPanelH - 48 * ui; }

POINT centerOf(const D2D1_RECT_F& r) { return {static_cast<LONG>((r.left + r.right) / 2), static_cast<LONG>((r.top + r.bottom) / 2)}; }

// 切到 metadata 分頁（點第二個分頁：在面板右半、分頁列的高度）。
void showImageTab(RenderFixture& f, InfoPanel& panel, float ui) {
    drawPanel(f, panel, ui);
    REQUIRE(panel.click({static_cast<LONG>(240 * ui), static_cast<LONG>(20 * ui)}));
    REQUIRE(panel.tab() == InfoTab::Image);
    drawPanel(f, panel, ui);
}

} // namespace

TEST_CASE("buildInfoSections：檔案分頁兩個區塊，metadata 分頁是一般加檔案裡有的每個區塊") {
    InfoData d = longData();
    auto file = buildInfoSections(d, InfoTab::File);
    REQUIRE(file.size() == 2);
    CHECK(file[0].title == L"General");
    CHECK(file[0].rows[0].label == L"File name");
    CHECK(file[0].rows[0].value == L"photo.jpg");
    CHECK(file[1].title == L"Image");
    CHECK(file[1].rows[0].value == L"4000 × 3000 pixels");
    CHECK(file[1].rows[1].value == L"96 × 96 pixels/inch");
    CHECK(file[1].rows[3].value == L"(not embedded)");

    auto image = buildInfoSections(d, InfoTab::Image);
    REQUIRE(image.size() == 4);
    CHECK(image[0].title == L"General");
    CHECK(image[0].rows[1].value == L"24 bits");
    CHECK(image[2].title == L"Exif");
    REQUIRE(image[2].rows.size() == 20);
    CHECK(image[2].rows[3].label == L"Tag 3");
    CHECK(image[2].rows[3].value == L"value 3");

    d.info.colorProfile = L"sRGB";
    CHECK(buildInfoSections(d, InfoTab::File)[1].rows[3].value == L"sRGB");
}

TEST_CASE("buildInfoSections：字跟著語系翻") {
    LangGuard guard;
    setActiveLang(parseLangText(L"_code=zh-TW\nGeneral=一般\nFile name=檔名\n%d × %d pixels=%d × %d 像素\n"));
    auto file = buildInfoSections(longData(), InfoTab::File);
    CHECK(file[0].title == L"一般");
    CHECK(file[0].rows[0].label == L"檔名");
    CHECK(file[1].rows[0].value == L"4000 × 3000 像素");
}

TEST_CASE("B-22：資訊面板在每個 DPI 倍率都剛好捲得到底") {
    RenderFixture f;
    for (float ui : {1.0f, 1.25f, 1.5f, 2.0f}) {
        InfoPanel panel;
        panel.setData(longData());
        showImageTab(f, panel, ui);
        INFO("ui " << ui);
        CHECK_THAT(panel.viewHeight(), WithinAbs(expectedViewHeight(ui), 0.01f));
        REQUIRE(panel.contentHeight() > panel.viewHeight());
        CHECK(panel.scroll(1e6f)); // 一路捲到底
        // 捲到底時，內容的底邊剛好在面板的底邊：不多不少。
        CHECK_THAT(panel.scrollPos(), WithinAbs(panel.contentHeight() - expectedViewHeight(ui), 0.5f));
        CHECK_FALSE(panel.scroll(100)); // 到底了，再捲不動
        CHECK(panel.scroll(-1e6f));     // 捲回頂
        CHECK(panel.scrollPos() == 0);
        CHECK_FALSE(panel.scroll(-100));
    }
}

TEST_CASE("B-22：捲到底之後折疊區塊，捲動位置夾回內容的範圍") {
    RenderFixture f;
    InfoPanel panel;
    panel.setData(longData());
    showImageTab(f, panel, 1.0f);
    panel.scroll(1e6f);
    panel.scroll(-300); // 往回一點：最後一個區塊的標題進到畫面裡，後面還有 300 像素的內容
    drawPanel(f, panel, 1.0f);
    const float before = panel.contentHeight();

    // 點最後一個區塊的標題把它折起來。
    const auto headers = panel.headerRects();
    REQUIRE(headers.size() == 4);
    REQUIRE(headers.back().top >= 48);
    REQUIRE(headers.back().bottom <= kPanelH);
    REQUIRE(panel.click(centerOf(headers.back())));
    CHECK(panel.collapsed(panel.sections().back().title));
    drawPanel(f, panel, 1.0f);
    REQUIRE(panel.contentHeight() < before);
    // 內容變短了：捲動位置不能留在比新的底還下面的地方（否則面板底下是一片空白）。
    CHECK_THAT(panel.scrollPos(), WithinAbs(panel.contentHeight() - panel.viewHeight(), 0.5f));

    // 再點一次展開：內容回到原本的高度。
    REQUIRE(panel.click(centerOf(panel.headerRects().back())));
    drawPanel(f, panel, 1.0f);
    CHECK(panel.contentHeight() == before);
}

TEST_CASE("InfoPanel：折疊只算在那個分頁；切分頁捲回頂、內容換掉") {
    RenderFixture f;
    InfoPanel panel;
    panel.setData(longData());
    drawPanel(f, panel, 1.0f);
    CHECK(panel.tab() == InfoTab::File);
    REQUIRE(panel.sections().size() == 2);
    const float fileHeight = panel.contentHeight();

    REQUIRE(panel.click(centerOf(panel.headerRects()[0]))); // 折起檔案分頁的「General」
    drawPanel(f, panel, 1.0f);
    CHECK(panel.contentHeight() < fileHeight);

    showImageTab(f, panel, 1.0f);
    REQUIRE(panel.sections().size() == 4);
    CHECK_FALSE(panel.collapsed(L"General")); // metadata 分頁也有一個「General」，它沒有被折
    panel.scroll(200);
    CHECK(panel.scrollPos() == 200);

    REQUIRE(panel.click({80, 20})); // 第一個分頁
    CHECK(panel.tab() == InfoTab::File);
    CHECK(panel.scrollPos() == 0);
    CHECK(panel.collapsed(L"General")); // 折疊的狀態還在
    CHECK(panel.click({80, 20}));       // 點已經選到的分頁：沒事發生，但算點到了
    CHECK(panel.tab() == InfoTab::File);
}

TEST_CASE("InfoPanel：捲到分頁列後面的區塊標題點不到；面板外也點不到") {
    RenderFixture f;
    InfoPanel panel;
    panel.setData(longData());
    showImageTab(f, panel, 1.0f);
    panel.scroll(30); // 第一個標題捲到分頁列後面
    drawPanel(f, panel, 1.0f);
    const auto headers = panel.headerRects();
    REQUIRE(headers[0].bottom < 48);
    CHECK(headers[0].bottom > 40);
    CHECK_FALSE(panel.click({160, 40})); // 分頁和分隔線之間：標題的範圍蓋到這裡，但它被擋住了
    CHECK_FALSE(panel.collapsed(panel.sections()[0].title));
    CHECK_FALSE(panel.click({160, kPanelH + 10}));

    InfoPanel fresh; // 還沒畫過：沒有版面，點不到東西、也捲不動
    fresh.setData(longData());
    CHECK_FALSE(fresh.click({80, 20}));
    CHECK_FALSE(fresh.scroll(100));
}

TEST_CASE("InfoPanel：setImageSize 只在變了才重組；setData 捲回頂；setFileFacts 不動捲動位置") {
    RenderFixture f;
    InfoPanel panel;
    panel.setData(longData());
    showImageTab(f, panel, 1.0f);
    panel.scroll(100);

    const InfoSection* before = panel.sections().data();
    panel.setImageSize(4000, 3000, 96, 96); // 一樣：不重組
    CHECK(panel.sections().data() == before);
    CHECK(panel.sections()[0].rows[4].value == L"4000");

    panel.setImageSize(3000, 4000, 300, 300); // 旋轉後
    CHECK(panel.sections()[0].rows[4].value == L"3000");
    CHECK(panel.data().dpiX == 300);
    CHECK(panel.scrollPos() == 100);

    const std::wstring path = tempPath(L"info_facts.bin");
    writeBytes(path, std::string(321, 'x'));
    panel.setFileFacts(path);
    CHECK(panel.data().fileSize == 321);
    CHECK(panel.data().fileName == L"info_facts.bin");
    CHECK_FALSE(panel.data().modified.empty());
    CHECK(panel.scrollPos() == 100);
    CHECK(panel.data().width == 3000); // 其他的資料不動

    panel.setData(longData());
    CHECK(panel.scrollPos() == 0);
}

TEST_CASE("InfoPanel：畫出來的樣子。選到的分頁有底色、內容超過時有捲軸、值太長會折行") {
    RenderFixture f;
    InfoPanel panel;
    InfoData d = longData();
    d.info.sections[0].items[0].value.text = std::wstring(200, L'W'); // 一定要折成好幾行
    panel.setData(d);
    Image file = drawPanel(f, panel, 1.0f);
    const Color tabOn{199, 217, 242, 255}; // theme::kSelectedBg
    CHECK(isNear(pixel(file, 20, 14), tabOn, 6));        // 第一個分頁：選到的
    CHECK_FALSE(isNear(pixel(file, 300, 14), tabOn, 6)); // 第二個沒有
    CHECK(countNear(file, {0, 0, 0, 255}, {312, 48, 318, 400}, 200) == 0); // 內容比面板短：沒有捲軸

    showImageTab(f, panel, 1.0f);
    Image image = drawPanel(f, panel, 1.0f);
    CHECK(isNear(pixel(image, 300, 14), tabOn, 6));
    CHECK(countNear(image, {180, 180, 180, 255}, {312, 48, 318, 400}, 30) > 20); // 捲軸（半透明的黑疊在面板底色上）

    // 折行的那一列比別的列高：同樣 60 列，內容比每個值都很短時高。
    InfoPanel plain;
    plain.setData(longData());
    showImageTab(f, plain, 1.0f);
    CHECK(panel.contentHeight() > plain.contentHeight() + 22);
}
