#include <catch2/catch_test_macros.hpp>

#include <windows.h>
#include <objbase.h>

#include <string>
#include <thread>

#include "core/ImageIO.h"
#include "core/ImageOps.h"
#include "core/PathUtil.h"
#include "test_util.h"

using namespace mp;
using mp::test::tempPath;


static Image testImage(bool withAlpha) {
    Image img(16, 8);
    for (int y = 0; y < img.height; ++y)
        for (int x = 0; x < img.width; ++x) {
            uint8_t* p = img.px(x, y);
            uint8_t a = (withAlpha && x < 4) ? 128 : 255;
            // premultiplied：存入時先乘 alpha
            uint8_t r = static_cast<uint8_t>(x * 16), g = static_cast<uint8_t>(y * 32), b = 64;
            p[0] = static_cast<uint8_t>(b * a / 255);
            p[1] = static_cast<uint8_t>(g * a / 255);
            p[2] = static_cast<uint8_t>(r * a / 255);
            p[3] = a;
        }
    return img;
}

// path 旁邊所有的存檔暫存檔（<檔名>.*.mp-saving.tmp）的檔名。
static std::vector<std::wstring> saveTemps(const std::wstring& path) {
    std::vector<std::wstring> out;
    const size_t slash = path.find_last_of(L'\\');
    const std::wstring pattern = path.substr(slash + 1) + L".*.mp-saving.tmp";
    forEachDirEntry(path.substr(0, slash), [&](const DirEntry& e) {
        out.push_back(e.name);
        return true;
    }, pattern.c_str());
    return out;
}

static int maxDiff(const Image& a, const Image& b) {
    REQUIRE(a.width == b.width);
    REQUIRE(a.height == b.height);
    int m = 0;
    for (size_t i = 0; i < a.pixels.size(); ++i) m = std::max(m, std::abs(a.pixels[i] - b.pixels[i]));
    return m;
}

TEST_CASE("PNG 存檔再讀回，像素與 DPI 一致") {
    Image img = testImage(true);
    auto path = tempPath(L"a.png");
    SaveOptions opt;
    opt.dpi = {150, 150};
    REQUIRE(saveImage(img, path, opt));
    auto r = loadImage(path);
    REQUIRE(r.image);
    CHECK(r.info.format == ImageFormat::Png);
    CHECK(r.info.colorModel == ColorModel::Rgb);
    CHECK(r.info.bitsPerPixel == 32);
    CHECK(maxDiff(*r.image, img) <= 1);
    CHECK(r.dpi.x > 149);
    CHECK(r.dpi.x < 151);
    CHECK(r.dpi.y > 149);
    CHECK(r.dpi.y < 151);
    DeleteFileW(path.c_str());
}

TEST_CASE("JPEG 存檔再讀回，大致相同、沒有 alpha") {
    Image img = testImage(false);
    auto path = tempPath(L"a.jpg");
    REQUIRE(saveImage(img, path));
    auto r = loadImage(path);
    REQUIRE(r.image);
    CHECK(r.info.format == ImageFormat::Jpeg);
    CHECK(r.info.bitsPerPixel == 24);
    CHECK(maxDiff(*r.image, img) <= 24);
    DeleteFileW(path.c_str());
}

TEST_CASE("BMP 與 TIFF 存檔再讀回") {
    Image img = testImage(false);
    for (auto* ext : {L"a.bmp", L"a.tif"}) {
        auto path = tempPath(ext);
        REQUIRE(saveImage(img, path));
        auto r = loadImage(path);
        REQUIRE(r.image);
        CHECK(maxDiff(*r.image, img) == 0);
        DeleteFileW(path.c_str());
    }
}

// 紅色三區，各 16 px 寬（對齊 JPEG 的 16×16 MCU，色度取樣不會混到隔壁區）：全透明 / alpha 128 / 不透明。premultiplied。
static Image threeZoneRed() {
    Image img(48, 16);
    for (int y = 0; y < img.height; ++y)
        for (int x = 0; x < img.width; ++x) {
            uint8_t a = x < 16 ? 0 : x < 32 ? 128 : 255;
            uint8_t* p = img.px(x, y);
            p[0] = 0;
            p[1] = 0;
            p[2] = a; // 紅 255 乘上 alpha
            p[3] = a;
        }
    return img;
}

