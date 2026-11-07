#include <catch2/catch_test_macros.hpp>

#include <windows.h>

#include "core/Shelf.h"
#include "core/TempDir.h"

using namespace mp;

namespace {

std::wstring shelfRoot() {
    wchar_t dir[MAX_PATH];
    GetTempPathW(MAX_PATH, dir);
    std::wstring root = std::wstring(dir) + L"mp_test_shelf";
    removeDirectoryRecursive(root);
    REQUIRE(createDirectories(root));
    return root;
}

void touch(const std::wstring& path, const char* bytes = "x") {
    size_t slash = path.find_last_of(L"\\/");
    createDirectories(path.substr(0, slash));
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    REQUIRE(h != INVALID_HANDLE_VALUE);
    DWORD w = 0;
    WriteFile(h, bytes, static_cast<DWORD>(strlen(bytes)), &w, nullptr);
    CloseHandle(h);
}

} // namespace

TEST_CASE("Shelf：書架 = 直接有圖的子資料夾 + 壓縮檔，自然排序；同層的上一本 / 下一本") {
    std::wstring root = shelfRoot();
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

    CHECK(hasImageFiles(root + L"\\vol.001"));
    CHECK_FALSE(hasImageFiles(root + L"\\onlysub"));
    CHECK_FALSE(hasImageFiles(root + L"\\empty"));
    CHECK_FALSE(hasImageFiles(root + L"\\nowhere"));

    auto books = listBooks(root);
    REQUIRE(books.size() == 6);
    CHECK(books[0] == root + L"\\vol.001");
    CHECK(books[1] == root + L"\\vol.002");
    CHECK(books[2] == root + L"\\vol.003(final)");
    CHECK(books[3] == root + L"\\vol.004.cbz");
    CHECK(books[4] == root + L"\\vol.005.bin");
    CHECK(books[5] == root + L"\\vol.010");

    CHECK(neighborBook(root + L"\\vol.001", +1) == root + L"\\vol.002");
    CHECK(neighborBook(root + L"\\vol.001\\", +1) == root + L"\\vol.002"); // 尾巴有斜線也行
    CHECK(neighborBook(root + L"\\VOL.002", -1) == root + L"\\vol.001");   // 不分大小寫
    CHECK(neighborBook(root + L"\\vol.003(final)", +1) == root + L"\\vol.004.cbz");
    CHECK(neighborBook(root + L"\\vol.004.cbz", -1) == root + L"\\vol.003(final)");
    CHECK(neighborBook(root + L"\\vol.010", +1).empty());  // 最後一本
    CHECK(neighborBook(root + L"\\vol.001", -1).empty());  // 第一本
    CHECK(neighborBook(root + L"\\onlysub", +1).empty());  // 不在書架上
    CHECK(neighborBook(root + L"\\vol.001\\preview", +1).empty()); // 書裡的子資料夾：那一層沒有別本
    CHECK(neighborBook(L"", +1).empty());
    CHECK(neighborBook(L"C:\\", +1).empty());
    CHECK(listBooks(root + L"\\nowhere").empty());
    removeDirectoryRecursive(root);
}
