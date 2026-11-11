#include <catch2/catch_test_macros.hpp>

#include <windows.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <bit7z/bit7zlibrary.hpp>
#include <bit7z/bitarchivewriter.hpp>
#include <bit7z/bitformat.hpp>

#include "core/Archive.h"
#include "core/FolderNav.h"
#include "core/ImageIO.h"
#include "core/Prefetch.h"
#include "core/TempDir.h"

using namespace mp;

namespace {

std::wstring testRoot(const wchar_t* name) {
    wchar_t dir[MAX_PATH];
    GetTempPathW(MAX_PATH, dir);
    std::wstring root = std::wstring(dir) + L"mp_test_prefetch_" + name;
    removeDirectoryRecursive(root);
    REQUIRE(createDirectories(root));
    return root;
}

void writePng(const std::wstring& path, int w, int h) {
    createDirectories(path.substr(0, path.find_last_of(L"\\/")));
    Image img(w, h);
    for (auto& b : img.pixels) b = 255;
    REQUIRE(saveImage(img, path, nullptr));
}

std::string readBytes(const std::wstring& path) {
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    REQUIRE(f != INVALID_HANDLE_VALUE);
    std::string bytes;
    char buf[4096];
    DWORD got = 0;
    while (ReadFile(f, buf, sizeof(buf), &got, nullptr) && got > 0) bytes.append(buf, got);
    CloseHandle(f);
    return bytes;
}

bool exists(const std::wstring& path) { return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES; }

std::wstring dllPath() {
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring p = exe;
    return p.substr(0, p.find_last_of(L"\\/") + 1) + L"7z.dll";
}

// zip：name → 內容（PNG bytes）。password 非空時加密內容。
void makeZip(const std::wstring& out, const std::vector<std::pair<std::string, std::string>>& entries,
             const std::string& password = "") {
    REQUIRE(loadArchiveLibrary(dllPath()));
    static bit7z::Bit7zLibrary lib(bit7z::to_tstring(dllPath()));
    bit7z::BitArchiveWriter w(lib, bit7z::BitFormat::Zip);
    if (!password.empty()) w.setPassword(password);
    std::vector<bit7z::buffer_t> buffers;
    buffers.reserve(entries.size());
    for (const auto& [name, content] : entries) {
        buffers.emplace_back(content.begin(), content.end());
        w.addFile(buffers.back(), name);
    }
    w.compressTo(bit7z::to_tstring(out));
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
    auto r = pf.take(p2);
    REQUIRE(r);
    REQUIRE(r->image);
    CHECK(r->image->width == 40);
    CHECK(c.count == 3);
    CHECK_FALSE(pf.take(p1)); // current 是呼叫端自己解的
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
    std::weak_ptr<const Image> w2 = pf.take(p2)->image;
    // 翻到 2：1 變成上一屏、3 是下一屏
    pf.setWanted({p2}, {{p3}, {p1}}, std::nullopt, {}, kBig);
    pf.waitIdle();
    cur = {};
    CHECK_FALSE(w1.expired());
    CHECK(c.count == 2);
    auto back = pf.take(p1);
    REQUIRE(back);
    CHECK(back->image == w1.lock());
    back.reset();
    // 跳到別的地方：全部放掉
    pf.setWanted({}, {}, std::nullopt, {}, kBig);
    CHECK(w1.expired());
    CHECK(w2.expired());
    CHECK(pf.heldBytes() == 0);
}

TEST_CASE("Prefetch：上限依優先順序，放不下的那一屏起都不留；單張就超過時連解都不解") {
    std::wstring root = testRoot(L"limit");
    for (int i = 1; i <= 4; ++i) writePng(root + L"\\" + std::to_wstring(i) + L".png", 100, 100); // 一張 40000 bytes
    std::wstring p1 = root + L"\\1.png", p2 = root + L"\\2.png", p3 = root + L"\\3.png", p4 = root + L"\\4.png";
    Counting c;
    Prefetcher pf(c.decoder());
    pf.setWanted({p1}, {{p2, p3}, {p4}}, std::nullopt, {}, 100000);
    pf.waitIdle();
    CHECK(pf.heldBytes() == 80000);
    CHECK(pf.take(p2));
    CHECK(pf.take(p3));
    CHECK_FALSE(pf.take(p4));
    // 上限改小：優先順序低的先丟（這裡只剩第一屏也放不下，整屏丟）
    pf.setWanted({p1}, {{p2, p3}, {p4}}, std::nullopt, {}, 50000);
    CHECK(pf.heldBytes() == 0);
    pf.waitIdle();
    CHECK(pf.heldBytes() <= 50000);
    // 單張就比上限大：看檔頭就停手
    Counting c2;
    Prefetcher pf2(c2.decoder());
    pf2.setWanted({}, {{p2}}, std::nullopt, {}, 1000);
    pf2.waitIdle();
    CHECK(c2.count == 0);
    CHECK(pf2.heldBytes() == 0);
    // 上限 0 = 關閉
    pf2.setWanted({}, {{p2}}, std::nullopt, {}, 0);
    pf2.waitIdle();
    CHECK(c2.count == 0);
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
    std::thread opener([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        c.gate.release();
    });
    auto r = pf.take(a);
    opener.join();
    REQUIRE(r);
    CHECK(r->image);
    CHECK(c.count == 1);
}

TEST_CASE("Prefetch：解碼後檔案被改過（例如存檔），take 當作沒有") {
    std::wstring root = testRoot(L"stamp");
    std::wstring a = root + L"\\a.png";
    writePng(a, 30, 30);
    Prefetcher pf;
    pf.setWanted({}, {{a}}, std::nullopt, {}, kBig);
    pf.waitIdle();
    writePng(a, 31, 30);
    CHECK_FALSE(pf.take(a));
}

TEST_CASE("Prefetch：高品質縮圖照 target 的尺寸先做好；尺寸不對不給；不要縮圖時丟掉") {
    std::wstring root = testRoot(L"scaled");
    writePng(root + L"\\a.png", 80, 60);
    writePng(root + L"\\b.png", 40, 60);
    std::wstring a = root + L"\\a.png", b = root + L"\\b.png";
    Prefetcher pf;
    pf.setWanted({}, {{a, b}}, std::nullopt, halfSize(), kBig);
    pf.waitIdle();
    ImagePtr ia = pf.take(a)->image;
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
    std::wstring dest = root + L"\\tmp\\pre1";
    BookPlan p = planBook(root + L"\\B.zip", req, dest, nullptr);
    REQUIRE(p.usable());
    CHECK(p.archive);
    CHECK(p.dest == dest);
    REQUIRE(p.files.size() == 3);
    CHECK(p.extracted.names == std::vector<std::wstring>{L"b/1.png", L"b/2.png", L"b/10.png"});
    CHECK(p.edge == std::vector<std::wstring>{p.files[0]});
    CHECK(exists(p.files[0]));

    req.passwords = {L"nope", L"pw"};
    std::wstring dest2 = root + L"\\tmp\\pre2";
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
    CHECK(pf.take(B + L"\\1.png"));
    CHECK(pf.take(B + L"\\2.png"));
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
        CHECK(pf.take(plan->edge[0]));
        dest = plan->dest;
        REQUIRE(exists(dest));
    }
    CHECK(exists(dest)); // 接手了：解構不刪
    removeDirectoryRecursive(dest);

