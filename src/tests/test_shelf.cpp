#include <catch2/catch_test_macros.hpp>

#include <windows.h>

#include "core/Shelf.h"
#include "core/TempDir.h"
#include "test_util.h"

using namespace mp;

namespace {

void touch(const std::wstring& path, const char* bytes = "x") { mp::test::writeBytes(path, bytes); }

// 一個書架，下面三個測試共用同一種擺法（各自一份，互不影響）。回傳書架的資料夾。
std::wstring makeShelf(const wchar_t* name) {
    std::wstring root = mp::test::freshDir(name);
    // vol.001：有圖、有子資料夾 preview、裡面還有一個 zip（後兩者不算書的一部分，也不是書架上的書）
    touch(root + L"\\vol.001\\1.jpg");
    touch(root + L"\\vol.001\\2.jpg");
    touch(root + L"\\vol.001\\preview\\001.jpg");
    touch(root + L"\\vol.001\\3.zip", "PK\x03\x04junk");
    touch(root + L"\\vol.002\\1.jpg");
    touch(root + L"\\vol.003(final)\\1.jpg");
    touch(root + L"\\vol.010\\1.png");
    touch(root + L"\\vol.004.cbz", "PK\x03\x04junk");           // 副檔名就認
    touch(root + L"\\vol.005.bin", "PK\x03\x04junk");           // 不是圖片副檔名、檔頭是 zip：也算
    touch(root + L"\\notes.txt", "hello");                       // 不是書
    touch(root + L"\\cover.jpg", "notreally");                   // 散圖不是書
    touch(root + L"\\onlysub\\ch1\\1.jpg");                      // 只有子資料夾有圖：不算
    REQUIRE(createDirectories(root + L"\\empty"));
    return root;
}

} // namespace

TEST_CASE("hasImageFiles：資料夾裡直接有圖才算，子資料夾的圖不算") {
    const std::wstring root = makeShelf(L"shelf_has");
    CHECK(hasImageFiles(root + L"\\vol.001"));
    CHECK_FALSE(hasImageFiles(root + L"\\onlysub"));
    CHECK_FALSE(hasImageFiles(root + L"\\empty"));
    CHECK_FALSE(hasImageFiles(root + L"\\nowhere"));
}

TEST_CASE("listBooks：書架 = 直接有圖的子資料夾 + 壓縮檔，自然排序") {
    const std::wstring root = makeShelf(L"shelf_list");
    auto books = listBooks(root);
    REQUIRE(books.size() == 6);
    CHECK(books[0] == root + L"\\vol.001");
    CHECK(books[1] == root + L"\\vol.002");
    CHECK(books[2] == root + L"\\vol.003(final)");
    CHECK(books[3] == root + L"\\vol.004.cbz");
    CHECK(books[4] == root + L"\\vol.005.bin");
    CHECK(books[5] == root + L"\\vol.010");
    CHECK(listBooks(root + L"\\nowhere").empty());
}

TEST_CASE("neighborBook：同一層的上一本 / 下一本") {
    const std::wstring root = makeShelf(L"shelf_neighbor");
    CHECK(neighborBook(root + L"\\vol.001", +1) == root + L"\\vol.002");
    CHECK(neighborBook(root + L"\\vol.003(final)", +1) == root + L"\\vol.004.cbz"); // 資料夾的下一本是壓縮檔
    CHECK(neighborBook(root + L"\\vol.004.cbz", -1) == root + L"\\vol.003(final)");
    SECTION("路徑的寫法不影響：尾巴有斜線、大小寫不同") {
        CHECK(neighborBook(root + L"\\vol.001\\", +1) == root + L"\\vol.002");
        CHECK(neighborBook(root + L"\\VOL.002", -1) == root + L"\\vol.001");
    }
    SECTION("到頭了回空字串") {
        CHECK(neighborBook(root + L"\\vol.010", +1).empty()); // 最後一本
        CHECK(neighborBook(root + L"\\vol.001", -1).empty()); // 第一本
    }
    SECTION("不在書架上的回空字串") {
        CHECK(neighborBook(root + L"\\onlysub", +1).empty());
        CHECK(neighborBook(root + L"\\vol.001\\preview", +1).empty()); // 書裡的子資料夾：那一層沒有別本
        CHECK(neighborBook(L"", +1).empty());
        CHECK(neighborBook(L"C:\\", +1).empty());
    }
}
