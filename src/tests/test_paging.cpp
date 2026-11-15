// core/Paging（配對快取、翻頁目標、預先解碼的清單、標題、壓縮檔內的檔名）、core/Launch（命令列）、
// core/Shelf 的 classifyPath / firstInFolder。
#include <catch2/catch_test_macros.hpp>

#include <windows.h>
#include <shellapi.h>

#include <string>
#include <vector>

#include "core/Launch.h"
#include "core/Paging.h"
#include "core/Shelf.h"
#include "core/TempDir.h"
#include "test_util.h"

using namespace mp;

namespace {
using Files = std::vector<std::wstring>;
Files pages(int n) {
    Files f;
    for (int i = 1; i <= n; ++i) f.push_back(L"C:\\book\\p" + std::to_wstring(i) + L".png");
    return f;
}
} // namespace

// ---- SpreadCache ----

TEST_CASE("SpreadCache：清單與選項沒變就不重算，每個檔只問一次寬高") {
    SpreadCache cache;
    Files files = pages(5);
    int asked = 0;
    auto isWide = [&](const std::wstring& f) {
        ++asked;
        return f == files[2]; // 第三頁是寬圖
    };
    const auto& s1 = cache.get(files, true, false, isWide);
    CHECK(asked == 5);
    // 第一張單獨、第三張寬圖單獨：{0} {1} {2} {3,4}
    REQUIRE(s1.size() == 4);
    CHECK(s1[0].first == 0);
    CHECK(s1[0].alone());
    CHECK(s1[1].first == 1);
    CHECK(s1[2].first == 2);
    CHECK(s1[3].first == 3);
    CHECK(s1[3].second == 4);
    cache.get(files, true, false, isWide);
    CHECK(asked == 5); // 沒重算
    CHECK(&cache.spreads() == &s1);
}

TEST_CASE("SpreadCache：選項、清單內容、清單順序變了都會自己重算（不用人通知）") {
    SpreadCache cache;
    Files files = pages(4);
    int asked = 0;
    auto isWide = [&](const std::wstring&) { ++asked; return false; };
    CHECK(cache.get(files, true, false, isWide).size() == 3);  // {0} {1,2} {3}
    CHECK(cache.get(files, false, false, isWide).size() == 2); // {0,1} {2,3}
    CHECK(cache.get(files, false, true, isWide).size() == 3);  // 錯開一頁：{0} {1,2} {3}
    CHECK(asked == 4); // 改選項不用重讀寬高
    // 清單多了一個檔：只問新的那個
    Files more = files;
    more.push_back(L"C:\\book\\p5.png");
    CHECK(cache.get(more, false, false, isWide).size() == 3);
    CHECK(asked == 5);
    // 同樣的檔、順序不同（置頂檔名改了）：v0.9.4 的 bug 就是 key 只看第一個檔與數量
    Files reordered = {more[0], more[4], more[1], more[2], more[3]};
    int before = asked;
    auto wideLast = [&](const std::wstring& f) { ++asked; return f == more[4]; };
    cache.clear();
    cache.get(more, false, false, wideLast);        // p5 是寬圖，排最後：{0,1} {2,3} {4}
    CHECK(cache.spreads().size() == 3);
    cache.get(reordered, false, false, wideLast);   // p5 搬到第二個：{0} {1} {2,3} {4}
    CHECK(cache.spreads().size() == 4);
    CHECK(cache.spreads()[1].first == 1);
    CHECK(cache.spreads()[1].alone());
    CHECK(asked == before + 5); // 重排不用重讀
    // 換了一本：舊的紀錄不留
    Files other = {L"D:\\other\\a.png", L"D:\\other\\b.png"};
    cache.get(other, false, false, wideLast);
    cache.get(more, false, false, wideLast);
    CHECK(asked == before + 5 + 2 + 5);
}

