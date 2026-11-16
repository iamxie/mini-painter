#include <catch2/catch_test_macros.hpp>

#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>


#include "core/Archive.h"
#include "core/Image.h"
#include "core/ImageIO.h"
#include "core/PathUtil.h"
#include "core/TempDir.h"
#include "test_archive_util.h"
#include "test_util.h"

using namespace mp;

namespace {

using mp::test::dllPath;
using mp::test::makeArchive;
using mp::test::pngBytes;
using mp::test::readBytes;
using mp::test::writeBytes;
using Entry = mp::test::ArchiveEntry;
using MakeOptions = mp::test::MakeArchiveOptions;

std::wstring testRoot(const wchar_t* name) { return mp::test::freshDir((std::wstring(L"archive_") + name).c_str()); }

uint32_t crc32Of(const std::string& s) {
    uint32_t c = 0xFFFFFFFFu;
    for (unsigned char ch : s) {
        c ^= ch;
        for (int k = 0; k < 8; ++k) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u)));
    }
    return ~c;
}

void put16(std::string& s, uint32_t v) { s.push_back(static_cast<char>(v & 0xFF)); s.push_back(static_cast<char>((v >> 8) & 0xFF)); }
void put32(std::string& s, uint32_t v) { put16(s, v & 0xFFFF); put16(s, v >> 16); }

// 自己拼一個 stored zip：檔名是原始 bytes（例如 cp932），不設 UTF-8 旗標。bit7z 做不出這種檔，但外面一堆。
void writeStoredZip(const std::wstring& out, const std::vector<std::pair<std::string, std::string>>& entries) {
    std::string local, central;
    for (const auto& e : entries) {
        const std::string& name = e.first;
        const std::string& data = e.second;
        uint32_t crc = crc32Of(data), size = static_cast<uint32_t>(data.size());
        uint32_t offset = static_cast<uint32_t>(local.size());
        put32(local, 0x04034b50); put16(local, 20); put16(local, 0); put16(local, 0); put16(local, 0); put16(local, 0);
        put32(local, crc); put32(local, size); put32(local, size); put16(local, static_cast<uint32_t>(name.size())); put16(local, 0);
        local += name + data;
        put32(central, 0x02014b50); put16(central, 20); put16(central, 20); put16(central, 0); put16(central, 0);
        put16(central, 0); put16(central, 0); put32(central, crc); put32(central, size); put32(central, size);
        put16(central, static_cast<uint32_t>(name.size())); put16(central, 0); put16(central, 0); put16(central, 0);
        put16(central, 0); put32(central, 0); put32(central, offset);
        central += name;
    }
    std::string eocd;
    put32(eocd, 0x06054b50); put16(eocd, 0); put16(eocd, 0);
    put16(eocd, static_cast<uint32_t>(entries.size())); put16(eocd, static_cast<uint32_t>(entries.size()));
    put32(eocd, static_cast<uint32_t>(central.size())); put32(eocd, static_cast<uint32_t>(local.size())); put16(eocd, 0);
    writeBytes(out, local + central + eocd);
}

// 每個項目在檔頭宣稱解開後有 claimed 個位元組，實際的壓縮資料只有兩個位元組（空的 deflate 區塊）。
// 只給「開始解之前看大小」的檢查用：真的去解會失敗。
constexpr uint32_t kHugeClaim = 0xFFFFFFF0u; // 不能是 0xFFFFFFFF：那是 zip64 的記號
void writeHugeClaimZip(const std::wstring& out, int count) {
    const std::string data("\x03\x00", 2);
    const uint32_t csize = static_cast<uint32_t>(data.size());
    constexpr uint32_t kDeflate = 8;
    std::string local, central;
    for (int i = 0; i < count; ++i) {
        char buf[16];
        snprintf(buf, sizeof(buf), "%05d.png", i);
        const std::string name = buf;
        const uint32_t offset = static_cast<uint32_t>(local.size());
        put32(local, 0x04034b50); put16(local, 20); put16(local, 0); put16(local, kDeflate); put16(local, 0); put16(local, 0);
        put32(local, 0); put32(local, csize); put32(local, kHugeClaim); put16(local, static_cast<uint32_t>(name.size())); put16(local, 0);
        local += name + data;
        put32(central, 0x02014b50); put16(central, 20); put16(central, 20); put16(central, 0); put16(central, kDeflate);
        put16(central, 0); put16(central, 0); put32(central, 0); put32(central, csize); put32(central, kHugeClaim);
        put16(central, static_cast<uint32_t>(name.size())); put16(central, 0); put16(central, 0); put16(central, 0);
        put16(central, 0); put32(central, 0); put32(central, offset);
        central += name;
    }
    std::string eocd;
    put32(eocd, 0x06054b50); put16(eocd, 0); put16(eocd, 0);
    put16(eocd, static_cast<uint32_t>(count)); put16(eocd, static_cast<uint32_t>(count));
    put32(eocd, static_cast<uint32_t>(central.size())); put32(eocd, static_cast<uint32_t>(local.size())); put16(eocd, 0);
    writeBytes(out, local + central + eocd);
}