static bool closeTo(const uint8_t* p, int b, int g, int r, int tol) {
    return std::abs(p[0] - b) <= tol && std::abs(p[1] - g) <= tol && std::abs(p[2] - r) <= tol && p[3] == 255;
}

TEST_CASE("透明圖存成 JPEG / BMP：鋪白底，半透明處和白色混合") {
    Image img = threeZoneRed();
    for (auto* name : {L"t.jpg", L"t.bmp"}) {
        bool jpeg = std::wstring(name) == L"t.jpg";
        int tol = jpeg ? 6 : 0;
        auto path = tempPath(name);
        REQUIRE(saveImage(img, path));
        auto r = loadImage(path);
        REQUIRE(r.image);
        CHECK(r.info.bitsPerPixel == 24);
        CHECK(closeTo(r.image->px(8, 8), 255, 255, 255, tol));  // 透明 → 白
        CHECK(closeTo(r.image->px(24, 8), 127, 127, 255, tol)); // 半透明紅 + 白
        CHECK(closeTo(r.image->px(40, 8), 0, 0, 255, tol));     // 不透明紅不變
        DeleteFileW(path.c_str());
    }
}

TEST_CASE("透明圖存成 PNG / TIFF：透明照留") {
    Image img = threeZoneRed();
    for (auto* name : {L"t.png", L"t.tif"}) {
        auto path = tempPath(name);
        REQUIRE(saveImage(img, path));
        auto r = loadImage(path);
        REQUIRE(r.image);
        CHECK(r.info.bitsPerPixel == 32);
        CHECK(maxDiff(*r.image, img) <= 1);
        DeleteFileW(path.c_str());
    }
}

TEST_CASE("extensionKeepsTransparency") {
    for (auto* e : {L".png", L".tif", L".tiff", L".webp"}) CHECK(extensionKeepsTransparency(e));
    for (auto* e : {L".jpg", L".jpeg", L".jpe", L".jfif", L".bmp", L".dib"}) CHECK_FALSE(extensionKeepsTransparency(e));
}

TEST_CASE("WebP 存檔再讀回（含 alpha）") {
    Image img = testImage(true);
    auto path = tempPath(L"a.webp");
    REQUIRE(saveImage(img, path));
    auto r = loadImage(path);
    REQUIRE(r.image);
    CHECK(r.info.format == ImageFormat::WebP);
    CHECK(r.info.bitsPerPixel == 32);
    CHECK(maxDiff(*r.image, img) <= 40); // 有損
    DeleteFileW(path.c_str());
}

TEST_CASE("副檔名錯誤時 loadImage 仍能靠內容判斷 WebP") {
    Image img = testImage(false);
    auto path = tempPath(L"a.webp");
    REQUIRE(saveImage(img, path));
    auto renamed = tempPath(L"a.png");
    MoveFileExW(path.c_str(), renamed.c_str(), MOVEFILE_REPLACE_EXISTING);
    auto r = loadImage(renamed);
    CHECK(r.image);
    CHECK(r.info.format == ImageFormat::WebP);
    DeleteFileW(renamed.c_str());
}

TEST_CASE("不存在的檔案回傳錯誤") {
    auto r = loadImage(tempPath(L"nope.png"));
    CHECK_FALSE(r.image);
    CHECK(r.error.code == IoErrorCode::DecodeFailed);
    CHECK(r.error.hresult != 0);
}

TEST_CASE("不支援的副檔名存檔失敗") {
    SaveResult res = saveImage(testImage(false), tempPath(L"a.gif"));
    CHECK_FALSE(res);
    CHECK(res.error.code == IoErrorCode::UnsupportedSaveFormat);
    CHECK(res.error.detail == L".gif");
    CHECK_FALSE(mp::test::exists(tempPath(L"a.gif")));
}

