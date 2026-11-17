#include <catch2/catch_test_macros.hpp>

#include <windows.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>


#include "core/Archive.h"
#include "core/FolderNav.h"
#include "core/ImageIO.h"
#include "core/PathUtil.h"
#include "core/Prefetch.h"
#include "core/TempDir.h"
#include "test_archive_util.h"
#include "test_util.h"

using namespace mp;

namespace {

using mp::test::dllPath;
using mp::test::exists;
using mp::test::readBytes;
using mp::test::writePng;

std::wstring testRoot(const wchar_t* name) { return mp::test::freshDir((std::wstring(L"prefetch_") + name).c_str()); }

// 換本預備把壓縮檔解到暫存位置底下的一個子資料夾（名字是 BookPreparer 自己取的，測試不管它叫什麼）。
// dir 底下現在有哪些子資料夾，完整路徑。
std::vector<std::wstring> unpackDirs(const std::wstring& dir) {
    std::vector<std::wstring> out;
    forEachDirEntry(dir, [&](const DirEntry& e) {
        if (e.directory) out.push_back(dir + L"\\" + e.name);
        return true;
    });
    return out;
}

// zip：name → 內容（PNG bytes）。password 非空時加密內容。
void makeZip(const std::wstring& out, const std::vector<std::pair<std::string, std::string>>& entries,
             const std::string& password = "") {
    mp::test::requireArchiveLibrary();
    std::vector<mp::test::ArchiveEntry> list;
    for (const auto& [name, content] : entries) list.push_back({name, content});
    mp::test::MakeArchiveOptions mo;
    mo.password = password;
    mp::test::makeArchive(out, bit7z::BitFormat::Zip, list, mo);
}

// 會計數、可以卡住的解碼器。
struct Gate {
    std::mutex m;
    std::condition_variable cv;
    bool open = true;
    int entered = 0;
    void pass() {
        std::unique_lock<std::mutex> lk(m);
        ++entered;
        cv.notify_all();
        cv.wait(lk, [&] { return open; });
    }
    void close() {
        std::lock_guard<std::mutex> lk(m);
        open = false;
    }
    void release() {
        {
            std::lock_guard<std::mutex> lk(m);
            open = true;
        }
        cv.notify_all();
    }
    void waitEntered(int n) {
        std::unique_lock<std::mutex> lk(m);
        cv.wait(lk, [&] { return entered >= n; });
    }
};

struct Counting {
    std::atomic<int> count{0};
    Gate gate;
    Prefetcher::Decoder decoder() {
        return [this](const std::wstring& p) {
            ++count;
            gate.pass();
            return loadImage(p);
        };
    }
};

constexpr size_t kBig = size_t(1) << 30;

TargetSizes halfSize() {
    return [](const std::vector<Size2>& pages) {
        std::vector<Size2> out;
        for (auto [w, h] : pages) out.push_back({w / 2, h / 2});
        return out;
    };
}

} // namespace

TEST_CASE("Prefetch：背景先解好其他屏，take 直接給、不再解；current 不由背景解") {
    std::wstring root = testRoot(L"basic");
    for (int i = 1; i <= 4; ++i) writePng(root + L"\\" + std::to_wstring(i) + L".png", 40, 60);
    std::wstring p1 = root + L"\\1.png", p2 = root + L"\\2.png", p3 = root + L"\\3.png", p4 = root + L"\\4.png";
    Counting c;
    Prefetcher pf(c.decoder());
    pf.setWanted({p1}, {{p2, p3}, {p4}}, std::nullopt, {}, kBig);
    pf.waitIdle();
    CHECK(c.count == 3);
    auto r = pf.get(p2);
    REQUIRE(r);
    REQUIRE(r->image);
    CHECK(r->image->width == 40);
    CHECK(c.count == 3);
    CHECK_FALSE(pf.get(p1)); // current 是呼叫端自己解的
    CHECK(pf.heldBytes() == 3 * 40 * 60 * 4);
}