void ensureLib() { mp::test::requireArchiveLibrary(); }

} // namespace

TEST_CASE("extractImages：宣稱的大小超過暫存磁碟的剩餘空間就回 NoSpace，一個檔都不寫") {
    ensureLib();
    std::wstring root = testRoot(L"nospace");
    ULARGE_INTEGER freeBytes{};
    REQUIRE(GetDiskFreeSpaceExW(root.c_str(), &freeBytes, nullptr, nullptr));
    // 項目數照現在的剩餘空間算：總和一定比剩餘空間多（多兩個項目，約 8 GB 的餘裕）。
    const uint64_t count = freeBytes.QuadPart / kHugeClaim + 2;
    REQUIRE(count < 60000); // 一般 zip 的項目數上限是 65535
    const std::wstring zip = root + L"\\huge.zip";
    writeHugeClaimZip(zip, static_cast<int>(count));
    ExtractOptions opt;
    opt.maxTotalBytes = UINT64_MAX; // 不讓總量上限先擋下來
    ArchiveResult r = extractImages(zip, root + L"\\out", opt);
    CHECK(r.status == ArchiveStatus::NoSpace);
    CHECK(r.totalBytes == count * kHugeClaim);
    CHECK(r.files.empty());
    CHECK(mp::test::listImagesRecursive(root + L"\\out").empty());
    // 同一個檔、預設的總量上限：先被上限擋下來，是 TooLarge。
    REQUIRE(count * kHugeClaim > kArchiveMaxTotalBytes);
    CHECK(extractImages(zip, root + L"\\out2", {}).status == ArchiveStatus::TooLarge);
    removeDirectoryRecursive(root);
}

// 7z.dll 還沒載入時的行為。載入之後這個行程就一直是載入的，所以平常不跑；由下面那個測試另開一個行程跑它。
TEST_CASE("nolib：還沒載入 7z.dll 時，要用它的函式都回 NoLibrary，不碰磁碟", "[.nolib]") {
    REQUIRE_FALSE(archiveLibraryLoaded());
    std::wstring root = testRoot(L"nolib");
    const std::wstring zip = root + L"\\a.zip";
    writeStoredZip(zip, {{"a.png", pngBytes(2, 2)}});
    CHECK(extractImages(zip, root + L"\\out", {}).status == ArchiveStatus::NoLibrary);
    CHECK_FALSE(mp::test::exists(root + L"\\out"));
    CHECK(testPassword(zip, L"") == ArchiveStatus::NoLibrary);
    CHECK(extractAll(zip, root + L"\\all").reason == ArchiveFailure::NoLibrary);
    CHECK_FALSE(mp::test::exists(root + L"\\all"));
    CHECK_FALSE(archiveLibraryLoaded()); // 這些函式不會自己去載
    removeDirectoryRecursive(root);
}

TEST_CASE("7z.dll 沒載入：另開一個沒載過的行程，確認回的是 NoLibrary") {
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring cmd = L"\"" + std::wstring(exe) + L"\" \"[nolib]\"";
    STARTUPINFOW si{sizeof(si)};
    PROCESS_INFORMATION pi{};
    REQUIRE(CreateProcessW(exe, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi));
    const DWORD waited = WaitForSingleObject(pi.hProcess, 60000);
    DWORD code = 99;
    GetExitCodeProcess(pi.hProcess, &code);
    if (waited != WAIT_OBJECT_0) TerminateProcess(pi.hProcess, 1);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    REQUIRE(waited == WAIT_OBJECT_0);
    CHECK(code == 0); // 0 = 那個行程裡的測試全過
}

TEST_CASE("isArchiveEntrySkipped") {
    CHECK(isArchiveEntrySkipped(L"__MACOSX/a/001.jpg"));
    CHECK(isArchiveEntrySkipped(L"a/__macosx/001.jpg"));
    CHECK(isArchiveEntrySkipped(L"a/Thumbs.db"));
    CHECK(isArchiveEntrySkipped(L"a\\desktop.ini"));
    CHECK(isArchiveEntrySkipped(L".hidden/001.jpg"));
    CHECK(isArchiveEntrySkipped(L"a/._001.jpg"));
    CHECK_FALSE(isArchiveEntrySkipped(L"a/001.jpg"));
    CHECK_FALSE(isArchiveEntrySkipped(L"vol.1/ch.2/001.jpg")); // 中間有點不算
    CHECK_FALSE(isArchiveEntrySkipped(L"001.jpg"));
}

TEST_CASE("looksLikeArchive：zip / 7z 認得，PNG 與不存在的檔不認") {
    ensureLib();
    std::wstring root = testRoot(L"magic");
    makeArchive(root + L"\\a.zip", bit7z::BitFormat::Zip, {{"x.txt", "hi"}});
    makeArchive(root + L"\\a.7z", bit7z::BitFormat::SevenZip, {{"x.txt", "hi"}});
    CHECK(looksLikeArchive(root + L"\\a.zip"));
    CHECK(looksLikeArchive(root + L"\\a.7z"));
    std::wstring png = root + L"\\p.png";
    Image img(2, 2);
    REQUIRE(saveImage(img, png));
    CHECK_FALSE(looksLikeArchive(png));
    CHECK_FALSE(looksLikeArchive(root + L"\\nothing.zip"));
    removeDirectoryRecursive(root);
}