TEST_CASE("存檔失敗時不會動到原檔") {
    Image img = testImage(false);
    auto path = tempPath(L"keep.png");
    REQUIRE(saveImage(img, path));
    // 鎖住檔案讓取代失敗
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    REQUIRE(h != INVALID_HANDLE_VALUE);
    SaveResult res = saveImage(Image(2, 2), path);
    CloseHandle(h);
    CHECK_FALSE(res);
    CHECK(res.error.code == IoErrorCode::ReplaceFailed);
    CHECK(res.error.systemError != 0); // 帶著系統的錯誤碼：分得出被占用、沒權限、磁碟滿
    CHECK(saveTemps(path).empty());    // 自己的暫存檔刪掉了
    auto r = loadImage(path);
    REQUIRE(r.image);
    CHECK(r.image->width == 16);
    DeleteFileW(path.c_str());
}

// 一個已經結束的行程的編號（開一個馬上結束的 cmd，等它結束再放掉 handle）。
static unsigned long deadPid() {
    wchar_t cmd[] = L"cmd.exe /c exit 0";
    STARTUPINFOW si{sizeof(si)};
    PROCESS_INFORMATION pi{};
    REQUIRE(CreateProcessW(nullptr, cmd, nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi));
    WaitForSingleObject(pi.hProcess, 10000);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return pi.dwProcessId;
}

TEST_CASE("存檔：每次用自己的暫存檔，別人的不碰；結束的行程留下的刪掉；存完不留自己的") {
    // v0.9.9：暫存檔名固定是 <檔名>.mp-saving.tmp，旁邊已經有的（別的視窗正在存同一個檔）會被拿走。
    auto path = tempPath(L"race.png");
    auto other = tempPath(L"race-other.png");
    // 先開「還在跑的別的行程」（同一個測試程式的 [sleeper]），再拿結束的行程編號：編號才不會剛好被它重用。
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring sleeperCmd = L"\"" + std::wstring(exe) + L"\" \"[sleeper]\"";
    STARTUPINFOW si{sizeof(si)};
    PROCESS_INFORMATION live{};
    REQUIRE(CreateProcessW(exe, sleeperCmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &live));
    const std::wstring self = std::to_wstring(GetCurrentProcessId()), dead = std::to_wstring(deadPid());

    struct Planted {
        std::wstring path;
        std::string content;
    };
    const std::vector<Planted> kept = {
        {path + L".mp-saving.tmp", "old fixed name"},                                         // v0.9.9 的名字：不是這個格式
        {path + L"." + std::to_wstring(live.dwProcessId) + L"-1.mp-saving.tmp", "live other"}, // 別的視窗正在存
        {path + L".4-1.mp-saving.tmp", "system"},                                             // 查不到權限：當成還在跑
        {path + L"." + self + L"-999999.mp-saving.tmp", "mine"},                              // 自己這個行程的
        {other + L"." + dead + L"-1.mp-saving.tmp", "other target"},                          // 別的檔的
        {path + L".abc.mp-saving.tmp", "malformed"},
        {path + L".12-.mp-saving.tmp", "malformed"},
        {path + L"." + dead + L"-1-2.mp-saving.tmp", "malformed"},
    };
    const std::wstring crashed = path + L"." + dead + L"-7.mp-saving.tmp"; // 存到一半當掉留下的
    for (const auto& p : kept) mp::test::writeBytes(p.path, p.content);
    mp::test::writeBytes(crashed, "crashed");

    Image img = testImage(false);
    SaveResult res = saveImage(img, path);
    TerminateProcess(live.hProcess, 0);
    WaitForSingleObject(live.hProcess, 5000);
    CloseHandle(live.hThread);
    CloseHandle(live.hProcess);

    CHECK(res);
    auto r = loadImage(path);
    REQUIRE(r.image);
    CHECK(r.image->width == img.width);
    CHECK(r.image->height == img.height);
    CHECK(r.image->pixels == img.pixels);
    CHECK_FALSE(mp::test::exists(crashed));
    for (const auto& p : kept) {
        CAPTURE(p.content);
        CHECK(mp::test::readBytes(p.path) == p.content);
    }
    // 自己這次存檔用的暫存檔沒有留下（自己這個行程的只剩事先放的那一個）
    const std::wstring mine = L"race.png." + self + L"-";
    int mineLeft = 0;
    for (const auto& name : saveTemps(path))
        if (name.rfind(mine, 0) == 0) ++mineLeft;
    CHECK(mineLeft == 1);

    DeleteFileW(path.c_str());
    for (const auto& p : kept) DeleteFileW(p.path.c_str());
}