TEST_CASE("Prefetch：不在清單裡的立刻放掉；put 進來的 current 不算上限，翻走後當上一屏留著") {
    std::wstring root = testRoot(L"prune");
    for (int i = 1; i <= 3; ++i) writePng(root + L"\\" + std::to_wstring(i) + L".png", 50, 50);
    std::wstring p1 = root + L"\\1.png", p2 = root + L"\\2.png", p3 = root + L"\\3.png";
    Counting c;
    Prefetcher pf(c.decoder());
    LoadResult cur = loadImage(p1);
    std::weak_ptr<const Image> w1 = cur.image;
    pf.put(p1, cur);
    pf.setWanted({p1}, {{p2}}, std::nullopt, {}, kBig);
    pf.waitIdle();
    CHECK(pf.heldBytes() == 50 * 50 * 4);
    std::weak_ptr<const Image> w2 = pf.get(p2)->image;
    // 翻到 2：1 變成上一屏、3 是下一屏
    pf.setWanted({p2}, {{p3}, {p1}}, std::nullopt, {}, kBig);
    pf.waitIdle();
    cur = {};
    CHECK_FALSE(w1.expired());
    CHECK(c.count == 2);
    auto back = pf.get(p1);
    REQUIRE(back);
    CHECK(back->image == w1.lock());
    back.reset();
    // 跳到別的地方：全部放掉
    pf.setWanted({}, {}, std::nullopt, {}, kBig);
    CHECK(w1.expired());
    CHECK(w2.expired());
    CHECK(pf.heldBytes() == 0);
}

TEST_CASE("Prefetch：上限依優先順序，放不下的那一屏起都不留") {
    std::wstring root = testRoot(L"limit");
    for (int i = 1; i <= 4; ++i) writePng(root + L"\\" + std::to_wstring(i) + L".png", 100, 100); // 一張 40000 bytes
    std::wstring p1 = root + L"\\1.png", p2 = root + L"\\2.png", p3 = root + L"\\3.png", p4 = root + L"\\4.png";
    Counting c;
    Prefetcher pf(c.decoder());
    pf.setWanted({p1}, {{p2, p3}, {p4}}, std::nullopt, {}, 100000);
    pf.waitIdle();
    CHECK(pf.heldBytes() == 80000);
    CHECK(pf.get(p2));
    CHECK(pf.get(p3));
    CHECK_FALSE(pf.get(p4));
    // 上限改小：優先順序低的先丟（這裡只剩第一屏也放不下，整屏丟）。
    // 先把解碼器卡住：setWanted 之後背景會馬上重解，不卡住的話下面那一行有時看到的是重解後的量。
    c.gate.close();
    pf.setWanted({p1}, {{p2, p3}, {p4}}, std::nullopt, {}, 50000);
    CHECK(pf.heldBytes() == 0);
    const int decodedBefore = c.count;
    c.gate.release();
    pf.waitIdle();
    // 背景不會回頭只解半屏：開始解一屏之前先看檔頭估整屏的大小，放不下就不解（以前會留下第一頁，reflection.md N-04）。
    CHECK(pf.heldBytes() == 0);
    CHECK(c.count == decodedBefore);
    CHECK_FALSE(pf.get(p2));
    CHECK_FALSE(pf.get(p3));
    // 上限剛好放得下一屏：整屏都留
    pf.setWanted({p1}, {{p2, p3}, {p4}}, std::nullopt, {}, 80000);
    pf.waitIdle();
    CHECK(pf.heldBytes() == 80000);
    CHECK(pf.get(p3));
    CHECK_FALSE(pf.get(p4));
}

TEST_CASE("Prefetch：單張就超過上限時看檔頭就停手，連解都不解；上限 0 是關閉") {
    std::wstring root = testRoot(L"limit_single");
    const std::wstring page = root + L"\\1.png";
    writePng(page, 100, 100); // 40000 bytes
    Counting c;
    Prefetcher pf(c.decoder());
    pf.setWanted({}, {{page}}, std::nullopt, {}, 1000);
    pf.waitIdle();
    CHECK(c.count == 0);
    CHECK(pf.heldBytes() == 0);
    CHECK_FALSE(pf.get(page));
    // 上限 0 = 關閉
    pf.setWanted({}, {{page}}, std::nullopt, {}, 0);
    pf.waitIdle();
    CHECK(c.count == 0);
    CHECK_FALSE(pf.get(page));
}

TEST_CASE("Prefetch：背景正在解的那一頁，take 等它解完，不會解兩次") {
    std::wstring root = testRoot(L"wait");
    writePng(root + L"\\a.png", 30, 30);
    std::wstring a = root + L"\\a.png";
    Counting c;
    c.gate.close();
    Prefetcher pf(c.decoder());
    pf.setWanted({}, {{a}}, std::nullopt, {}, kBig);
    c.gate.waitEntered(1);
    // 背景卡在解 a 的時候，另一條執行緒來拿：解碼器沒放行之前 take 不能回來。
    auto taking = std::async(std::launch::async, [&] { return pf.get(a); });
    CHECK(taking.wait_for(std::chrono::milliseconds(200)) == std::future_status::timeout);
    c.gate.release();
    auto r = taking.get();
    REQUIRE(r);
    CHECK(r->image);
    CHECK(c.count == 1); // take 自己再解一次的話會是 2
}