TEST_CASE("extractImages：zip 攤平、自然排序、略過非圖片與 __MACOSX，解出的 PNG 可讀") {
    ensureLib();
    std::wstring root = testRoot(L"zip");
    std::string png = pngBytes(4, 3);
    makeArchive(root + L"\\book.cbz", bit7z::BitFormat::Zip,
                {{"ch2/1.png", png}, {"ch1/10.png", png}, {"ch1/2.png", png}, {"ch1/notes.txt", "x"},
                 {"__MACOSX/ch1/._2.png", "junk"}, {"ch1/Thumbs.db", "junk"}, {"cover.jpg", "notreallyjpg"}});
    ArchiveResult r = extractImages(root + L"\\book.cbz", root + L"\\out", {});
    REQUIRE(r.status == ArchiveStatus::Ok);
    REQUIRE(r.files.size() == 4);
    REQUIRE(r.names.size() == 4);
    CHECK(r.expected == 4);
    CHECK(r.written == 4);
    // 依壓縮檔內完整路徑自然排序：ch1/2 < ch1/10 < ch2/1 < cover.jpg（c-h < c-o）；磁碟檔名是序號 + 副檔名
    CHECK(r.names[0] == L"ch1/2.png");
    CHECK(r.names[1] == L"ch1/10.png");
    CHECK(r.names[2] == L"ch2/1.png");
    CHECK(r.names[3] == L"cover.jpg");
    CHECK(r.files[0] == root + L"\\out\\00001.png");
    CHECK(r.files[1] == root + L"\\out\\00002.png");
    CHECK(r.files[2] == root + L"\\out\\00003.png");
    CHECK(r.files[3] == root + L"\\out\\00004.jpg");
    CHECK(r.totalBytes == png.size() * 3 + 12);
    CHECK_FALSE(r.solid);
    int w = 0, h = 0;
    CHECK(readImageSize(r.files[1], w, h));
    CHECK(w == 4);
    CHECK(h == 3);
    CHECK(mp::test::listImagesRecursive(root + L"\\out").size() == 4); // 沒有多解出別的東西（notes.txt、__MACOSX）
    CHECK(GetFileAttributesW((root + L"\\out\\ch1").c_str()) == INVALID_FILE_ATTRIBUTES);
    removeDirectoryRecursive(root);
}

TEST_CASE("extractImages：solid 7z") {
    ensureLib();
    std::wstring root = testRoot(L"7z");
    makeArchive(root + L"\\b.7z", bit7z::BitFormat::SevenZip, {{"1.jpg", "aaaa"}, {"2.jpg", "bbbb"}}, {"", false, true});
    ArchiveResult r = extractImages(root + L"\\b.7z", root + L"\\out", {});
    REQUIRE(r.status == ArchiveStatus::Ok);
    CHECK(r.solid);
    CHECK(r.files.size() == 2);
    removeDirectoryRecursive(root);
}

TEST_CASE("extractImages：加密 zip：沒密碼問、錯密碼拒、對密碼過") {
    ensureLib();
    std::wstring root = testRoot(L"enczip");
    makeArchive(root + L"\\e.zip", bit7z::BitFormat::Zip, {{"1.png", pngBytes(2, 2)}}, {"s3cret"});
    CHECK(extractImages(root + L"\\e.zip", root + L"\\o1", {}).status == ArchiveStatus::NeedPassword);
    ExtractOptions wrong;
    wrong.password = L"nope";
    CHECK(extractImages(root + L"\\e.zip", root + L"\\o2", wrong).status == ArchiveStatus::WrongPassword);
    ExtractOptions right;
    right.password = L"s3cret";
    ArchiveResult r = extractImages(root + L"\\e.zip", root + L"\\o3", right);
    REQUIRE(r.status == ArchiveStatus::Ok);
    int w = 0, h = 0;
    CHECK(readImageSize(r.files[0], w, h));
    CHECK(w == 2);
    removeDirectoryRecursive(root);
}

TEST_CASE("extractImages：7z 加密檔頭：列檔前就要密碼") {
    ensureLib();
    std::wstring root = testRoot(L"enc7z");
    makeArchive(root + L"\\e.7z", bit7z::BitFormat::SevenZip, {{"a/1.png", pngBytes(2, 2)}}, {"pw", true, true});
    CHECK(extractImages(root + L"\\e.7z", root + L"\\o1", {}).status == ArchiveStatus::NeedPassword);
    ExtractOptions wrong;
    wrong.password = L"x";
    CHECK(extractImages(root + L"\\e.7z", root + L"\\o2", wrong).status == ArchiveStatus::WrongPassword);
    ExtractOptions right;
    right.password = L"pw";
    ArchiveResult r = extractImages(root + L"\\e.7z", root + L"\\o3", right);
    REQUIRE(r.status == ArchiveStatus::Ok);
    CHECK(r.files.size() == 1);
    CHECK(r.files[0] == root + L"\\o3\\00001.png");
    CHECK(r.names[0] == L"a/1.png");
    removeDirectoryRecursive(root);
}

