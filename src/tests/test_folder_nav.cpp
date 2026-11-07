#include <catch2/catch_test_macros.hpp>

#include <windows.h>

#include "core/FolderNav.h"

using namespace mp;

TEST_CASE("isSupportedExtension / extensionOf") {
    CHECK(isSupportedExtension(L".JPG"));
    CHECK(isSupportedExtension(L".webp"));
    CHECK_FALSE(isSupportedExtension(L".gif"));
    CHECK(extensionOf(L"C:\\a\\b.PNG") == L".png");
    CHECK(extensionOf(L"C:\\a.b\\noext") == L"");
    CHECK(extensionOf(L"x") == L"");
}

TEST_CASE("isArchiveExtension / archiveExtensions / comicExtensions") {
    CHECK(isArchiveExtension(L".ZIP"));
    CHECK(isArchiveExtension(L".7z"));
    CHECK(isArchiveExtension(L".cbr"));
    CHECK_FALSE(isArchiveExtension(L".jpg"));
    CHECK_FALSE(isArchiveExtension(L".tar"));
    CHECK(archiveExtensions().size() == 6);
    CHECK(comicExtensions().size() == 3);
    for (const auto& e : comicExtensions()) CHECK(isArchiveExtension(e));
    for (const auto& e : archiveExtensions()) CHECK_FALSE(isSupportedExtension(e));
}

TEST_CASE("sortLogical：數字照大小、資料夾分開") {
    std::vector<std::wstring> v = {L"C:\\a\\10.png", L"C:\\b\\1.png", L"C:\\a\\2.png", L"C:\\a\\1.png"};
    sortLogical(v);
    REQUIRE(v.size() == 4);
    CHECK(v[0] == L"C:\\a\\1.png");
    CHECK(v[1] == L"C:\\a\\2.png");
    CHECK(v[2] == L"C:\\a\\10.png");
    CHECK(v[3] == L"C:\\b\\1.png");
}

TEST_CASE("FolderNav 翻頁不循環") {
    FolderNav nav;
    nav.setFiles({L"C:\\x\\1.png", L"C:\\x\\2.png", L"C:\\x\\3.png"}, L"C:\\x\\2.PNG");
    CHECK(nav.index() == 1);
    CHECK(nav.count() == 3);
    CHECK(nav.hasPrev());
    CHECK(nav.hasNext());
    CHECK(nav.next() == L"C:\\x\\3.png");
    CHECK_FALSE(nav.hasNext());
    CHECK(nav.next() == L"C:\\x\\3.png");
    CHECK(nav.prev() == L"C:\\x\\2.png");
    CHECK(nav.prev() == L"C:\\x\\1.png");
    CHECK_FALSE(nav.hasPrev());
}

TEST_CASE("FolderNav 檔案不在清單時 index 為 -1") {
    FolderNav nav;
    nav.setFiles({L"a.png"}, L"b.png");
    CHECK(nav.index() == -1);
    CHECK_FALSE(nav.hasNext());
    CHECK(nav.current().empty());
}