TEST_CASE("Prefetch：解碼後檔案被改過（例如存檔），take 當作沒有") {
    std::wstring root = testRoot(L"stamp");
    std::wstring a = root + L"\\a.png";
    writePng(a, 30, 30);
    Prefetcher pf;
    pf.setWanted({}, {{a}}, std::nullopt, {}, kBig);
    pf.waitIdle();
    writePng(a, 31, 30);
    CHECK_FALSE(pf.get(a));
}

TEST_CASE("Prefetch：高品質縮圖照 target 的尺寸先做好；尺寸不對不給；不要縮圖時丟掉") {
    std::wstring root = testRoot(L"scaled");
    writePng(root + L"\\a.png", 80, 60);
    writePng(root + L"\\b.png", 40, 60);
    std::wstring a = root + L"\\a.png", b = root + L"\\b.png";
    Prefetcher pf;
    pf.setWanted({}, {{a, b}}, std::nullopt, halfSize(), kBig);
    pf.waitIdle();
    ImagePtr ia = pf.get(a)->image;
    ImagePtr s = pf.scaled(ia, 40, 30);
    REQUIRE(s);
    CHECK(s->width == 40);
    CHECK(s->height == 30);
    CHECK_FALSE(pf.scaled(ia, 20, 15));
    CHECK(pf.heldBytes() == (80 * 60 + 40 * 60 + 40 * 30 + 20 * 30) * 4);
    // target 換了（視窗大小變了）：重縮
    pf.setWanted({}, {{a, b}}, std::nullopt,
                 [](const std::vector<Size2>& p) { return std::vector<Size2>(p.size(), Size2{10, 10}); }, kBig);
    pf.waitIdle();
    CHECK(pf.scaled(ia, 10, 10));
    // 快速模式：不縮
    pf.setWanted({}, {{a, b}}, std::nullopt, {}, kBig);
    CHECK_FALSE(pf.scaled(ia, 10, 10));
    CHECK(pf.heldBytes() == (80 * 60 + 40 * 60) * 4);
}

TEST_CASE("planBook：資料夾，置頂、兩頁並排的第一屏 / 最後一屏、單頁") {
    std::wstring root = testRoot(L"plan");
    writePng(root + L"\\B\\1.png", 40, 60);
    writePng(root + L"\\B\\2.png", 40, 60);
    writePng(root + L"\\B\\3.png", 90, 60); // 寬圖單獨一屏
    writePng(root + L"\\B\\4.png", 40, 60);
    writePng(root + L"\\B\\5.png", 40, 60);
    writePng(root + L"\\B\\cover.png", 40, 60);
    BookRequest req;
    req.twoPage = true;
    req.firstAlone = true;
    req.pinned = {L"cover.png"};
    std::wstring B = root + L"\\B";
    BookPlan p = planBook(B, req, L"", nullptr);
    REQUIRE(p.usable());
    CHECK_FALSE(p.archive);
    REQUIRE(p.files.size() == 6);
    CHECK(p.files[0] == B + L"\\cover.png");
    CHECK(p.wide.at(B + L"\\3.png"));
    CHECK_FALSE(p.wide.at(B + L"\\1.png"));
    CHECK(p.edge == std::vector<std::wstring>{B + L"\\cover.png"}); // 第一張單獨
    req.dir = -1;
    p = planBook(B, req, L"", nullptr);
    CHECK(p.edge == std::vector<std::wstring>{B + L"\\4.png", B + L"\\5.png"});
    req.firstAlone = false;
    req.dir = 1;
    p = planBook(B, req, L"", nullptr);
    CHECK(p.edge == std::vector<std::wstring>{B + L"\\cover.png", B + L"\\1.png"});
    req.twoPage = false;
    req.dir = -1;
    p = planBook(B, req, L"", nullptr);
    CHECK(p.edge == std::vector<std::wstring>{B + L"\\5.png"});
    CHECK(p.wide.empty());
    // 沒有圖的資料夾
    REQUIRE(createDirectories(root + L"\\E"));
    CHECK_FALSE(planBook(root + L"\\E", req, L"", nullptr).usable());
}