TEST_CASE("claimSaveTemp：每次一個新的名字，建好空檔；資料夾不存在回空字串並帶原因") {
    auto path = tempPath(L"claim.png");
    unsigned long err = 1;
    std::wstring a = claimSaveTemp(path, err), b = claimSaveTemp(path, err);
    CHECK(err == 0);
    REQUIRE_FALSE(a.empty());
    REQUIRE_FALSE(b.empty());
    CHECK(a != b);
    const std::wstring head = path + L"." + std::to_wstring(GetCurrentProcessId()) + L"-";
    for (const auto& t : {a, b}) {
        CHECK(t.rfind(head, 0) == 0);
        CHECK(t.size() > head.size() + 14);
        CHECK(t.compare(t.size() - 14, 14, L".mp-saving.tmp") == 0);
        CHECK(mp::test::exists(t));
        CHECK(mp::test::readBytes(t).empty());
    }
    DeleteFileW(a.c_str());
    DeleteFileW(b.c_str());

    std::wstring none = claimSaveTemp(tempPath(L"no-such-dir") + L"\\x.png", err);
    CHECK(none.empty());
    CHECK(err == ERROR_PATH_NOT_FOUND);
}

TEST_CASE("Lanczos 放大後存 BMP / JPEG：半透明邊緣鋪白底後接近白，不是暗點（完整存檔路徑）") {
    // v0.9.9：放大後第一點是 (145, 145, 145, 111)，鋪白底溢位，存成 BMP / JPEG 讀回來是 33。
    Image src(3, 1);
    auto set = [](Image& im, int x, uint8_t c, uint8_t a) {
        uint8_t* p = im.px(x, 0);
        p[0] = p[1] = p[2] = c;
        p[3] = a;
    };
    set(src, 0, 128, 128); // 半透明白（premultiplied）
    set(src, 1, 0, 255);   // 不透明黑
    set(src, 2, 0, 255);
    Image big = resample(src, 6, 1);
    for (int x = 0; x < 6; ++x) {
        const uint8_t* p = big.px(x, 0);
        CAPTURE(x, int(p[0]), int(p[1]), int(p[2]), int(p[3]));
        CHECK(p[0] <= p[3]);
        CHECK(p[1] <= p[3]);
        CHECK(p[2] <= p[3]);
    }
    for (int jpeg = 0; jpeg < 2; ++jpeg) {
        CAPTURE(jpeg);
        auto path = tempPath(jpeg ? L"edge.jpg" : L"edge.bmp");
        REQUIRE(saveImage(big, path));
        auto r = loadImage(path);
        REQUIRE(r.image);
        REQUIRE(r.image->width == 6);
        REQUIRE(r.image->height == 1);
        const uint8_t* p = r.image->px(0, 0);
        CAPTURE(int(p[0]), int(p[1]), int(p[2]), int(p[3]));
        CHECK(p[2] >= 150); // 半透明白疊在白底上應該接近白，不是暗灰
        // BMP 不失真：每一點都正好是 c + (255 − a)
        if (!jpeg)
            for (int x = 0; x < 6; ++x) {
                const uint8_t* s = big.px(x, 0);
                const uint8_t* q = r.image->px(x, 0);
                CAPTURE(x);
                for (int c = 0; c < 3; ++c) CHECK(int(q[c]) == s[c] + 255 - s[3]);
                CHECK(int(q[3]) == 255);
            }
        DeleteFileW(path.c_str());
    }
}

TEST_CASE("readImageSize：PNG / JPEG / WebP 只讀檔頭，不存在回傳 false") {
    Image img(16, 8);
    for (auto* name : {L"size.png", L"size.jpg", L"size.webp"}) {
        auto path = tempPath(name);
        REQUIRE(saveImage(img, path));
        int w = 0, h = 0;
        CHECK(readImageSize(path, w, h));
        CHECK(w == 16);
        CHECK(h == 8);
        DeleteFileW(path.c_str());
    }
    int w = 1, h = 1;
    CHECK_FALSE(readImageSize(tempPath(L"nope.png"), w, h));
    CHECK(w == 0);
}

// ---- 以下是 reflection 批次 2 修的 bug：每一個都先在 v0.9.5 上重現過 ----