TEST_CASE("SpreadCache：invalidateFile 重讀那一個檔並重新配對；remember 先記下來就不問") {
    SpreadCache cache;
    Files files = pages(4);
    bool p2Wide = false;
    int asked = 0;
    auto isWide = [&](const std::wstring& f) { ++asked; return f == files[1] && p2Wide; };
    CHECK(cache.get(files, false, false, isWide).size() == 2); // {0,1} {2,3}
    // 第二頁存檔後變成寬圖（旋轉）：不通知的話配對不會變
    p2Wide = true;
    CHECK(cache.get(files, false, false, isWide).size() == 2);
    cache.invalidateFile(L"c:\\BOOK\\P2.PNG"); // 路徑不分大小寫
    const auto& s = cache.get(files, false, false, isWide);
    CHECK(asked == 5); // 只多問了那一個檔
    REQUIRE(s.size() == 3); // {0} {1} {2,3}
    CHECK(s[1].first == 1);
    CHECK(s[1].alone());

    SpreadCache known;
    int asked2 = 0;
    known.remember(files[0], true);
    known.remember(files[1], false);
    known.get(files, false, false, [&](const std::wstring&) { ++asked2; return false; });
    CHECK(asked2 == 2); // 記過的兩個不問
    CHECK(known.spreads()[0].alone()); // 第一頁是記下來的寬圖
    known.clear();
    CHECK(known.spreads().empty());
}

// ---- 翻頁 ----

TEST_CASE("pageTurnTarget：單頁") {
    CHECK(pageTurnTarget(+1, 0, 3, nullptr) == 1);
    CHECK(pageTurnTarget(-1, 2, 3, nullptr) == 1);
    CHECK(pageTurnTarget(+1, 2, 3, nullptr) == -1); // 最後一張
    CHECK(pageTurnTarget(-1, 0, 3, nullptr) == -1); // 第一張
    CHECK(pageTurnTarget(+1, -1, 3, nullptr) == -1); // 不在清單裡
    CHECK(pageTurnTarget(+1, 0, 0, nullptr) == -1);
    CHECK(pageTurnTarget(0, 1, 3, nullptr) == -1);
    CHECK(pageTurnTarget(+5, 0, 3, nullptr) == 1);   // 只看方向
}

TEST_CASE("pageTurnTarget：兩頁並排翻的是一屏") {
    // 6 頁、第一張單獨：{0} {1,2} {3,4} {5}
    auto spreads = buildSpreads(std::vector<bool>(6, false), true, false);
    REQUIRE(spreads.size() == 4);
    CHECK(pageTurnTarget(+1, 0, 6, &spreads) == 1);
    CHECK(pageTurnTarget(+1, 1, 6, &spreads) == 3);
    CHECK(pageTurnTarget(+1, 2, 6, &spreads) == 3); // 從那一屏的第二頁算也一樣
    CHECK(pageTurnTarget(-1, 3, 6, &spreads) == 1);
    CHECK(pageTurnTarget(-1, 1, 6, &spreads) == 0);
    CHECK(pageTurnTarget(+1, 5, 6, &spreads) == -1);
    CHECK(pageTurnTarget(-1, 0, 6, &spreads) == -1);
    std::vector<Spread> empty;
    CHECK(pageTurnTarget(+1, 0, 6, &empty) == -1); // 配對還沒算出來
}

TEST_CASE("wantedPages：翻頁方向的下一屏優先，再來是上一屏；到盡頭回報 atEdge") {
    Files f = pages(5);
    auto w = wantedPages(f, 2, +1, nullptr);
    CHECK_FALSE(w.atEdge);
    REQUIRE(w.others.size() == 2);
    CHECK(w.others[0] == Files{f[3]});
    CHECK(w.others[1] == Files{f[1]});
    w = wantedPages(f, 2, -1, nullptr); // 往回翻：上一張優先
    CHECK(w.others[0] == Files{f[1]});
    CHECK(w.others[1] == Files{f[3]});
    w = wantedPages(f, 4, +1, nullptr); // 最後一張
    CHECK(w.atEdge);
    REQUIRE(w.others.size() == 1);
    CHECK(w.others[0] == Files{f[3]});
    w = wantedPages(f, 0, -1, nullptr); // 第一張往回
    CHECK(w.atEdge);
    CHECK(w.others[0] == Files{f[1]});
    w = wantedPages(pages(1), 0, +1, nullptr);
    CHECK(w.atEdge);
    CHECK(w.others.empty());

    // 兩頁並排：{0} {1,2} {3,4}
    auto spreads = buildSpreads(std::vector<bool>(5, false), true, false);
    w = wantedPages(f, 1, +1, &spreads);
    CHECK_FALSE(w.atEdge);
    REQUIRE(w.others.size() == 2);
    CHECK(w.others[0] == Files{f[3], f[4]});
    CHECK(w.others[1] == Files{f[0]});
    w = wantedPages(f, 4, +1, &spreads);
    CHECK(w.atEdge);
    CHECK(w.others[0] == Files{f[1], f[2]});
    std::vector<Spread> none;
    w = wantedPages(f, 1, +1, &none); // 目前這頁不在配對裡：什麼都不準備
    CHECK_FALSE(w.atEdge);
    CHECK(w.others.empty());
}