TEST_CASE("extractImages：不是壓縮檔、沒圖片、太大") {
    ensureLib();
    std::wstring root = testRoot(L"errors");
    std::wstring png = root + L"\\p.png";
    Image img(2, 2);
    REQUIRE(saveImage(img, png));
    CHECK(extractImages(png, root + L"\\o1", {}).status == ArchiveStatus::NotArchive);
    CHECK(extractImages(root + L"\\missing.zip", root + L"\\o1", {}).status == ArchiveStatus::NotArchive);
    makeArchive(root + L"\\t.zip", bit7z::BitFormat::Zip, {{"readme.txt", "x"}});
    CHECK(extractImages(root + L"\\t.zip", root + L"\\o2", {}).status == ArchiveStatus::NoImages);
    makeArchive(root + L"\\big.zip", bit7z::BitFormat::Zip, {{"1.jpg", std::string(4096, 'a')}});
    ExtractOptions limited; // 不能叫 small：Windows 標頭把它定義成 char
    limited.maxTotalBytes = 100;
    CHECK(extractImages(root + L"\\big.zip", root + L"\\o3", limited).status == ArchiveStatus::TooLarge);
    removeDirectoryRecursive(root);
}

TEST_CASE("extractImages：進度回呼會被叫到，回 false 就取消") {
    ensureLib();
    std::wstring root = testRoot(L"cancel");
    std::vector<Entry> entries;
    for (int i = 0; i < 20; ++i) entries.push_back({std::to_string(i) + ".jpg", std::string(200000, static_cast<char>('a' + i))});
    makeArchive(root + L"\\c.zip", bit7z::BitFormat::Zip, entries);
    int calls = 0;
    ExtractOptions ok;
    ok.progress = [&](uint64_t done, uint64_t total) { ++calls; CHECK(done <= total); return true; };
    ArchiveResult r = extractImages(root + L"\\c.zip", root + L"\\o1", ok);
    REQUIRE(r.status == ArchiveStatus::Ok);
    CHECK(calls > 0);
    CHECK(r.files.size() == 20);
    ExtractOptions cancel;
    cancel.progress = [](uint64_t, uint64_t) { return false; };
    CHECK(extractImages(root + L"\\c.zip", root + L"\\o2", cancel).status == ArchiveStatus::Cancelled);
    removeDirectoryRecursive(root);
}