TEST_CASE("B-16：格式看內容不看副檔名，PNG 改名成 .webp / .jpg 都開得了") {
    Image img = testImage(false);
    auto png = tempPath(L"b16.png");
    REQUIRE(saveImage(img, png));
    for (auto* name : {L"b16_as.webp", L"b16_as.jpg", L"b16_as.bmp"}) {
        auto renamed = tempPath(name);
        REQUIRE(CopyFileW(png.c_str(), renamed.c_str(), FALSE));
        auto r = loadImage(renamed);
        REQUIRE(r.image);
        CHECK(r.info.format == ImageFormat::Png); // 實際的格式
        CHECK(maxDiff(*r.image, img) <= 1);
        int w = 0, h = 0;
        CHECK(readImageSize(renamed, w, h));
        CHECK(w == 16);
        CHECK(h == 8);
        DeleteFileW(renamed.c_str());
    }
    DeleteFileW(png.c_str());
}

namespace {
uint32_t crc32Of(const std::string& s) {
    uint32_t c = 0xFFFFFFFFu;
    for (unsigned char ch : s) {
        c ^= ch;
        for (int k = 0; k < 8; ++k) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u)));
    }
    return ~c;
}
std::string be32s(uint32_t v) {
    return {static_cast<char>(v >> 24), static_cast<char>(v >> 16), static_cast<char>(v >> 8), static_cast<char>(v)};
}
std::string pngChunk(const std::string& type, const std::string& data) {
    return be32s(static_cast<uint32_t>(data.size())) + type + data + be32s(crc32Of(type + data));
}
// 檔頭宣稱 w×h 的 PNG，資料只有一小段。
std::string pngClaiming(uint32_t w, uint32_t h) {
    std::string ihdr = be32s(w) + be32s(h) + std::string("\x08\x06\x00\x00\x00", 5);
    std::string idat("\x78\x9c\x63\x00\x00\x00\x01\x00\x01", 9);
    return std::string("\x89PNG\r\n\x1a\n", 8) + pngChunk("IHDR", ihdr) + pngChunk("IDAT", idat) + pngChunk("IEND", "");
}
} // namespace

TEST_CASE("B-10：檔頭宣稱超大尺寸、記憶體配不出來時，loadImage 回 OutOfMemory，不丟例外") {
    auto path = tempPath(L"b10.png");
    mp::test::writeBytes(path, pngClaiming(200000, 200000)); // 160 GB
    int w = 0, h = 0;
    REQUIRE(readImageSize(path, w, h)); // 檔頭讀得出來，所以真的會走到配置那一步
    CHECK(w == 200000);
    LoadResult r;
    REQUIRE_NOTHROW(r = loadImage(path));
    CHECK_FALSE(r.image);
    CHECK(r.error.code == IoErrorCode::OutOfMemory);
    DeleteFileW(path.c_str());
}

TEST_CASE("readImageSize：寬高是 0 或檔案是空的回 false") {
    auto path = tempPath(L"empty.png");
    mp::test::writeBytes(path, "");
    int w = 5, h = 5;
    CHECK_FALSE(readImageSize(path, w, h));
    CHECK(w == 0);
    CHECK(h == 0);
    CHECK_FALSE(loadImage(path).image);
    DeleteFileW(path.c_str());
}

TEST_CASE("saveImage：沒有 metadataSource 時 metadataDropped 是 false") {
    for (auto* name : {L"nd.png", L"nd.jpg", L"nd.webp", L"nd.tif", L"nd.bmp"}) {
        auto path = tempPath(name);
        SaveResult res = saveImage(testImage(false), path);
        CHECK(res);
        CHECK_FALSE(res.metadataDropped);
        DeleteFileW(path.c_str());
    }
}