TEST_CASE("listImageFiles 只列支援的格式並依檔案總管排序") {
    wchar_t tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    std::wstring dir = std::wstring(tmp) + L"mp_nav_test";
    CreateDirectoryW(dir.c_str(), nullptr);
    auto touch = [&](const wchar_t* name) {
        HANDLE h = CreateFileW((dir + L"\\" + name).c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
        CloseHandle(h);
    };
    touch(L"img10.png");
    touch(L"img2.jpg");
    touch(L"notes.txt");
    touch(L"IMG1.webp");
    auto files = listImageFiles(dir);
    REQUIRE(files.size() == 3);
    CHECK(files[0].find(L"IMG1.webp") != std::wstring::npos);
    CHECK(files[1].find(L"img2.jpg") != std::wstring::npos);
    CHECK(files[2].find(L"img10.png") != std::wstring::npos);

    FolderNav nav;
    nav.load(dir + L"\\img2.jpg");
    CHECK(nav.index() == 1);

    for (auto* n : {L"img10.png", L"img2.jpg", L"notes.txt", L"IMG1.webp"}) DeleteFileW((dir + L"\\" + n).c_str());
    RemoveDirectoryW(dir.c_str());
}

TEST_CASE("supportedExtensions 與 isDirectoryPath") {
    const auto& exts = supportedExtensions();
    CHECK(exts.size() == 10);
    for (const auto& e : exts) CHECK(isSupportedExtension(e));
    wchar_t tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    CHECK(isDirectoryPath(tmp));
    CHECK_FALSE(isDirectoryPath(std::wstring(tmp) + L"mp_no_such_dir_xyz"));
    std::wstring f = std::wstring(tmp) + L"mp_isdir_file.tmp";
    HANDLE h = CreateFileW(f.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    CloseHandle(h);
    CHECK_FALSE(isDirectoryPath(f));
    DeleteFileW(f.c_str());
}

TEST_CASE("movePinnedFirst：依清單順序置頂、不分大小寫、其餘維持原序") {
    std::vector<std::wstring> v = {L"C:\\a\\1.png", L"C:\\a\\Cover.jpg", L"C:\\b\\1.png", L"C:\\b\\cover.png",
                                   L"C:\\cover.jpg"};
    movePinnedFirst(v, {L"cover.png", L"cover.jpg"});
    REQUIRE(v.size() == 5);
    CHECK(v[0] == L"C:\\b\\cover.png");
    CHECK(v[1] == L"C:\\a\\Cover.jpg");
    CHECK(v[2] == L"C:\\cover.jpg");
    CHECK(v[3] == L"C:\\a\\1.png");
    CHECK(v[4] == L"C:\\b\\1.png");
    auto w = v;
    movePinnedFirst(w, {});
    CHECK(w == v);
    movePinnedFirst(w, {L"nothing.jpg"});
    CHECK(w == v);
}

TEST_CASE("FolderNav::load 套置頂檔名") {
    wchar_t tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    std::wstring dir = std::wstring(tmp) + L"mp_nav_pin";
    CreateDirectoryW(dir.c_str(), nullptr);
    auto touch = [&](const wchar_t* name) {
        HANDLE h = CreateFileW((dir + L"\\" + name).c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
        CloseHandle(h);
    };
    touch(L"001.png");
    touch(L"002.png");
    touch(L"cover.jpg");

    FolderNav nav;
    nav.load(dir + L"\\002.png");
    CHECK(nav.index() == 1); // 沒設置頂：001 002 cover
    CHECK(nav.at(2) == dir + L"\\cover.jpg");

    nav.setPinnedNames({L"COVER.JPG"});
    nav.load(dir + L"\\002.png");
    CHECK(nav.at(0) == dir + L"\\cover.jpg");
    CHECK(nav.index() == 2);

    for (auto* n : {L"001.png", L"002.png", L"cover.jpg"}) DeleteFileW((dir + L"\\" + n).c_str());
    RemoveDirectoryW(dir.c_str());
}

TEST_CASE("movePinnedFirst（paths + names）：用 names 比對，paths 跟著搬") {
    std::vector<std::wstring> paths = {L"C:\\t\\00001.png", L"C:\\t\\00002.jpg", L"C:\\t\\00003.png"};
    std::vector<std::wstring> names = {L"ch1/001.png", L"ch1/Cover.JPG", L"ch1/002.png"};
    movePinnedFirst(paths, names, {L"cover.jpg"});
    REQUIRE(paths.size() == 3);
    CHECK(paths[0] == L"C:\\t\\00002.jpg");
    CHECK(names[0] == L"ch1/Cover.JPG");
    CHECK(paths[1] == L"C:\\t\\00001.png");
    CHECK(names[1] == L"ch1/001.png");
    CHECK(paths[2] == L"C:\\t\\00003.png");
    CHECK(names[2] == L"ch1/002.png");
    std::vector<std::wstring> shorter = {L"x"};
    auto p2 = paths;
    movePinnedFirst(p2, shorter, {L"cover.jpg"}); // 長度不同：不動
    CHECK(p2 == paths);
}

TEST_CASE("sanitizeFileName：Windows 不允許的字元換成 _") {
    CHECK(sanitizeFileName(L"a?b:c*d.png") == L"a_b_c_d.png");
    CHECK(sanitizeFileName(L"ch1/001.png") == L"ch1_001.png");
    CHECK(sanitizeFileName(L"<x>|\"y\"\\z") == L"_x___y__z");
    CHECK(sanitizeFileName(L"normal 名字.jpg") == L"normal 名字.jpg");
    CHECK(sanitizeFileName(L"a\x01" L"b") == L"a_b");
    CHECK(sanitizeFileName(L"").empty());
}