TEST_CASE("planBook：壓縮檔解到 dest；加密時試密碼列表；都不對就不解") {
    std::wstring root = testRoot(L"planzip");
    std::wstring png = root + L"\\src.png";
    writePng(png, 40, 60);
    std::string bytes = readBytes(png);
    makeZip(root + L"\\B.zip", {{"b/2.png", bytes}, {"b/1.png", bytes}, {"b/10.png", bytes}});
    makeZip(root + L"\\C.zip", {{"1.png", bytes}, {"2.png", bytes}}, "pw");
    BookRequest req;
    req.twoPage = true;
    std::wstring dest = root + L"\\tmp\\first";
    BookPlan p = planBook(root + L"\\B.zip", req, dest, nullptr);
    REQUIRE(p.usable());
    CHECK(p.archive);
    CHECK(p.dest == dest);
    REQUIRE(p.files.size() == 3);
    CHECK(p.extracted.names == std::vector<std::wstring>{L"b/1.png", L"b/2.png", L"b/10.png"});
    CHECK(p.edge == std::vector<std::wstring>{p.files[0]});
    CHECK(exists(p.files[0]));

    req.passwords = {L"nope", L"pw"};
    std::wstring dest2 = root + L"\\tmp\\second";
    p = planBook(root + L"\\C.zip", req, dest2, nullptr);
    REQUIRE(p.usable());
    CHECK(p.password == L"pw");
    CHECK(p.files.size() == 2);

    req.passwords = {L"nope"};
    std::wstring dest3 = root + L"\\tmp\\pre3";
    p = planBook(root + L"\\C.zip", req, dest3, nullptr);
    CHECK_FALSE(p.usable());
    CHECK(p.extracted.status == ArchiveStatus::NeedPassword);
    CHECK(p.dest.empty());
    CHECK_FALSE(exists(dest3));
}

TEST_CASE("Prefetch 換本：資料夾，背景查到下一本並先解第一屏；takeBook 只給那一本、只給一次") {
    std::wstring root = testRoot(L"book");
    for (int i = 1; i <= 3; ++i) writePng(root + L"\\A\\" + std::to_wstring(i) + L".png", 40, 60);
    for (int i = 1; i <= 4; ++i) writePng(root + L"\\B\\" + std::to_wstring(i) + L".png", 40, 60);
    std::wstring A = root + L"\\A", B = root + L"\\B";
    Counting c;
    Prefetcher pf(c.decoder());
    BookRequest req;
    req.fromBook = A;
    req.dir = 1;
    req.twoPage = true;
    req.firstAlone = false;
    pf.setWanted({A + L"\\3.png"}, {}, req, {}, kBig);
    pf.waitIdle();
    CHECK(c.count == 2); // B 的第一屏：1、2
    CHECK_FALSE(pf.takeBook(root + L"\\C", nullptr));
    auto plan = pf.takeBook(B, nullptr);
    REQUIRE(plan);
    REQUIRE(plan->usable());
    CHECK(plan->files.size() == 4);
    CHECK(plan->wide.size() == 4);
    CHECK(plan->edge == std::vector<std::wstring>{B + L"\\1.png", B + L"\\2.png"});
    CHECK_FALSE(pf.takeBook(B, nullptr));
    CHECK(pf.get(B + L"\\1.png"));
    CHECK(pf.get(B + L"\\2.png"));
    CHECK(c.count == 2);

    // 往回：B 的上一本是 A，第一張單獨時最後一屏是 2、3
    req.fromBook = B;
    req.dir = -1;
    req.firstAlone = true;
    pf.setWanted({B + L"\\1.png"}, {}, req, {}, kBig);
    pf.waitIdle();
    plan = pf.takeBook(A, nullptr);
    REQUIRE(plan);
    CHECK(plan->edge == std::vector<std::wstring>{A + L"\\2.png", A + L"\\3.png"});

    // 沒有下一本
    req.fromBook = B;
    req.dir = 1;
    pf.setWanted({}, {}, req, {}, kBig);
    pf.waitIdle();
    CHECK_FALSE(pf.takeBook(root + L"\\C", nullptr));
}

