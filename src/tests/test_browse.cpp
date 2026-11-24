// 瀏覽資料夾（目錄瀏覽模式）：清單、路徑的小工具、標題列、指令的可用條件、設定。
#include <catch2/catch_test_macros.hpp>

#include <windows.h>

#include "app/UiText.h"
#include "core/BrowseList.h"
#include "core/Commands.h"
#include "core/Lang.h"
#include "core/PathUtil.h"
#include "core/Settings.h"
#include "test_util.h"

using namespace mp;
using namespace mp::test;

namespace {

std::vector<std::wstring> names(const std::vector<BrowseEntry>& entries) {
    std::vector<std::wstring> out;
    for (const auto& e : entries) out.push_back(e.name);
    return out;
}

} // namespace

TEST_CASE("listBrowseEntries：子資料夾、壓縮檔、圖片依序，各自自然排序；圖片套置頂；隱藏的資料夾與壓縮檔不列") {
    const std::wstring dir = freshDir(L"browse_list");
    for (const wchar_t* sub : {L"b", L"a10", L"a2", L"secret"}) CreateDirectoryW(joinPath(dir, sub).c_str(), nullptr);
    SetFileAttributesW(joinPath(dir, L"secret").c_str(), FILE_ATTRIBUTE_HIDDEN);
    writeBytes(joinPath(dir, L"y.zip"), "PK");
    writeBytes(joinPath(dir, L"x.cbz"), "PK");
    writeBytes(joinPath(dir, L"Z.7z"), "7z");
    writeBytes(joinPath(dir, L"hidden.rar"), "Rar!");
    SetFileAttributesW(joinPath(dir, L"hidden.rar").c_str(), FILE_ATTRIBUTE_HIDDEN);
    writePng(joinPath(dir, L"p10.png"), 2, 2);
    writePng(joinPath(dir, L"p2.png"), 2, 2);
    writePng(joinPath(dir, L"cover.png"), 2, 2);
    writeBytes(joinPath(dir, L"readme.txt"), "x");

    const auto entries = listBrowseEntries(dir, {L"cover.png"});
    CHECK(names(entries) == std::vector<std::wstring>{L"a2", L"a10", L"b", L"x.cbz", L"y.zip", L"Z.7z", L"cover.png", L"p2.png", L"p10.png"});
    CHECK(entries[0].kind == BrowseKind::Folder);
    CHECK(entries[2].kind == BrowseKind::Folder);
    CHECK(entries[3].kind == BrowseKind::Archive);
    CHECK(entries[5].kind == BrowseKind::Archive);
    CHECK(entries[6].kind == BrowseKind::Image);
    CHECK(entries[6].path == joinPath(dir, L"cover.png"));
    // 沒有置頂清單時圖片照自然排序
    CHECK(names(listBrowseEntries(dir, {})).back() == L"p10.png");
    CHECK(names(listBrowseEntries(dir, {}))[6] == L"cover.png");
    // 不存在的資料夾：空的
    CHECK(listBrowseEntries(joinPath(dir, L"nope"), {}).empty());
}

TEST_CASE("indexOfBrowsePath：不分大小寫，沒有回 -1") {
    std::vector<BrowseEntry> e = {{L"C:\\a\\One.png", L"One.png", BrowseKind::Image}, {L"C:\\a\\two", L"two", BrowseKind::Folder}};
    CHECK(indexOfBrowsePath(e, L"c:\\A\\one.PNG") == 0);
    CHECK(indexOfBrowsePath(e, L"C:\\a\\two") == 1);
    CHECK(indexOfBrowsePath(e, L"C:\\a\\three") == -1);
    CHECK(indexOfBrowsePath({}, L"x") == -1);
}

TEST_CASE("browseFolderOf / parentFolder / folderDisplayName：檔案、資料夾、磁碟根目錄、網路分享") {
    const std::wstring dir = freshDir(L"browse_paths");
    writePng(joinPath(dir, L"a.png"), 1, 1);
    CHECK(browseFolderOf(joinPath(dir, L"a.png")) == dir);
    CHECK(browseFolderOf(dir) == dir);
    CHECK(browseFolderOf(dir + L"\\") == dir);
    CHECK(browseFolderOf(L"").empty());
    CHECK(browseFolderOf(L"C:\\nope-mpainter-test.png") == L"C:\\"); // 根目錄留一個反斜線

    CHECK(parentFolder(L"D:\\a\\b") == L"D:\\a");
    CHECK(parentFolder(L"D:\\a\\b\\") == L"D:\\a");
    CHECK(parentFolder(L"D:\\a") == L"D:\\");
    CHECK(parentFolder(L"D:\\").empty());
    CHECK(parentFolder(L"D:").empty());
    CHECK(parentFolder(L"\\\\srv\\share\\x") == L"\\\\srv\\share");
    CHECK(parentFolder(L"\\\\srv\\share").empty());
    CHECK(parentFolder(L"").empty());

    CHECK(folderDisplayName(L"D:\\comic\\金田一") == L"金田一");
    CHECK(folderDisplayName(L"D:\\comic\\金田一\\") == L"金田一");
    CHECK(folderDisplayName(L"D:\\") == L"D:\\");
    CHECK(folderDisplayName(L"D:") == L"D:\\");
    CHECK(folderDisplayName(L"\\\\srv\\share") == L"\\\\srv\\share");
    CHECK(folderDisplayName(L"\\\\srv\\share\\x") == L"x");
}