TEST_CASE("testPassword：加密 zip、加密檔頭 7z、solid 7z、沒加密、不是壓縮檔") {
    ensureLib();
    std::wstring root = testRoot(L"testpw");
    std::string png = pngBytes(2, 2);

    makeArchive(root + L"\\e.zip", bit7z::BitFormat::Zip, {{"1.png", png}, {"2.png", png}}, {"s3cret"});
    CHECK(testPassword(root + L"\\e.zip", L"") == ArchiveStatus::NeedPassword);
    CHECK(testPassword(root + L"\\e.zip", L"nope") == ArchiveStatus::WrongPassword);
    CHECK(testPassword(root + L"\\e.zip", L"s3cret") == ArchiveStatus::Ok);

    makeArchive(root + L"\\h.7z", bit7z::BitFormat::SevenZip, {{"a/1.png", png}}, {"pw", true, true});
    CHECK(testPassword(root + L"\\h.7z", L"") == ArchiveStatus::NeedPassword);
    CHECK(testPassword(root + L"\\h.7z", L"x") == ArchiveStatus::WrongPassword);
    CHECK(testPassword(root + L"\\h.7z", L"pw") == ArchiveStatus::Ok);

    makeArchive(root + L"\\s.7z", bit7z::BitFormat::SevenZip, {{"1.png", png}, {"2.png", png}}, {"pw", false, true});
    CHECK(testPassword(root + L"\\s.7z", L"bad") == ArchiveStatus::WrongPassword);
    CHECK(testPassword(root + L"\\s.7z", L"pw") == ArchiveStatus::Ok);

    makeArchive(root + L"\\p.zip", bit7z::BitFormat::Zip, {{"1.png", png}});
    CHECK(testPassword(root + L"\\p.zip", L"") == ArchiveStatus::Ok);
    CHECK(testPassword(root + L"\\p.zip", L"anything") == ArchiveStatus::Ok);

    {
        HANDLE f = CreateFileW((root + L"\\t.txt").c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
        REQUIRE(f != INVALID_HANDLE_VALUE);
        DWORD w = 0;
        WriteFile(f, "hello, not an archive", 21, &w, nullptr);
        CloseHandle(f);
    }
    CHECK(testPassword(root + L"\\t.txt", L"") == ArchiveStatus::NotArchive);
    removeDirectoryRecursive(root);
}

TEST_CASE("extractImages：cp932 檔名、沒有 UTF-8 旗標的 zip：每一張都落地，序號命名不撞名") {
    ensureLib();
    std::wstring root = testRoot(L"cp932");
    std::string png = pngBytes(2, 2);
    // 第１話　ひまわり/001.png、表紙.png、あ.png、い.png、ｱ.png（半形假名，lead byte 會把「.」吃掉）、ascii/003.png
    std::string folder = "\x91\xe6\x82\x50\x98\x62\x81\x40\x82\xd0\x82\xdc\x82\xed\x82\xe8/";
    writeStoredZip(root + L"\\sjis.zip", {
        {folder + "001.png", png},
        {folder + "002.png", png},
        {folder + "\x95\x5c\x8e\x86.png", png},
        {"\x82\xa0.png", png},
        {"\x82\xa2.png", png},
        {"\xb1.png", png},
        {"ascii/003.png", png},
        {"readme", "not an image, no extension"},
    });
    ArchiveResult r = extractImages(root + L"\\sjis.zip", root + L"\\out", {});
    REQUIRE(r.status == ArchiveStatus::Ok);
    REQUIRE(r.files.size() == 7); // 不管系統字碼頁把名字解成什麼，七張都在；readme 不是圖片，解出來看過就丟
    REQUIRE(r.names.size() == 7);
    CHECK(r.expected == 7);
    CHECK(r.written == 7);
    for (const auto& f : r.files) {
        int w = 0, h = 0;
        CHECK(readImageSize(f, w, h));
        CHECK(fileNameOf(f).size() == 9); // 00001.png
        CHECK(f.substr(f.size() - 4) == L".png");
    }
    bool sawAscii = false, sawFolder = false;
    for (const auto& n : r.names) {
        if (n == L"ascii/003.png") sawAscii = true;
        if (n.size() > 8 && n.substr(n.size() - 8) == L"/001.png") sawFolder = true;
    }
    CHECK(sawAscii);
    CHECK(sawFolder);
    CHECK(mp::test::listImagesRecursive(root + L"\\out").size() == 7);
    CHECK(GetFileAttributesW((root + L"\\out\\00008").c_str()) == INVALID_FILE_ATTRIBUTES); // readme 被刪了
    removeDirectoryRecursive(root);
}

TEST_CASE("extractImages：目標檔名被占住（同名的非空資料夾）：那一張略過，其他照解，回 Incomplete") {
    ensureLib();
    std::wstring root = testRoot(L"blocked");
    std::string png = pngBytes(2, 2);
    makeArchive(root + L"\\b.zip", bit7z::BitFormat::Zip, {{"1.png", png}, {"2.png", png}, {"3.png", png}});
    // 空資料夾 bit7z 會刪掉再寫（Overwrite），所以放一個檔讓它刪不掉。
    REQUIRE(createDirectories(root + L"\\out\\00002.png"));
    writeBytes(root + L"\\out\\00002.png\\keep.txt", "x");
    ArchiveResult r = extractImages(root + L"\\b.zip", root + L"\\out", {});
    // bit7z 在第 2 個項目丟例外停下來；第 3 個由補救流程解到記憶體再寫檔。
    REQUIRE(r.status == ArchiveStatus::Incomplete);
    CHECK(r.expected == 3);
    CHECK(r.written == 2);
    REQUIRE(r.files.size() == 2);
    CHECK(r.files[0] == root + L"\\out\\00001.png");
    CHECK(r.files[1] == root + L"\\out\\00003.png");
    CHECK(r.names[0] == L"1.png");
    CHECK(r.names[1] == L"3.png");
    CHECK_FALSE(r.error.empty());
    int w = 0, h = 0;
    CHECK(readImageSize(r.files[1], w, h));
    removeDirectoryRecursive(root);
}

TEST_CASE("extractImages：中間一個項目損毀（CRC 錯）：那一張略過，前後都在，回 Incomplete") {
    ensureLib();
    std::wstring root = testRoot(L"corrupt");
    std::string png = pngBytes(4, 4);
    writeStoredZip(root + L"\\c.zip", {{"1.png", png}, {"2.png", png}, {"3.png", png}});
    // 第 2 個項目的資料：local header 30 bytes + 檔名 5 bytes；第 1 個項目佔 35 + png.size()。
    size_t data2 = (30 + 5 + png.size()) + 30 + 5;
    std::string bytes = readBytes(root + L"\\c.zip");
    for (size_t i = 16; i < 24 && data2 + i < bytes.size(); ++i) bytes[data2 + i] = static_cast<char>(~bytes[data2 + i]);
    writeBytes(root + L"\\c.zip", bytes);
    ArchiveResult r = extractImages(root + L"\\c.zip", root + L"\\out", {});
    REQUIRE(r.status == ArchiveStatus::Incomplete);
    CHECK(r.expected == 3);
    CHECK(r.written == 2);
    REQUIRE(r.files.size() == 2);
    REQUIRE(r.names.size() == 2);
    CHECK(r.files[0] == root + L"\\out\\00001.png");
    CHECK(r.files[1] == root + L"\\out\\00003.png");
    CHECK(r.names[0] == L"1.png");
    CHECK(r.names[1] == L"3.png");
    CHECK(GetFileAttributesW((root + L"\\out\\00002.png").c_str()) == INVALID_FILE_ATTRIBUTES); // 壞的不留在磁碟上
    int w = 0, h = 0;
    CHECK(readImageSize(r.files[0], w, h));
    CHECK(readImageSize(r.files[1], w, h));
    CHECK(w == 4);
    // 三個都壞：一張都解不出來 → Failed
    bytes = readBytes(root + L"\\c.zip");
    for (size_t off : {size_t(35 + 16), data2 + 35 + png.size() + 16}) bytes[off] = static_cast<char>(~bytes[off]);
    writeBytes(root + L"\\all.zip", bytes);
    ArchiveResult r2 = extractImages(root + L"\\all.zip", root + L"\\out2", {});
    CHECK(r2.status == ArchiveStatus::Failed);
    CHECK(r2.files.empty());
    removeDirectoryRecursive(root);
}

// rar 沒有開源 writer，fixture 是 WinRAR 造好放在 src/tests/fixtures/ 的（scripts/make_rar_fixtures.cmd）。
// 三個檔內容相同：a.png、sub/b.png、sub/中文 c.png、Thumbs.db、readme.txt。
std::wstring fixture(const wchar_t* name) { return mp::test::fixturePath(name); }

void checkRarContents(const ArchiveResult& r, const std::wstring& out) {
    REQUIRE(r.status == ArchiveStatus::Ok);
    REQUIRE(r.files.size() == 3);
    REQUIRE(r.names.size() == 3);
    CHECK(r.expected == 3);
    CHECK(r.written == 3);
    CHECK(r.names[0] == L"a.png");
    CHECK(r.names[1] == L"sub/b.png");
    CHECK(r.names[2] == L"sub/中文 c.png"); // RAR5 檔名是 UTF-8，不受系統字碼頁影響
    CHECK(r.files[0] == out + L"\\00001.png");
    CHECK(r.files[1] == out + L"\\00002.png");
    CHECK(r.files[2] == out + L"\\00003.png");
    int w = 0, h = 0;
    CHECK(readImageSize(r.files[0], w, h));
    CHECK(w == 2);
    CHECK(h == 1);
    CHECK(readImageSize(r.files[2], w, h));
    CHECK(w == 3);
    CHECK(mp::test::listImagesRecursive(out).size() == 3); // Thumbs.db、readme.txt 沒解出來
}

TEST_CASE("rar：檔頭辨識、無密碼解壓、子資料夾攤平、中文檔名、略過規則") {
    ensureLib();
    std::wstring root = testRoot(L"rar");
    CHECK(looksLikeArchive(fixture(L"plain.rar")));
    CHECK(looksLikeArchive(fixture(L"hp.rar")));
    checkRarContents(extractImages(fixture(L"plain.rar"), root + L"\\out", {}), root + L"\\out");
    CHECK(testPassword(fixture(L"plain.rar"), L"") == ArchiveStatus::Ok);
    removeDirectoryRecursive(root);
}

TEST_CASE("rar：內容加密（檔頭明文）：沒密碼問、錯密碼拒、對密碼過") {
    ensureLib();
    std::wstring root = testRoot(L"rarpw");
    CHECK(extractImages(fixture(L"pw.rar"), root + L"\\o1", {}).status == ArchiveStatus::NeedPassword);
    ExtractOptions wrong;
    wrong.password = L"nope";
    CHECK(extractImages(fixture(L"pw.rar"), root + L"\\o2", wrong).status == ArchiveStatus::WrongPassword);
    ExtractOptions right;
    right.password = L"123456";
    checkRarContents(extractImages(fixture(L"pw.rar"), root + L"\\o3", right), root + L"\\o3");
    CHECK(testPassword(fixture(L"pw.rar"), L"") == ArchiveStatus::NeedPassword);
    CHECK(testPassword(fixture(L"pw.rar"), L"nope") == ArchiveStatus::WrongPassword);
    CHECK(testPassword(fixture(L"pw.rar"), L"123456") == ArchiveStatus::Ok);
    removeDirectoryRecursive(root);
}

TEST_CASE("rar：檔頭也加密：列檔前就要密碼") {
    ensureLib();
    std::wstring root = testRoot(L"rarhp");
    CHECK(extractImages(fixture(L"hp.rar"), root + L"\\o1", {}).status == ArchiveStatus::NeedPassword);
    ExtractOptions wrong;
    wrong.password = L"x";
    CHECK(extractImages(fixture(L"hp.rar"), root + L"\\o2", wrong).status == ArchiveStatus::WrongPassword);
    ExtractOptions right;
    right.password = L"123456";
    checkRarContents(extractImages(fixture(L"hp.rar"), root + L"\\o3", right), root + L"\\o3");
    CHECK(testPassword(fixture(L"hp.rar"), L"") == ArchiveStatus::NeedPassword);
    CHECK(testPassword(fixture(L"hp.rar"), L"x") == ArchiveStatus::WrongPassword);
    CHECK(testPassword(fixture(L"hp.rar"), L"123456") == ArchiveStatus::Ok);
    removeDirectoryRecursive(root);
}

TEST_CASE("sniffImageExtension：看檔頭認 png / jpg / webp / bmp / tif，其他回空") {
    std::wstring root = testRoot(L"sniff");
    writeBytes(root + L"\\a", pngBytes(2, 2));
    writeBytes(root + L"\\b", std::string("\xFF\xD8\xFF\xE0junk", 8));
    writeBytes(root + L"\\c", std::string("RIFF\x10\0\0\0WEBPVP8 ", 16));
    writeBytes(root + L"\\d", "BMjunkjunk");
    writeBytes(root + L"\\e", std::string("II*\0junk", 8));
    writeBytes(root + L"\\f", std::string("MM\0*junk", 8));
    writeBytes(root + L"\\g", "hello world");
    writeBytes(root + L"\\h", "");
    CHECK(sniffImageExtension(root + L"\\a") == L".png");
    CHECK(sniffImageExtension(root + L"\\b") == L".jpg");
    CHECK(sniffImageExtension(root + L"\\c") == L".webp");
    CHECK(sniffImageExtension(root + L"\\d") == L".bmp");
    CHECK(sniffImageExtension(root + L"\\e") == L".tif");
    CHECK(sniffImageExtension(root + L"\\f") == L".tif");
    CHECK(sniffImageExtension(root + L"\\g").empty());
    CHECK(sniffImageExtension(root + L"\\h").empty());
    CHECK(sniffImageExtension(root + L"\\missing").empty());
    removeDirectoryRecursive(root);
}

TEST_CASE("解不開的原因是代碼，不是英文句子：CRC 錯、不是壓縮檔、每個代碼都有說明") {
    ensureLib();
    std::wstring root = testRoot(L"reasons");
    std::string png = pngBytes(4, 4);
    writeStoredZip(root + L"\\c.zip", {{"1.png", png}, {"2.png", png}});
    std::string bytes = readBytes(root + L"\\c.zip");
    size_t data2 = (30 + 5 + png.size()) + 30 + 5;
    for (size_t i = 16; i < 24; ++i) bytes[data2 + i] = static_cast<char>(~bytes[data2 + i]);
    writeBytes(root + L"\\c.zip", bytes);
    ArchiveResult r = extractImages(root + L"\\c.zip", root + L"\\out", {});
    REQUIRE(r.status == ArchiveStatus::Incomplete);
    CHECK(r.error.reason == ArchiveFailure::CrcError); // 內容被改過：檢查碼不符
    CHECK(r.usable());

    writeBytes(root + L"\\not.zip", "this is not an archive at all");
    r = extractImages(root + L"\\not.zip", root + L"\\out2", {});
    CHECK(r.status == ArchiveStatus::NotArchive);
    CHECK(r.error.reason == ArchiveFailure::InvalidArchive); // 有翻譯的代碼，不是 7-Zip 的英文句子
    CHECK(r.error.detail.empty());
    CHECK_FALSE(r.usable());

    // 解成功：沒有原因
    makeArchive(root + L"\\ok.zip", bit7z::BitFormat::Zip, {{"1.png", png}});
    r = extractImages(root + L"\\ok.zip", root + L"\\out3", {});
    REQUIRE(r.status == ArchiveStatus::Ok);
    CHECK(r.error.empty());

    CHECK(std::wstring(archiveFailureMessage(ArchiveFailure::None)).empty());
    for (ArchiveFailure f : {ArchiveFailure::InvalidArchive, ArchiveFailure::DataError, ArchiveFailure::CrcError, ArchiveFailure::HeadersError, ArchiveFailure::UnexpectedEnd,
                             ArchiveFailure::UnsupportedMethod, ArchiveFailure::CannotCreateFolder, ArchiveFailure::NoLibrary,
                             ArchiveFailure::System, ArchiveFailure::Other}) {
        CAPTURE(static_cast<int>(f));
        CHECK_FALSE(std::wstring(archiveFailureMessage(f)).empty());
    }
    removeDirectoryRecursive(root);
}

TEST_CASE("findPassword：回傳對的那一個的位置；都不對回 -1；試的途中取消算取消（B-24c）") {
    ensureLib();
    std::wstring root = testRoot(L"findpw");
    makeArchive(root + L"\\e.zip", bit7z::BitFormat::Zip, {{"1.png", pngBytes(2, 2)}}, {"s3cret"});
    const std::wstring zip = root + L"\\e.zip";
    bool cancelled = true;
    std::vector<std::pair<int, int>> tried;
    PasswordHooks hooks;
    hooks.onTry = [&](int index, int total) { tried.push_back({index, total}); };
    CHECK(findPassword(zip, {L"a", L"s3cret", L"never"}, hooks, cancelled) == 1);
    CHECK_FALSE(cancelled);
    CHECK(tried == std::vector<std::pair<int, int>>{{1, 3}, {2, 3}}); // 對了就不往下試

    CHECK(findPassword(zip, {L"a", L"b"}, {}, cancelled) == -1);
    CHECK_FALSE(cancelled);
    CHECK(findPassword(zip, {}, {}, cancelled) == -1);
    CHECK_FALSE(cancelled);

    // 一開始就取消：一個都不試
    tried.clear();
    hooks.cancelled = [] { return true; };
    CHECK(findPassword(zip, {L"s3cret"}, hooks, cancelled) == -1);
    CHECK(cancelled);
    CHECK(tried.empty());

    // 試最後一個的途中按了取消（那一個也不對）：以前會被當成「都不對」，接著跳出密碼詢問
    int asked = 0;
    hooks.cancelled = [&] { return ++asked > 2; }; // 試兩個之前各問一次（都沒取消），試完再問就是取消了
    CHECK(findPassword(zip, {L"a", L"b"}, hooks, cancelled) == -1);
    CHECK(cancelled);

    // 取消了但剛好試到對的：算找到（密碼是對的，不必丟掉）
    asked = 0;
    hooks.cancelled = [&] { return ++asked > 1; };
    CHECK(findPassword(zip, {L"s3cret"}, hooks, cancelled) == 0);
    CHECK_FALSE(cancelled);
    removeDirectoryRecursive(root);
}

TEST_CASE("extractWithPasswords：沒加密直接解；加密的試密碼列表；都不對回 NeedPassword 並記下試過了；取消") {
    ensureLib();
    std::wstring root = testRoot(L"withpw");
    std::string png = pngBytes(2, 2);
    makeArchive(root + L"\\plain.zip", bit7z::BitFormat::Zip, {{"1.png", png}});
    makeArchive(root + L"\\e.zip", bit7z::BitFormat::Zip, {{"1.png", png}, {"2.png", png}}, {"s3cret"});

    PasswordExtract x = extractWithPasswords(root + L"\\plain.zip", root + L"\\o1", {}, {L"a", L"b"});
    CHECK(x.result.status == ArchiveStatus::Ok);
    CHECK(x.password.empty());
    CHECK_FALSE(x.listTried); // 不需要密碼：列表沒動過

    int tries = 0;
    PasswordHooks hooks;
    hooks.onTry = [&](int, int) { ++tries; };
    x = extractWithPasswords(root + L"\\e.zip", root + L"\\o2", {}, {L"a", L"s3cret"}, hooks);
    REQUIRE(x.result.status == ArchiveStatus::Ok);
    CHECK(x.password == L"s3cret");
    CHECK(x.listTried);
    CHECK(tries == 2);
    CHECK(x.result.files.size() == 2);

    x = extractWithPasswords(root + L"\\e.zip", root + L"\\o3", {}, {L"a", L"b"});
    CHECK(x.result.status == ArchiveStatus::NeedPassword);
    CHECK(x.listTried);
    CHECK(x.password.empty());
    CHECK(GetFileAttributesW((root + L"\\o3").c_str()) == INVALID_FILE_ATTRIBUTES); // 沒解出東西，資料夾不留

    x = extractWithPasswords(root + L"\\e.zip", root + L"\\o4", {}, {}); // 沒有列表
    CHECK(x.result.status == ArchiveStatus::NeedPassword);
    CHECK_FALSE(x.listTried);

    // 呼叫端自己給了密碼（使用者打的）：對就解，錯就回 WrongPassword，不去試列表
    ExtractOptions typed;
    typed.password = L"s3cret";
    x = extractWithPasswords(root + L"\\e.zip", root + L"\\o5", typed, {L"a"});
    CHECK(x.result.status == ArchiveStatus::Ok);
    CHECK(x.password == L"s3cret");
    CHECK_FALSE(x.listTried);
    typed.password = L"wrong";
    x = extractWithPasswords(root + L"\\e.zip", root + L"\\o6", typed, {L"s3cret"});
    CHECK(x.result.status == ArchiveStatus::WrongPassword);
    CHECK(x.password.empty());

    hooks.cancelled = [] { return true; };
    x = extractWithPasswords(root + L"\\e.zip", root + L"\\o7", {}, {L"s3cret"}, hooks);
    CHECK(x.result.status == ArchiveStatus::Cancelled);
    CHECK_FALSE(x.listTried);
    removeDirectoryRecursive(root);
}

TEST_CASE("loadArchiveLibrary：載過之後再呼叫都回 Ok（不看路徑）") {
    ensureLib();
    LibraryLoad again = loadArchiveLibrary(L"C:\\no\\such\\folder\\7z.dll");
    CHECK(again.ok());
    CHECK(again.status == LibraryLoad::Ok);
    CHECK(archiveLibraryLoaded());
    LibraryLoad unset;
    CHECK_FALSE(unset.ok()); // 預設值是失敗，不是成功
}

TEST_CASE("LibraryLoad：三種結果分得出來") {
    // 這個行程的 7z.dll 已經載入（載過就一律回 Ok），所以沒辦法在這裡真的載一個壞檔；只確認型別本身的行為。
    // 「檔案在但載不進來」的訊息在 VM 桌面看過（docs/regress/messages.ps1）。
    LibraryLoad missing;
    missing.status = LibraryLoad::FileMissing;
    CHECK_FALSE(missing.ok());
    CHECK_FALSE(static_cast<bool>(missing));
    LibraryLoad failed;
    failed.status = LibraryLoad::LoadFailed;
    failed.systemError = 193;
    CHECK_FALSE(failed.ok());
    LibraryLoad ok;
    ok.status = LibraryLoad::Ok;
    CHECK(ok.ok());
    CHECK(static_cast<bool>(ok));
}