// ---- 標題 ----

TEST_CASE("composeTitle") {
    TitleInfo t;
    t.appName = L"mpainter";
    CHECK(composeTitle(t) == L"mpainter"); // 什麼都沒開
    t.profile = L"一般模式";
    CHECK(composeTitle(t) == L"一般模式 - mpainter");
    t.name = L"a.png";
    t.index = 0;
    t.count = 1;
    CHECK(composeTitle(t) == L"a.png - 一般模式 - mpainter"); // 只有一個檔：不顯示頁碼
    t.count = 12;
    t.index = 2;
    CHECK(composeTitle(t) == L"a.png  (3/12) - 一般模式 - mpainter");
    t.dirty = true;
    t.profileModified = true;
    CHECK(composeTitle(t) == L"a.png*  (3/12) - 一般模式* - mpainter");
    t.index = -1; // 不在清單裡
    CHECK(composeTitle(t) == L"a.png* - 一般模式* - mpainter");
    // 兩頁並排
    TitleInfo two;
    two.appName = L"mpainter";
    two.name = L"p2.png";
    two.secondName = L"p3.png";
    two.index = 1;
    two.count = 4;
    two.spreadFirst = 1;
    two.spreadSecond = 2;
    CHECK(composeTitle(two) == L"p2.png | p3.png  (2-3/4) - mpainter");
    two.rightToLeft = true; // 檔名的左右順序跟著畫面，頁碼不變
    two.dirty = true;
    CHECK(composeTitle(two) == L"p3.png | p2.png*  (2-3/4) - mpainter");
    two.spreadFirst = -1; // 不知道在哪一屏：不顯示頁碼
    CHECK(composeTitle(two) == L"p3.png | p2.png* - mpainter");
}

// ---- 壓縮檔內的檔名 ----

TEST_CASE("isInsideDir / archiveEntryName") {
    CHECK(isInsideDir(L"C:\\t\\1", L"C:\\t\\1\\00001.png"));
    CHECK(isInsideDir(L"c:\\T\\1", L"C:\\t\\1/00001.png")); // 不分大小寫、兩種分隔符號
    CHECK_FALSE(isInsideDir(L"C:\\t\\1", L"C:\\t\\1"));     // 自己不算
    CHECK_FALSE(isInsideDir(L"C:\\t\\1", L"C:\\t\\10\\a.png")); // 只是字首相同
    CHECK_FALSE(isInsideDir(L"", L"C:\\a.png"));
    CHECK_FALSE(isInsideDir(L"C:\\t\\1", L"D:\\x.png"));

    Files files = {L"C:\\t\\1\\00001.png", L"C:\\t\\1\\00002.jpg"};
    Files names = {L"ch1/001.png", L"ch1/中文 2.jpg"};
    CHECK(archiveEntryName(L"C:\\t\\1", files, names, L"C:\\t\\1\\00002.jpg") == L"ch1/中文 2.jpg");
    CHECK(archiveEntryName(L"C:\\t\\1", files, names, L"c:\\T\\1\\00001.PNG") == L"ch1/001.png");
    CHECK(archiveEntryName(L"C:\\t\\1", files, names, L"C:\\t\\1\\00009.png") == L"00009.png"); // 對不到：磁碟檔名
    CHECK(archiveEntryName(L"C:\\t\\1", files, names, L"D:\\pics\\a.png").empty());            // 不是壓縮檔內的檔
    CHECK(archiveEntryName(L"", files, names, files[0]).empty());
}

// ---- 命令列 ----

namespace {
// 用系統的規則把命令列拆回去。
std::vector<std::wstring> split(const std::wstring& commandLine) {
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(commandLine.c_str(), &argc);
    REQUIRE(argv);
    std::vector<std::wstring> out(argv, argv + argc);
    LocalFree(argv);
    return out;
}
} // namespace

TEST_CASE("quoteArg：系統拆回來和原本一樣，含結尾的反斜線與引號") {
    for (const wchar_t* arg : {L"C:\\a\\b.png", L"C:\\my pics\\b.png", L"D:\\comics\\", L"D:\\my comics\\", L"say \"hi\".png",
                               L"C:\\a\\\\b", L"ends with \\\\", L"quote at end\"", L"\\\"", L"", L"tab\there", L"中文 路徑\\"}) {
        INFO("arg index: " << std::wstring(arg).size());
        auto argv = split(L"mpainter.exe " + quoteArg(arg));
        REQUIRE(argv.size() == 2);
        CHECK(argv[1] == arg);
    }
    CHECK(quoteArg(L"C:\\a\\b.png") == L"C:\\a\\b.png"); // 不需要引號的不加
    CHECK(quoteArg(L"D:\\my comics\\") == L"\"D:\\my comics\\\\\"");
}