TEST_CASE("Prefetch 換本：壓縮檔先解好；沒被接手的解壓資料夾在換要求與關閉時刪掉") {
    std::wstring root = testRoot(L"bookzip");
    std::wstring shelf = root + L"\\shelf", tmp = root + L"\\tmp";
    writePng(shelf + L"\\A\\1.png", 40, 60);
    writePng(root + L"\\src.png", 40, 60);
    std::string bytes = readBytes(root + L"\\src.png");
    makeZip(shelf + L"\\B.zip", {{"1.png", bytes}, {"2.png", bytes}, {"3.png", bytes}});
    REQUIRE(createDirectories(tmp));
    BookRequest req;
    req.fromBook = shelf + L"\\A";
    req.archiveRoot = tmp;
    std::wstring dest;
    {
        Prefetcher pf;
        pf.setWanted({}, {}, req, {}, kBig);
        pf.waitIdle();
        auto plan = pf.takeBook(shelf + L"\\B.zip", nullptr);
        REQUIRE(plan);
        REQUIRE(plan->usable());
        CHECK(plan->archive);
        CHECK(plan->files.size() == 3);
        CHECK(pf.get(plan->edge[0]));
        dest = plan->dest;
        REQUIRE(exists(dest));
    }
    CHECK(exists(dest)); // 接手了：解構不刪
    removeDirectoryRecursive(dest);

    {
        Prefetcher pf;
        pf.setWanted({}, {}, req, {}, kBig);
        pf.waitIdle();
        REQUIRE(unpackDirs(tmp).size() == 1);
        dest = unpackDirs(tmp)[0];
        pf.setWanted({}, {}, std::nullopt, {}, kBig); // 不跳了
        pf.waitIdle();
        CHECK_FALSE(exists(dest));
        CHECK(unpackDirs(tmp).empty());

        pf.setWanted({}, {}, req, {}, kBig);
        pf.waitIdle();
        REQUIRE(unpackDirs(tmp).size() == 1);
        dest = unpackDirs(tmp)[0];
    }
    CHECK_FALSE(exists(dest)); // 沒接手：解構時刪
}

TEST_CASE("Prefetch 換本：等的時候第一次問進度就取消，回 Cancelled，之後不再準備；解碼不被換本擋住") {
    std::wstring root = testRoot(L"bookcancel");
    std::wstring shelf = root + L"\\shelf", tmp = root + L"\\tmp";
    writePng(shelf + L"\\A\\1.png", 40, 60);
    writePng(shelf + L"\\A\\2.png", 40, 60);
    // 張數多到解壓一定比 takeBook 問一次進度（約 50 ms）久。
    std::string png = mp::test::pngBytes(2, 2);
    std::vector<std::pair<std::string, std::string>> entries;
    for (int i = 0; i < 4000; ++i) entries.push_back({std::to_string(i) + ".png", png});
    makeZip(shelf + L"\\B.zip", entries);
    REQUIRE(createDirectories(tmp));
    Counting c;
    Prefetcher pf(c.decoder());
    BookRequest req;
    req.fromBook = shelf + L"\\A";
    req.archiveRoot = tmp;
    pf.setWanted({}, {{shelf + L"\\A\\2.png"}}, req, {}, kBig);
    int calls = 0;
    auto plan = pf.takeBook(shelf + L"\\B.zip", [&](uint64_t, uint64_t) {
        ++calls;
        return false;
    });
    REQUIRE(plan);
    REQUIRE(plan->extracted.status == ArchiveStatus::Cancelled); // 不是 Cancelled 表示取消之前就解完了：把張數加大
    CHECK(calls == 1);
    CHECK_FALSE(plan->usable());
    // 換本在另一條執行緒做：這一頁的解碼不必等它
    CHECK(pf.get(shelf + L"\\A\\2.png"));
    pf.waitIdle();
    CHECK(unpackDirs(tmp).empty());
    CHECK_FALSE(pf.takeBook(shelf + L"\\B.zip", nullptr)); // 同一個要求不再重做
    // 換一個要求（方向不同）再換回來：重新準備
    BookRequest other = req;
    other.dir = -1;
    pf.setWanted({}, {}, other, {}, kBig);
    pf.setWanted({}, {}, req, {}, kBig);
    auto again = pf.takeBook(shelf + L"\\B.zip", nullptr);
    REQUIRE(again);
    CHECK(again->usable());
    removeDirectoryRecursive(again->dest);
}