    {
        Prefetcher pf;
        pf.setWanted({}, {}, req, {}, kBig);
        pf.waitIdle();
        dest = tmp + L"\\pre1";
        REQUIRE(exists(dest));
        pf.setWanted({}, {}, std::nullopt, {}, kBig); // 不跳了
        pf.waitIdle();
        CHECK_FALSE(exists(dest));

        pf.setWanted({}, {}, req, {}, kBig);
        pf.waitIdle();
        dest = tmp + L"\\pre2";
        REQUIRE(exists(dest));
    }
    CHECK_FALSE(exists(dest)); // 沒接手：解構時刪
}

TEST_CASE("Prefetch 換本：等待時取消，回 Cancelled，之後不再準備") {
    std::wstring root = testRoot(L"bookcancel");
    std::wstring shelf = root + L"\\shelf", tmp = root + L"\\tmp";
    writePng(shelf + L"\\A\\1.png", 40, 60);
    writePng(shelf + L"\\A\\2.png", 40, 60);
    writePng(root + L"\\src.png", 40, 60);
    makeZip(shelf + L"\\B.zip", {{"1.png", readBytes(root + L"\\src.png")}});
    REQUIRE(createDirectories(tmp));
    Counting c;
    c.gate.close();
    Prefetcher pf(c.decoder());
    // 背景先卡在解 A\2.png，換本的工作還沒開始
    pf.setWanted({}, {{shelf + L"\\A\\2.png"}}, std::nullopt, {}, kBig);
    c.gate.waitEntered(1);
    BookRequest req;
    req.fromBook = shelf + L"\\A";
    req.archiveRoot = tmp;
    pf.setWanted({}, {{shelf + L"\\A\\2.png"}}, req, {}, kBig);
    int calls = 0;
    auto plan = pf.takeBook(shelf + L"\\B.zip", [&](uint64_t, uint64_t) { return ++calls < 2; });
    REQUIRE(plan);
    CHECK(plan->extracted.status == ArchiveStatus::Cancelled);
    CHECK_FALSE(plan->usable());
    c.gate.release();
    pf.waitIdle();
    CHECK_FALSE(exists(tmp + L"\\pre1"));
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
    REQUIRE(exists(tmp + L"\\pre1"));
    pf.dropBook();
    pf.waitIdle();
    CHECK_FALSE(exists(tmp + L"\\pre1"));
    CHECK_FALSE(pf.takeBook(shelf + L"\\B.zip", nullptr));
    CHECK(pf.heldBytes() == 0);
}