TEST_CASE("解析度：水平與垂直分開存、分開讀；沒給就是 96；WebP 沒有這個欄位") {
    Image img = testImage(false);
    for (auto* name : {L"dpi.png", L"dpi.jpg", L"dpi.tif", L"dpi.bmp"}) {
        CAPTURE(std::wstring(name).size(), name[4]);
        auto path = tempPath(name);
        SaveOptions opt;
        opt.dpi = {300, 150};
        REQUIRE(saveImage(img, path, opt));
        auto r = loadImage(path);
        REQUIRE(r.image);
        CHECK(std::abs(r.dpi.x - 300) < 1);
        CHECK(std::abs(r.dpi.y - 150) < 1);
        REQUIRE(saveImage(img, path)); // 沒給：96
        r = loadImage(path);
        REQUIRE(r.image);
        CHECK(std::abs(r.dpi.x - 96) < 1);
        CHECK(std::abs(r.dpi.y - 96) < 1);
    }
    auto webp = tempPath(L"dpi.webp");
    SaveOptions opt;
    opt.dpi = {300, 150};
    REQUIRE(saveImage(img, webp, opt));
    auto r = loadImage(webp);
    REQUIRE(r.image);
    CHECK(r.dpi == Dpi{});
}

TEST_CASE("saveImage：來源的圖不被改動；接手的版本存出來的檔一模一樣") {
    // 有透明的圖存檔要換算像素（鋪白底、換成 straight）。const 版本複製一份再改，接手的版本直接改交進來的那一份。
    for (bool alpha : {false, true}) {
        for (auto* ext : {L".png", L".jpg", L".bmp", L".tif", L".webp"}) {
            CAPTURE(alpha, ext[1]);
            const Image img = testImage(alpha);
            const std::vector<uint8_t> before = img.pixels;
            auto a = tempPath((std::wstring(L"keep_a") + ext).c_str());
            auto b = tempPath((std::wstring(L"keep_b") + ext).c_str());
            REQUIRE(saveImage(img, a));
            CHECK(img.pixels == before);
            Image owned = img;
            REQUIRE(saveImage(std::move(owned), b));
            CHECK(mp::test::readBytes(a) == mp::test::readBytes(b));
        }
    }
}

TEST_CASE("saveImage：很寬、很高、只有一個像素的圖都存得回來（編碼器分段讀來源）") {
    for (auto size : {std::pair<int, int>{3000, 2}, {2, 3000}, {1, 1}, {257, 131}}) {
        Image img(size.first, size.second);
        for (int y = 0; y < img.height; ++y)
            for (int x = 0; x < img.width; ++x) {
                uint8_t* p = img.px(x, y);
                p[0] = static_cast<uint8_t>(x * 3 + y);
                p[1] = static_cast<uint8_t>(x + y * 5);
                p[2] = static_cast<uint8_t>(x ^ y);
                p[3] = 255;
            }
        for (auto* name : {L"shape.png", L"shape.bmp", L"shape.tif"}) {
            CAPTURE(size.first, size.second, name[6]);
            auto path = tempPath(name);
            REQUIRE(saveImage(img, path));
            auto r = loadImage(path);
            REQUIRE(r.image);
            CHECK(maxDiff(*r.image, img) == 0); // 無損格式：逐像素相同
        }
    }
}

TEST_CASE("讀圖、存圖在同一條執行緒重用 COM：第一次用到之後就留著，結果不變") {
    const std::wstring src = tempPath(L"ctx_src.png"), dst = tempPath(L"ctx_dst.png");
    mp::test::writePng(src, 12, 7);
    HRESULT before = E_FAIL, after = E_FAIL;
    bool sized = false, saved = false;
    int w = 0, h = 0, loadedW = 0, savedW = 0;
    std::thread([&] {
        // 新的執行緒還沒有人初始化過 COM。
        before = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        if (SUCCEEDED(before)) CoUninitialize();
        sized = readImageSize(src, w, h);
        // 讀過一次之後 COM 是初始化著的（再初始化一次會回 S_FALSE），下一次呼叫不必重來。
        after = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        if (SUCCEEDED(after)) CoUninitialize();
        LoadResult r = loadImage(src);
        if (r.image) {
            loadedW = r.image->width;
            saved = saveImage(*r.image, dst).error.ok();
        }
        if (saved && readImageSize(dst, w, h)) savedW = w;
    }).join(); // 執行緒結束時把它的 COM 與 factory 放掉，不會當
    CHECK(before == S_OK);
    CHECK(sized);
    CHECK(after == S_FALSE);
    CHECK(loadedW == 12);
    CHECK(saved);
    CHECK(savedW == 12);
    // 這條（測試的主）執行緒照樣能用。
    CHECK(readImageSize(src, w, h));
    CHECK(h == 7);
}