TEST_CASE("Prefetch 換本：解壓到一半取消，回 Cancelled，解到一半的資料夾刪掉") {
    std::wstring root = testRoot(L"bookcancelmid");
    std::wstring shelf = root + L"\\shelf", tmp = root + L"\\tmp";
    writePng(shelf + L"\\A\\1.png", 40, 60);
    // 張數多到解壓一定比 takeBook 問一次進度（約 50 ms）久。
    std::string png = mp::test::pngBytes(2, 2);
    std::vector<std::pair<std::string, std::string>> entries;
    for (int i = 0; i < 4000; ++i) entries.push_back({std::to_string(i) + ".png", png});
    makeZip(shelf + L"\\B.zip", entries);
    REQUIRE(createDirectories(tmp));
    Prefetcher pf;
    BookRequest req;
    req.fromBook = shelf + L"\\A";
    req.archiveRoot = tmp;
    pf.setWanted({}, {}, req, {}, kBig);
    bool sawPartial = false;
    auto plan = pf.takeBook(shelf + L"\\B.zip", [&](uint64_t done, uint64_t) {
        if (done == 0) return true; // 還沒開始解
        sawPartial = !unpackDirs(tmp).empty();
        return false;
    });
    REQUIRE(plan);
    // 這裡不是 Cancelled 表示取消之前就解完了，沒走到要測的那條路：把張數加大。
    REQUIRE(plan->extracted.status == ArchiveStatus::Cancelled);
    CHECK(sawPartial);
    CHECK_FALSE(plan->usable());
    pf.waitIdle();
    CHECK(unpackDirs(tmp).empty());
    CHECK_FALSE(pf.takeBook(shelf + L"\\B.zip", nullptr));
}

TEST_CASE("Prefetch：dropBook 等背景停下來，沒被接手的解壓資料夾刪掉") {
    std::wstring root = testRoot(L"dropbook");
    std::wstring shelf = root + L"\\shelf", tmp = root + L"\\tmp";
    writePng(shelf + L"\\A\\1.png", 40, 60);
    writePng(root + L"\\src.png", 40, 60);
    makeZip(shelf + L"\\B.zip", {{"1.png", readBytes(root + L"\\src.png")}});
    REQUIRE(createDirectories(tmp));
    Prefetcher pf;
    BookRequest req;
    req.fromBook = shelf + L"\\A";
    req.archiveRoot = tmp;
    pf.setWanted({}, {}, req, {}, kBig);
    pf.waitIdle();
    REQUIRE(unpackDirs(tmp).size() == 1);
    pf.dropBook();
    // 回來的時候已經刪完了：呼叫端接著刪整個暫存資料夾，不會兩邊同時刪。
    CHECK(unpackDirs(tmp).empty());
    CHECK_FALSE(pf.takeBook(shelf + L"\\B.zip", nullptr));
    CHECK(pf.heldBytes() == 0);
}

TEST_CASE("Prefetch：背景解失敗的頁不會被 get 給出去，也不會一直重試（B-24d）") {
    // 檔案當時被別的程式鎖住，背景解失敗；之後鎖放開了，翻過去要重新解，不是拿到當時的錯誤。
    std::wstring root = testRoot(L"failcache");
    writePng(root + L"\\1.png", 30, 30);
    writePng(root + L"\\2.png", 30, 30);
    std::wstring p1 = root + L"\\1.png", p2 = root + L"\\2.png";
    std::atomic<int> calls{0};
    std::atomic<bool> locked{true};
    Prefetcher pf([&](const std::wstring& p) {
        ++calls;
        if (locked) {
            LoadResult r;
            r.error.code = IoErrorCode::DecodeFailed;
            return r;
        }
        return loadImage(p);
    });
    pf.setWanted({p1}, {{p2}}, std::nullopt, {}, kBig);
    pf.waitIdle();
    CHECK(calls == 1);
    pf.setWanted({p1}, {{p2}}, std::nullopt, {}, kBig); // 再排一次：失敗過的不重試
    pf.waitIdle();
    CHECK(calls == 1);
    locked = false;
    CHECK_FALSE(pf.get(p2)); // 當時的失敗不給：呼叫端自己再解
    LoadResult fresh = loadImage(p2);
    REQUIRE(fresh.image);
    pf.put(p2, fresh);
    auto again = pf.get(p2);
    REQUIRE(again);
    CHECK(again->image == fresh.image);
    // 呼叫端自己解也失敗、放進來的結果，同樣不會被給出去
    LoadResult bad;
    bad.error.code = IoErrorCode::DecodeFailed;
    pf.put(p1, bad);
    CHECK_FALSE(pf.get(p1));
}