TEST_CASE("nearestExistingFolder：資料夾被刪掉時退回還在的上一層") {
    const std::wstring dir = freshDir(L"browse_nearest");
    const std::wstring deep = joinPath(joinPath(dir, L"a"), L"b");
    CHECK(nearestExistingFolder(deep) == dir); // a 與 b 都不存在
    CreateDirectoryW(joinPath(dir, L"a").c_str(), nullptr);
    CHECK(nearestExistingFolder(deep) == joinPath(dir, L"a"));
    CHECK(nearestExistingFolder(dir) == dir);
    CHECK(nearestExistingFolder(L"").empty());
}

TEST_CASE("composeBrowseTitle：資料夾名 - 瀏覽資料夾 - mpainter；沒有名字時省略") {
    CHECK(composeBrowseTitle(L"金田一", L"瀏覽資料夾", L"mpainter") == L"金田一 - 瀏覽資料夾 - mpainter");
    CHECK(composeBrowseTitle(L"D:\\", L"Browse Folder", L"mpainter") == L"D:\\ - Browse Folder - mpainter");
    CHECK(composeBrowseTitle(L"", L"Browse Folder", L"mpainter") == L"Browse Folder - mpainter");
}

TEST_CASE("commandEnabled：瀏覽資料夾的開關要有開著的檔；瀏覽時看圖與編輯的指令都不能用") {
    CommandState s;
    CHECK_FALSE(commandEnabled(IDM_BROWSE, s)); // 沒開圖不能進入（使用者 2026-11-24 定）
    s.canBrowse = true;
    CHECK(commandEnabled(IDM_BROWSE, s)); // 圖開不了（hasImage false）也算有開圖
    // 不在瀏覽時縮圖大小不能選
    for (int id = kThumbSizeCommandFirst; id <= kThumbSizeCommandLast; ++id) CHECK_FALSE(commandEnabled(id, s));

    CommandState b;
    b.hasImage = b.hasView = b.dirty = b.canUndo = b.canRedo = b.hasSelection = b.hasPages = b.canBrowse = true;
    b.browsing = true;
    for (int id : {IDM_SAVE, IDM_SAVEAS, IDM_UNDO, IDM_REDO, IDM_DELETE, IDM_ROTATE, IDM_ZOOMIN, IDM_ZOOMOUT, IDM_ZOOM100, IDM_ZOOMFIT,
                   IDM_FITWIDTH, IDM_FITHEIGHT, IDM_TWOPAGE, IDM_UPSCALE, IDM_RTL, IDM_FIRSTALONE, IDM_SHIFTONE, IDM_PREV, IDM_NEXT,
                   IDM_AUTONEXT, IDM_MARKUP}) {
        INFO("id " << id);
        CHECK_FALSE(commandEnabled(id, b));
    }
    const int allowed[] = {IDM_BROWSE, IDM_THUMB_SMALL, IDM_THUMB_MEDIUM, IDM_THUMB_LARGE, IDM_OPEN, IDM_EXIT, IDM_SETTINGS, IDM_INFO,
                           IDM_LANG_SYSTEM, IDM_ABOUT, IDM_PROFILE_MANAGE, IDM_LANG_FIRST, IDM_PROFILE_FIRST + 1};
    for (int id : allowed) {
        INFO("id " << id);
        CHECK(commandEnabled(id, b));
    }
    CHECK_FALSE(commandEnabled(IDM_PROFILE_SAVE, b));
    b.profileModified = true;
    CHECK(commandEnabled(IDM_PROFILE_SAVE, b));
    b.dragging = true; // 拖曳中照樣全部不行
    CHECK_FALSE(commandEnabled(IDM_BROWSE, b));
}