TEST_CASE("openArgs / afterUpdateArgs / parseLaunchArgs：組出來的命令列拆回來是同一組資料") {
    CHECK(openArgs(L"").empty());
    for (const wchar_t* path : {L"C:\\pics\\a.png", L"D:\\my comics\\vol 1.cbz", L"D:\\my comics\\", L""}) {
        // 一般啟動
        LaunchArgs open = parseLaunchArgs(split(quoteArg(L"C:\\Program Files\\mp\\mpainter.exe") + L" " + openArgs(path)));
        CHECK_FALSE(open.afterUpdate);
        CHECK(open.path == path);
        // 自動更新後重啟
        LaunchArgs after = parseLaunchArgs(split(quoteArg(L"C:\\Program Files\\mp\\mpainter.exe") + L" " + afterUpdateArgs(4321, path)));
        CHECK(after.afterUpdate);
        CHECK(after.waitPid == 4321);
        CHECK(after.path == path);
    }
    // v0.9.5 的組法：結尾是反斜線的路徑直接包引號，拆回來多一個引號
    auto broken = split(L"mpainter.exe \"D:\\my comics\\\"");
    CHECK(broken[1] == L"D:\\my comics\"");

    CHECK(parseLaunchArgs({L"mpainter.exe"}).path.empty());
    CHECK(parseLaunchArgs({}).path.empty());
    // --after-update 後面沒有 pid：當成一般的路徑參數（不會發生，但不能當掉）
    LaunchArgs odd = parseLaunchArgs({L"mpainter.exe", L"--after-update"});
    CHECK_FALSE(odd.afterUpdate);
    CHECK(odd.path == L"--after-update");
}

// ---- 開檔時先分類 ----

TEST_CASE("classifyPath / firstInFolder") {
    mp::test::requireArchiveLibrary();
    std::wstring root = mp::test::freshDir(L"classify");
    mp::test::writePng(root + L"\\pics\\b.png", 2, 2);
    mp::test::writePng(root + L"\\pics\\a10.png", 2, 2);
    mp::test::writePng(root + L"\\pics\\a2.png", 2, 2);
    mp::test::writeBytes(root + L"\\book.cbz", "PK\x03\x04junk");
    mp::test::writeBytes(root + L"\\noext", "PK\x03\x04junk");       // 檔頭是 zip
    mp::test::writeBytes(root + L"\\fake.png", "PK\x03\x04junk");    // 圖片副檔名：不看檔頭，當圖片（讀圖時再報錯）
    mp::test::writeBytes(root + L"\\notes.txt", "hello");
    mp::test::writePng(root + L"\\shelf\\vol1\\1.png", 2, 2);
    REQUIRE(createDirectories(root + L"\\empty"));

    CHECK(classifyPath(root + L"\\pics") == PathKind::Folder);
    CHECK(classifyPath(root + L"\\pics\\b.png") == PathKind::Image);
    CHECK(classifyPath(root + L"\\book.cbz") == PathKind::Archive);
    CHECK(classifyPath(root + L"\\noext") == PathKind::Archive);
    CHECK(classifyPath(root + L"\\fake.png") == PathKind::Image);
    CHECK(classifyPath(root + L"\\notes.txt") == PathKind::Image);
    CHECK(classifyPath(root + L"\\nowhere.png") == PathKind::Image); // 不存在：讓讀圖那一步報錯
    CHECK(classifyPath(root + L"\\nowhere.zip") == PathKind::Archive);

    CHECK(firstInFolder(root + L"\\pics") == root + L"\\pics\\a2.png");   // 自然排序：a2 在 a10 前面
    CHECK(firstInFolder(root + L"\\shelf") == root + L"\\shelf\\vol1");   // 沒有散圖：第一本書
    CHECK(firstInFolder(root + L"\\empty").empty());
    CHECK(firstInFolder(root + L"\\nowhere").empty());
    // root 自己：有壓縮檔與資料夾書，沒有支援的散圖……fake.png 是圖片副檔名，所以先開它
    CHECK(firstInFolder(root) == root + L"\\fake.png");
}