TEST_CASE("Prefetch：算縮圖尺寸的函式在背景執行緒、不持鎖的時候被呼叫") {
    std::wstring root = testRoot(L"targetnolock");
    writePng(root + L"\\1.png", 40, 40);
    writePng(root + L"\\2.png", 40, 40);
    std::wstring p1 = root + L"\\1.png", p2 = root + L"\\2.png";
    Prefetcher pf;
    std::atomic<int> calls{0};
    const DWORD uiThread = GetCurrentThreadId();
    std::atomic<bool> onUiThread{false};
    pf.setWanted({p1}, {{p2}}, std::nullopt, [&](const std::vector<Size2>& pages) {
        ++calls;
        if (GetCurrentThreadId() == uiThread) onUiThread = true;
        pf.heldBytes(); // 要拿 Prefetcher 的鎖：如果是持鎖呼叫的，這裡就卡死了（測試的看門狗會抓到）
        std::vector<Size2> out;
        for (auto [w, h] : pages) out.push_back({w / 2, h / 2});
        return out;
    }, kBig);
    pf.waitIdle();
    CHECK(calls >= 1);
    CHECK_FALSE(onUiThread);
    ImagePtr im = pf.get(p2)->image;
    CHECK(pf.scaled(im, 20, 20));
}

TEST_CASE("Prefetch：背景高品質縮圖配置不到 → 那一頁不縮、不一直重試，程式照常；尺寸換了再試") {
    // v0.9.9：配置失敗的例外逃出背景執行緒 → terminate，整個程式結束（實測結束碼 0xC0000409）。
    std::wstring root = testRoot(L"scalefail");
    writePng(root + L"\\1.png", 100, 100);
    writePng(root + L"\\2.png", 100, 100);
    std::wstring p1 = root + L"\\1.png", p2 = root + L"\\2.png";
    Prefetcher pf;
    TargetSizes twice = [](const std::vector<Size2>& pages) { return std::vector<Size2>(pages.size(), Size2{200, 200}); };
    TargetSizes triple = [](const std::vector<Size2>& pages) { return std::vector<Size2>(pages.size(), Size2{300, 300}); };
    {
        mp::test::ImageAllocLimit limit(100 * 100 * 4); // 原圖解得出來，放大兩倍的縮圖配置不到
        pf.setWanted({p1}, {{p2}}, std::nullopt, twice, SIZE_MAX);
        pf.waitIdle(); // 失敗後還一直重試的話會卡在這裡（每個測試有時間上限）
        auto r = pf.get(p2);
        REQUIRE(r);
        REQUIRE(r->image);
        CHECK(r->image->width == 100);
        CHECK_FALSE(pf.scaled(r->image, 200, 200));
        CHECK(pf.heldBytes() == 40000); // 只有原圖，沒有縮圖
    }
    // 同一個尺寸不再試；尺寸換了（倍率變了）才再試，這次配置得到
    ImagePtr im = pf.get(p2)->image;
    pf.setWanted({p1}, {{p2}}, std::nullopt, triple, SIZE_MAX);
    pf.waitIdle();
    REQUIRE(pf.scaled(im, 300, 300));
    CHECK(pf.scaled(im, 300, 300)->width == 300);
}

TEST_CASE("Prefetch：縮圖放不下就不縮，原圖留著") {
    std::wstring root = testRoot(L"scalelimit");
    writePng(root + L"\\1.png", 100, 100);
    writePng(root + L"\\2.png", 100, 100); // 原圖 40000，縮成一半 10000
    std::wstring p1 = root + L"\\1.png", p2 = root + L"\\2.png";
    Prefetcher pf;
    pf.setWanted({p1}, {{p2}}, std::nullopt, halfSize(), 45000);
    pf.waitIdle();
    CHECK(pf.heldBytes() == 40000);
    ImagePtr im = pf.get(p2)->image;
    CHECK_FALSE(pf.scaled(im, 50, 50));
    pf.setWanted({p1}, {{p2}}, std::nullopt, halfSize(), 50000); // 剛好放得下
    pf.waitIdle();
    CHECK(pf.heldBytes() == 50000);
    CHECK(pf.scaled(im, 50, 50));
}