TEST_CASE("指令表：瀏覽資料夾與三個縮圖大小有選單文字，縮圖大小的 id 連號、順序和 kThumbSides 一樣") {
    for (int id : {IDM_BROWSE, IDM_THUMB_SMALL, IDM_THUMB_MEDIUM, IDM_THUMB_LARGE}) {
        INFO("id " << id);
        REQUIRE(commandInfo(id) != nullptr);
        CHECK(commandInfo(id)->shortcut == nullptr); // 快捷鍵併入「快捷鍵總檢討」再定
    }
    CHECK(kThumbSizeCommandLast - kThumbSizeCommandFirst + 1 == kThumbSizeCount);
    CHECK(std::wstring(commandInfo(IDM_THUMB_SMALL)->name) == L"Small");
    CHECK(std::wstring(commandInfo(IDM_THUMB_LARGE)->name) == L"Large");
    CHECK(kThumbSides[0] == 96);
    CHECK(kThumbSides[1] == 160);
    CHECK(kThumbSides[2] == 256);
    CHECK(kThumbSizeDefault == 1);
}

TEST_CASE("Settings：縮圖快取上限預設 500 MB、0 是不存、夾範圍；縮圖大小與版面能往返、壞值夾回") {
    Settings d = parseSettings(L"");
    CHECK(d.thumbCacheMB == 500);
    CHECK(d.browseThumbSize == 1);
    CHECK(d.browseLeftW == 0);
    CHECK(d.browseTreeH == 0);
    Settings s = parseSettings(L"thumbCacheMB=0\nbrowseThumbSize=2\nbrowseLeftW=280\nbrowseTreeH=330\n");
    CHECK(s.thumbCacheMB == 0);
    CHECK(s.browseThumbSize == 2);
    CHECK(s.browseLeftW == 280);
    CHECK(s.browseTreeH == 330);
    const Settings back = parseSettings(serializeSettings(s));
    CHECK(back.thumbCacheMB == 0);
    CHECK(back.browseThumbSize == 2);
    CHECK(back.browseLeftW == 280);
    CHECK(back.browseTreeH == 330);
    CHECK(parseSettings(L"thumbCacheMB=99999999\n").thumbCacheMB == 65536);
    CHECK(parseSettings(L"thumbCacheMB=abc\n").thumbCacheMB == 500);
    CHECK(parseSettings(L"browseThumbSize=7\n").browseThumbSize == 2);
    CHECK(parseSettings(L"browseThumbSize=-1\n").browseThumbSize == 0);
    CHECK(parseSettings(L"browseLeftW=-5\n").browseLeftW == 0);
}

TEST_CASE("mergeSettings：縮圖快取上限、縮圖大小、版面各自是一個欄位，這個視窗改過的才用自己的") {
    Settings base, mine, disk;
    mine.browseThumbSize = 2;      // 這個視窗改了大小
    disk.browseLeftW = 400;        // 別的視窗拖了左欄
    disk.thumbCacheMB = 100;       // 別的視窗改了上限
    const Settings out = mergeSettings(base, mine, disk);
    CHECK(out.browseThumbSize == 2);
    CHECK(out.browseLeftW == 400);
    CHECK(out.thumbCacheMB == 100);
}

TEST_CASE("previewInfoText：寬 × 高、色深、檔案大小，換行接修改日期；色深與日期沒有就不寫") {
    LangGuard guard;
    // 2014-11-10 02:18 UTC 的 FILETIME
    SYSTEMTIME st{2014, 11, 1, 10, 2, 18, 0, 0};
    FILETIME ft{};
    REQUIRE(SystemTimeToFileTime(&st, &ft));
    const long long mtime = static_cast<long long>((static_cast<unsigned long long>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime);
    const std::wstring s = previewInfoText(3960, 2640, 24, 10075381, mtime);
    CHECK(s.find(L"3960 \u00D7 2640") == 0);
    CHECK(s.find(L"24 bit") != std::wstring::npos);
    CHECK(s.find(fileSizeText(10075381)) != std::wstring::npos);
    CHECK(s.find(L"2014") != std::wstring::npos);
    CHECK(s.find(L"\n") != std::wstring::npos); // 日期自己一行
    CHECK(s.find(L"\n") > s.find(fileSizeText(10075381)));
    const std::wstring bare = previewInfoText(10, 20, 0, 5, 0);
    CHECK(bare == L"10 \u00D7 20  \u00B7  " + fileSizeText(5));
    setActiveLang(parseLangText(L"_code=zh-TW\n%s bit=%s 位元\n"));
    CHECK(previewInfoText(1, 1, 32, 1, 0).find(L"32 位元") != std::wstring::npos);
}