TEST_CASE("BookPreparer：沒給解壓位置的壓縮檔不預開；資料夾照樣準備；換要求時沒人接手的解壓資料夾刪掉") {
    std::wstring root = testRoot(L"bookprep");
    std::wstring shelf = root + L"\\shelf", tmp = root + L"\\tmp";
    writePng(shelf + L"\\A\\1.png", 40, 60);
    writePng(root + L"\\src.png", 40, 60);
    makeZip(shelf + L"\\B.zip", {{"1.png", readBytes(root + L"\\src.png")}});
    writePng(shelf + L"\\C\\1.png", 40, 60);
    REQUIRE(createDirectories(tmp));
    std::atomic<int> ready{0};
    BookPreparer books([&] { ++ready; });
    CHECK(books.edge().empty());
    CHECK_FALSE(books.take(shelf + L"\\B.zip", nullptr)); // 沒有要求

    BookRequest req;
    req.fromBook = shelf + L"\\A";
    books.request(req); // archiveRoot 空的：不能預開
    books.waitIdle();
    CHECK(ready == 1);
    CHECK(books.edge().empty());
    auto plan = books.take(shelf + L"\\B.zip", nullptr);
    REQUIRE(plan);
    CHECK(plan->archive);
    CHECK(plan->notPrepared);
    CHECK_FALSE(plan->usable());
    CHECK_FALSE(books.take(shelf + L"\\B.zip", nullptr)); // 只給一次

    req.archiveRoot = tmp;
    books.request(req);
    books.request(req); // 一樣的要求：不重做
    books.waitIdle();
    CHECK(ready == 2);
    REQUIRE(books.edge().size() == 1);
    CHECK(unpackDirs(tmp).size() == 1);
    CHECK_FALSE(books.take(shelf + L"\\C", nullptr)); // 準備的不是這一本

    BookRequest fromB = req;
    fromB.fromBook = shelf + L"\\B.zip"; // 下一本是資料夾 C
    books.request(fromB);
    books.waitIdle();
    CHECK(unpackDirs(tmp).empty()); // 上一個要求解好的沒人接手：刪掉
    CHECK(books.edge() == std::vector<std::wstring>{shelf + L"\\C\\1.png"});
    auto c = books.take(shelf + L"\\C", nullptr);
    REQUIRE(c);
    CHECK(c->usable());
    CHECK_FALSE(c->archive);
    CHECK(books.edge().size() == 1); // 接手之後那一屏還是要的

    fromB.dir = 1;
    fromB.fromBook = shelf + L"\\C"; // C 是最後一本
    books.request(fromB);
    books.waitIdle();
    CHECK(books.edge().empty());
    CHECK_FALSE(books.take(shelf + L"\\A", nullptr));
    books.request(std::nullopt);
    books.drop();
    books.shutdown();
    books.shutdown(); // 可以重複呼叫
}

TEST_CASE("planBook：密碼列表都不對時記下「試過了」；讀寬高的途中取消") {
    std::wstring root = testRoot(L"plantried");
    std::wstring png = root + L"\\src.png";
    writePng(png, 40, 60);
    std::string bytes = readBytes(png);
    makeZip(root + L"\\C.zip", {{"1.png", bytes}, {"2.png", bytes}}, "pw");
    BookRequest req;
    BookPlan p = planBook(root + L"\\C.zip", req, root + L"\\t1", nullptr);
    CHECK(p.extracted.status == ArchiveStatus::NeedPassword);
    CHECK_FALSE(p.passwordsTried); // 沒有列表可試
    req.passwords = {L"a", L"b"};
    p = planBook(root + L"\\C.zip", req, root + L"\\t2", nullptr);
    CHECK(p.extracted.status == ArchiveStatus::NeedPassword);
    CHECK(p.passwordsTried);
    req.passwords = {L"a", L"pw"};
    p = planBook(root + L"\\C.zip", req, root + L"\\t3", nullptr);
    CHECK(p.usable());
    CHECK_FALSE(p.passwordsTried);
    removeDirectoryRecursive(p.dest);

    // 兩頁並排要逐頁讀檔頭：取消了就不讀完，解出來的也刪掉
    req.twoPage = true;
    int polls = 0;
    std::wstring dest = root + L"\\t4";
    p = planBook(root + L"\\C.zip", req, dest, [&](uint64_t, uint64_t total) {
        if (total != 0) return true; // 解壓的進度
        return ++polls < 3;          // 試密碼兩次之前各問一次，第三次是讀寬高之前
    });
    CHECK(p.extracted.status == ArchiveStatus::Cancelled);
    CHECK_FALSE(p.usable());
    CHECK(p.dest.empty());
    CHECK_FALSE(exists(dest));
}
