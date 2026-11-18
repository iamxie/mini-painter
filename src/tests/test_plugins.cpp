// 讀圖 plugin：資訊解析、掃描、衝突、載入、開檔路徑、當掉紀錄。
// 用 mp_test_plugin.dll（src/tests/plugin/test_plugin.cpp）複製成 plugins\<id>\<id>.dll；要別的資訊時用 UpdateResource 改寫複本。
#include <catch2/catch_test_macros.hpp>

#include <windows.h>

#include <algorithm>
#include <string>
#include <vector>

#include "app/UiText.h"
#include "core/Archive.h"
#include "core/FolderNav.h"
#include "core/ImageFormat.h"
#include "core/ImageIO.h"
#include "core/Lang.h"
#include "core/PluginManifest.h"
#include "core/PluginRun.h"
#include "core/Plugins.h"
#include "core/Settings.h"
#include "core/TempDir.h"
#include "test_util.h"

using namespace mp;
using namespace mp::test;

namespace {

const char kDefaultManifest[] = "api=2\nname=Test plugin\nversion=0.1\nformat=MPTEST image|.mpt\nformat=MPTEST other|.mpt2;.mpu\n";

std::wstring testPluginDll() { return exeDir() + L"mp_test_plugin.dll"; }
std::wstring crashChildExe() { return exeDir() + L"mp_plugin_crash_child.exe"; }

// 改寫（text 是 nullptr 時刪掉）DLL 的資訊資源。
void setManifest(const std::wstring& dll, const std::string* text) {
    HANDLE h = BeginUpdateResourceW(dll.c_str(), FALSE);
    REQUIRE(h != nullptr);
    BOOL ok = UpdateResourceW(h, RT_RCDATA, L"MPAINTER_PLUGIN", MAKELANGID(LANG_NEUTRAL, SUBLANG_NEUTRAL),
                              text ? const_cast<char*>(text->data()) : nullptr, text ? static_cast<DWORD>(text->size()) : 0);
    REQUIRE(ok);
    REQUIRE(EndUpdateResourceW(h, FALSE));
}

// 裝一個 plugin：plugins\<id>\<id>.dll。manifest 給了就換掉資訊。回傳 DLL 路徑。
std::wstring installPlugin(const std::wstring& pluginsDir, const std::wstring& id, const char* manifest = nullptr,
                           const std::wstring& source = L"") {
    const std::wstring dir = pluginsDir + L"\\" + id;
    createDirectories(dir);
    const std::wstring dll = dir + L"\\" + id + L".dll";
    REQUIRE(CopyFileW((source.empty() ? testPluginDll() : source).c_str(), dll.c_str(), FALSE));
    if (manifest) {
        const std::string text = manifest;
        setManifest(dll, &text);
    }
    return dll;
}

std::string manifestWith(const char* formats) {
    return std::string("api=2\nname=X\nversion=1\n") + formats;
}

// MPTEST 檔：magic '1'（格式 0）或 '2'（格式 1）。
std::string mptBytes(char magic, int w, int h, char cmd, uint8_t b = 10, uint8_t g = 20, uint8_t r = 30, uint8_t a = 255) {
    std::string s = "MPTEST";
    s += magic;
    s += '\0';
    auto i32 = [&](int v) {
        for (int k = 0; k < 4; ++k) s += static_cast<char>((static_cast<unsigned>(v) >> (8 * k)) & 0xFF);
    };
    i32(w);
    i32(h);
    s += cmd;
    s += static_cast<char>(b);
    s += static_cast<char>(g);
    s += static_cast<char>(r);
    s += static_cast<char>(a);
    s += std::string(3, '\0');
    return s;
}

// 掃 pluginsDir、全部啟用、設成生效中的。回傳載入結果。
std::vector<PluginLoadResult> activateAll(const std::wstring& pluginsDir) {
    auto entries = scanPlugins(pluginsDir);
    std::vector<std::wstring> ids;
    for (const auto& e : entries) ids.push_back(e.id);
    std::vector<PluginLoadResult> results;
    setActivePlugins(loadPlugins(entries, ids, results));
    return results;
}

bool hasExt(const std::vector<std::wstring>& v, const wchar_t* ext) { return std::find(v.begin(), v.end(), ext) != v.end(); }

} // namespace

// ---- 資訊 ----

TEST_CASE("parsePluginManifest：合格的、BOM、CRLF、註解、大寫副檔名、重複的副檔名、不認得的 key") {
    PluginManifest m;
    std::wstring problem;
    const std::string text = "\xEF\xBB\xBF# comment\r\napi = 1\r\nname=JPEG XL\r\nversion=1.2.3\r\nauthor=someone\r\n\r\n"
                             "format=JPEG XL image|.JXL; .jxl ;.jxls\r\nformat=Other|.oth\r\n";
    REQUIRE(parsePluginManifest(text, m, problem));
    CHECK(m.api == 1);
    CHECK(m.name == L"JPEG XL");
    CHECK(m.version == L"1.2.3");
    REQUIRE(m.formats.size() == 2);
    CHECK(m.formats[0].name == L"JPEG XL image");
    CHECK(m.formats[0].extensions == std::vector<std::wstring>{L".jxl", L".jxls"});
    CHECK(m.formats[1].extensions == std::vector<std::wstring>{L".oth"});
}

TEST_CASE("parsePluginManifest：少了必要的、或格式不對時回 false 並說明") {
    PluginManifest m;
    std::wstring problem;
    struct Case {
        const char* text;
        const wchar_t* expect;
    };
    const Case cases[] = {
        {"name=a\nversion=1\nformat=A|.a\n", L"missing api"},
        {"api=1\nversion=1\nformat=A|.a\n", L"missing name"},
        {"api=1\nname=a\nformat=A|.a\n", L"missing version"},
        {"api=1\nname=a\nversion=1\n", L"no format"},
        {"api=x\nname=a\nversion=1\nformat=A|.a\n", L"bad api"},
        {"api=0\nname=a\nversion=1\nformat=A|.a\n", L"bad api"},
        {"api=1\nname=a\nversion=1\nformat=A .a\n", L"no '|'"},
        {"api=1\nname=a\nversion=1\nformat=|.a\n", L"no name"},
        {"api=1\nname=a\nversion=1\nformat=A|\n", L"no extension"},
        {"api=1\nname=a\nversion=1\nformat=A|jxl\n", L"bad extension"},
        {"api=1\nname=a\nversion=1\nformat=A|.\n", L"bad extension"},
        {"api=1\nname=a\nversion=1\nformat=A|.j*l\n", L"bad extension"},
        {"api=1\nname=a\nversion=1\nformat=A|.a b\n", L"bad extension"},
        {"api=1\nname=a\nversion=1\nformat=A|.tar.gz\n", L"bad extension"},
    };
    for (const auto& c : cases) {
        INFO(c.text);
        CHECK_FALSE(parsePluginManifest(c.text, m, problem));
        CHECK(problem.find(c.expect) != std::wstring::npos);
    }
}

TEST_CASE("readPluginManifest：從 DLL 的資源讀，不執行它；沒有資源、讀不到、內容不合格") {
    const std::wstring dir = freshDir(L"manifest");
    ManifestRead r = readPluginManifest(testPluginDll());
    REQUIRE(r.ok());
    CHECK(r.manifest.name == L"Test plugin");
    REQUIRE(r.manifest.formats.size() == 2);
    CHECK(r.manifest.formats[1].extensions == std::vector<std::wstring>{L".mpt2", L".mpu"});

    const std::wstring copy = dir + L"\\copy.dll";
    REQUIRE(CopyFileW(testPluginDll().c_str(), copy.c_str(), FALSE));
    setManifest(copy, nullptr);
    CHECK(readPluginManifest(copy).error == ManifestError::NotFound);

    const std::string bad = "api=1\nname=x\n";
    setManifest(copy, &bad);
    r = readPluginManifest(copy);
    CHECK(r.error == ManifestError::Invalid);
    CHECK(r.problem == L"missing version");

    r = readPluginManifest(dir + L"\\nope.dll");
    CHECK(r.error == ManifestError::Unreadable);
    CHECK(r.systemError != 0);
    writeBytes(dir + L"\\text.dll", "not a dll");
    CHECK(readPluginManifest(dir + L"\\text.dll").error == ManifestError::Unreadable);
}

// ---- 掃描與衝突 ----

TEST_CASE("scanPlugins：只認 <id>\\<id>.dll，照 id 排序（不分大小寫）；內建的副檔名不給 plugin") {
    const std::wstring dir = freshDir(L"scan");
    CHECK(scanPlugins(dir + L"\\nope").empty());
    installPlugin(dir, L"beta");
    installPlugin(dir, L"Alpha", "api=1\nname=A\nversion=1\nformat=A|.png;.zip;.aaa;.CBZ\n");
    createDirectories(dir + L"\\empty");                        // 沒有 dll
    writeBytes(dir + L"\\wrongname\\other.dll", "x");             // 名字和資料夾不同
    writeBytes(dir + L"\\loose.dll", "x");                        // 不在子資料夾裡
    auto entries = scanPlugins(dir);
    REQUIRE(entries.size() == 2);
    CHECK(entries[0].id == L"Alpha");
    CHECK(entries[1].id == L"beta");
    CHECK(entries[0].builtInOverlap == std::vector<std::wstring>{L".png", L".zip", L".cbz"});
    CHECK(pluginEffectiveExtensions(entries[0]) == std::vector<std::wstring>{L".aaa"});
    CHECK(entries[1].builtInOverlap.empty());
    CHECK(pluginEffectiveExtensions(entries[1]) == std::vector<std::wstring>{L".mpt", L".mpt2", L".mpu"});
}

TEST_CASE("findPluginConflicts：只看要啟用的；內建的副檔名不算衝突") {
    const std::wstring dir = freshDir(L"conflict");
    installPlugin(dir, L"a", manifestWith("format=A|.mpt;.png\n").c_str());
    installPlugin(dir, L"b", manifestWith("format=B|.mpt;.mpu;.png\n").c_str());
    installPlugin(dir, L"c", manifestWith("format=C|.mpu\n").c_str());
    installPlugin(dir, L"d", manifestWith("format=D|.ddd\n").c_str());
    auto entries = scanPlugins(dir);

    auto all = findPluginConflicts(entries, {L"a", L"b", L"c", L"d"});
    REQUIRE(all.size() == 2);
    CHECK(all[0].extension == L".mpt");
    CHECK(all[0].ids == std::vector<std::wstring>{L"a", L"b"});
    CHECK(all[1].extension == L".mpu");
    CHECK(all[1].ids == std::vector<std::wstring>{L"b", L"c"});
    CHECK(conflictingIds(all) == std::vector<std::wstring>{L"a", L"b", L"c"});

    CHECK(findPluginConflicts(entries, {L"A", L"C", L"d"}).empty()); // 不分大小寫；b 沒啟用
    CHECK(findPluginConflicts(entries, {L"b"}).empty());
}

// ---- 載入 ----

TEST_CASE("loadPlugins：只載入要啟用的；資訊、介面版本、匯出函式不對的回報原因") {
    const std::wstring dir = freshDir(L"load");
    installPlugin(dir, L"good");
    installPlugin(dir, L"off", manifestWith("format=O|.off\n").c_str());
    installPlugin(dir, L"nomanifest");
    setManifest(dir + L"\\nomanifest\\nomanifest.dll", nullptr);
    installPlugin(dir, L"newapi", "api=3\nname=N\nversion=1\nformat=N|.new\n");
    installPlugin(dir, L"badapi", manifestWith("format=B|.bad\n").c_str());
    installPlugin(dir, L"nullentry", manifestWith("format=N|.nul\n").c_str());
    installPlugin(dir, L"noexport", manifestWith("format=E|.noe\n").c_str(), dllPath()); // 7z.dll 加上資訊：沒有 mpPluginGetFuncs

    auto entries = scanPlugins(dir);
    std::vector<PluginLoadResult> results;
    auto set = loadPlugins(entries, {L"good", L"nomanifest", L"newapi", L"badapi", L"nullentry", L"noexport"}, results);
    auto errorOf = [&](const wchar_t* id) {
        auto it = std::find_if(results.begin(), results.end(), [&](const PluginLoadResult& r) { return r.id == id; });
        REQUIRE(it != results.end());
        return it->error;
    };
    CHECK(results.size() == 6); // off 沒試
    CHECK(errorOf(L"good") == PluginLoadError::None);
    CHECK(errorOf(L"nomanifest") == PluginLoadError::BadManifest);
    CHECK(errorOf(L"newapi") == PluginLoadError::ApiMismatch);
    CHECK(errorOf(L"badapi") == PluginLoadError::Incompatible);
    CHECK(errorOf(L"nullentry") == PluginLoadError::Incompatible);
    CHECK(errorOf(L"noexport") == PluginLoadError::NoEntry);
    REQUIRE(set->plugins().size() == 1);
    CHECK(set->plugins()[0].id == L"good");
    CHECK(set->pluginForExtension(L".MPU") == &set->plugins()[0]);
    CHECK(set->pluginForExtension(L".off") == nullptr);
}

TEST_CASE("loadPlugins：衝突沒被排除時，副檔名給先載入的那個") {
    const std::wstring dir = freshDir(L"taken");
    installPlugin(dir, L"a", manifestWith("format=A|.mpt\n").c_str());
    installPlugin(dir, L"b", manifestWith("format=B|.mpt;.mpu\n").c_str());
    auto entries = scanPlugins(dir);
    std::vector<PluginLoadResult> results;
    auto set = loadPlugins(entries, {L"a", L"b"}, results);
    REQUIRE(set->plugins().size() == 2);
    CHECK(set->plugins()[0].extensions == std::vector<std::wstring>{L".mpt"});
    CHECK(set->plugins()[1].extensions == std::vector<std::wstring>{L".mpu"});
}

TEST_CASE("isBundledPlugin：隨附的清單，不分大小寫") {
    CHECK(isBundledPlugin(L"jxl"));
    CHECK(isBundledPlugin(L"JXL"));
    CHECK(isBundledPlugin(L"dng"));
    CHECK_FALSE(isBundledPlugin(L"mptest"));
}

// ---- 生效之後：副檔名表、開檔、讀寬高 ----

TEST_CASE("生效中的 plugin：副檔名表、開檔對話框的樣式、資料夾列舉都算進去；拿掉就回到只有內建") {
    const std::wstring dir = freshDir(L"active");
    const std::wstring images = freshDir(L"active-images");
    writeBytes(images + L"\\a.mpt", mptBytes('1', 2, 2, 'O'));
    writePng(images + L"\\b.png", 2, 2);
    CHECK_FALSE(isSupportedExtension(L".mpt"));
    CHECK(listImageFiles(images).size() == 1);

    installPlugin(dir, L"mptest");
    activateAll(dir);
    CHECK(isSupportedExtension(L".MPT"));
    CHECK(isSupportedExtension(L".mpu"));
    CHECK(imageFormatOfExtension(L".mpt") == ImageFormat::Unknown); // 存檔的判斷只看內建
    CHECK_FALSE(canSaveExtension(L".mpt"));
    auto exts = supportedExtensions();
    CHECK(hasExt(exts, L".mpt2"));
    CHECK(exts.front() == L".bmp"); // 內建的在前
    CHECK(allImagePatterns().find(L"*.mpu") != std::wstring::npos);
    CHECK(listImageFiles(images).size() == 2);

    setActivePlugins(nullptr);
    CHECK_FALSE(isSupportedExtension(L".mpt"));
    CHECK_FALSE(hasExt(supportedExtensions(), L".mpt"));
}

TEST_CASE("loadImage：內建認不得的交給 plugin；未乘 alpha 換成 premultiplied、格式名、解析度") {
    const std::wstring dir = freshDir(L"decode");
    installPlugin(dir, L"mptest");
    activateAll(dir);
    const std::wstring p = tempPath(L"decode-a.mpt");
    writeBytes(p, mptBytes('1', 3, 2, 'O', 100, 50, 200, 128));
    LoadResult r = loadImage(p);
    REQUIRE(r.error.ok());
    REQUIRE(r.image);
    CHECK(r.image->width == 3);
    CHECK(r.image->height == 2);
    const uint8_t* px = r.image->px(2, 1);
    CHECK(px[0] == premultiplyChannel(100, 128));
    CHECK(px[1] == premultiplyChannel(50, 128));
    CHECK(px[2] == premultiplyChannel(200, 128));
    CHECK(px[3] == 128);
    CHECK(r.info.format == ImageFormat::Unknown);
    CHECK(r.info.pluginFormat == L"MPTEST image");
    CHECK(r.dpi == Dpi{96, 96});

    // 第二個格式、副檔名不是 plugin 的（看內容，不看副檔名）、plugin 給的解析度。
    const std::wstring q = tempPath(L"decode-b.png");
    writeBytes(q, mptBytes('2', 1, 1, 'D'));
    r = loadImage(q);
    REQUIRE(r.error.ok());
    CHECK(r.info.pluginFormat == L"MPTEST other");
    CHECK(r.dpi == Dpi{300, 150});

    int w = 0, h = 0;
    CHECK(readImageSize(p, w, h));
    CHECK(w == 3);
    CHECK(h == 2);
}

TEST_CASE("loadImage：plugin 回報失敗、成功卻沒給圖、檔頭太短、記憶體不足") {
    const std::wstring dir = freshDir(L"decode-fail");
    installPlugin(dir, L"mptest");
    activateAll(dir);
    const std::wstring p = tempPath(L"fail.mpt");

    writeBytes(p, mptBytes('1', 2, 2, 'E'));
    LoadResult r = loadImage(p);
    CHECK_FALSE(r.image);
    CHECK(r.error.code == IoErrorCode::PluginFailed);
    CHECK(r.error.detail == L"mptest, decode returned 1: test failure");
    CHECK(r.info.pluginFormat == L"MPTEST image");
    const std::wstring text = ioErrorText(r.error);
    CHECK(text.find(L"The plugin could not read this image") != std::wstring::npos);
    CHECK(text.find(L"test failure") != std::wstring::npos);

    writeBytes(p, mptBytes('1', 2, 2, 'N'));
    r = loadImage(p);
    CHECK(r.error.code == IoErrorCode::PluginFailed);
    CHECK(r.error.detail.find(L"without an image") != std::wstring::npos);

    writeBytes(p, mptBytes('1', 2, 2, 'O').substr(0, 12));
    r = loadImage(p);
    CHECK(r.error.code == IoErrorCode::PluginFailed);
    CHECK(r.error.detail.find(L"file too short") != std::wstring::npos);
    int w = 0, h = 0;
    CHECK_FALSE(readImageSize(p, w, h));

    writeBytes(p, mptBytes('1', 100, 100, 'O'));
    {
        ImageAllocLimit limit(1000);
        r = loadImage(p);
    }
    CHECK_FALSE(r.image);
    CHECK(r.error.code == IoErrorCode::OutOfMemory);

    writeBytes(p, mptBytes('1', 2, 2, 'z'));
    CHECK_FALSE(readImageSize(p, w, h));
}

TEST_CASE("loadImage：plugin 也不認的照舊交給 WIC；空檔不交給 plugin") {
    const std::wstring dir = freshDir(L"decode-none");
    installPlugin(dir, L"mptest");
    activateAll(dir);
    const std::wstring p = tempPath(L"none.mpt");
    writeBytes(p, "this is not an image at all");
    LoadResult r = loadImage(p);
    CHECK_FALSE(r.image);
    CHECK(r.error.code == IoErrorCode::DecodeFailed); // WIC 的路
    CHECK(r.info.pluginFormat.empty());
    writeBytes(p, "");
    CHECK_FALSE(loadImage(p).image);
}

TEST_CASE("介面 v2 的 probe 看得到整個檔；v1 的 plugin 照樣載入，probe 只看到檔頭") {
    // 第 100 byte 是 'L' 才認：只看前 64 bytes 認不出來。
    std::string bytes = mptBytes('L', 2, 2, 'O');
    bytes.resize(101, '\0');
    bytes[100] = 'L';
    const std::wstring p = tempPath(L"long.mpt");
    writeBytes(p, bytes);

    const std::wstring v2 = freshDir(L"probe-v2");
    installPlugin(v2, L"mptest");
    auto results = activateAll(v2);
    REQUIRE(results.size() == 1);
    REQUIRE(results[0].error == PluginLoadError::None);
    CHECK(activePlugins()->plugins()[0].manifest.api == 2);
    LoadResult r = loadImage(p);
    REQUIRE(r.error.ok());
    CHECK(r.info.pluginFormat == L"MPTEST image");

    const std::wstring v1 = freshDir(L"probe-v1");
    installPlugin(v1, L"mptest", "api=1\nname=Old\nversion=1\nformat=MPTEST image|.mpt\nformat=MPTEST other|.mpt2;.mpu\n");
    results = activateAll(v1);
    REQUIRE(results.size() == 1);
    CHECK(results[0].error == PluginLoadError::None);
    r = loadImage(p);
    CHECK_FALSE(r.image); // plugin 認不出來 → WIC 也解不開
    CHECK(r.info.pluginFormat.empty());
    const std::wstring q = tempPath(L"short-v1.mpt"); // 檔頭就認得出來的照常
    writeBytes(q, mptBytes('1', 2, 2, 'O'));
    r = loadImage(q);
    REQUIRE(r.error.ok());
    CHECK(r.info.pluginFormat == L"MPTEST image");
}

TEST_CASE("TIFF 檔頭的檔先問 plugin（像 DNG）；plugin 不認的照舊是內建 TIFF；PNG 不問 plugin") {
    const std::wstring dir = freshDir(L"tiff-first");
    installPlugin(dir, L"mptest");
    activateAll(dir);

    // 假的 DNG：TIFF 檔頭，第 100 byte 起是 "MPTIFF"；寬高、指令、顏色在 mptBytes 的位置。
    std::string fake = mptBytes('1', 3, 2, 'O');
    fake.replace(0, 8, std::string("II*\0\0\0\0\0", 8));
    fake.resize(120, '\0');
    fake.replace(100, 6, "MPTIFF");
    const std::wstring p = tempPath(L"fake-dng.tif");
    writeBytes(p, fake);
    REQUIRE(detectImageFormat(p) == ImageFormat::Tiff);
    LoadResult r = loadImage(p);
    REQUIRE(r.error.ok());
    CHECK(r.info.pluginFormat == L"MPTEST other");
    CHECK(r.image->width == 3);
    int w = 0, h = 0;
    CHECK(readImageSize(p, w, h));
    CHECK(w == 3);
    CHECK(h == 2);
    const std::wstring noext = tempPath(L"fake-dng-noext");
    writeBytes(noext, fake);
    CHECK(sniffImageExtension(noext) == L".mpt2");

    const std::wstring t = tempPath(L"real.tif");
    REQUIRE(saveImage(Image(4, 3), t));
    r = loadImage(t);
    REQUIRE(r.error.ok());
    CHECK(r.info.format == ImageFormat::Tiff);
    CHECK(r.info.pluginFormat.empty());
    CHECK(readImageSize(t, w, h));
    CHECK(w == 4);
    CHECK(sniffImageExtension(t) == L".tif");

    const std::wstring png = tempPath(L"really-png.mpt"); // 內容是 PNG，副檔名是 plugin 的
    writeBytes(png, pngBytes(2, 2));
    r = loadImage(png);
    REQUIRE(r.error.ok());
    CHECK(r.info.format == ImageFormat::Png);
    CHECK(r.info.pluginFormat.empty());
}

TEST_CASE("sniffImageExtension：壓縮檔裡沒副檔名的檔，plugin 認得就補它的副檔名") {
    const std::wstring dir = freshDir(L"sniff");
    const std::wstring p = tempPath(L"noext");
    writeBytes(p, mptBytes('2', 1, 1, 'O'));
    CHECK(sniffImageExtension(p).empty());
    installPlugin(dir, L"mptest");
    activateAll(dir);
    CHECK(sniffImageExtension(p) == L".mpt2");
    // 這個格式的副檔名都被別的 plugin 拿走時，用這個 plugin 自己的第一個。
    const std::wstring dir2 = freshDir(L"sniff2");
    installPlugin(dir2, L"a", manifestWith("format=A|.mpt2;.mpu\n").c_str());
    installPlugin(dir2, L"b");
    activateAll(dir2);
    REQUIRE(activePlugins()->plugins().size() == 2);
    // a 的 probe 也回格式 1（同一個 DLL），但 a 的資訊只有一個格式 → 不算，換 b。b 的格式 1 的副檔名（.mpt2、.mpu）
    // 都被 a 拿走了，所以用 b 自己接手的第一個。
    CHECK(sniffImageExtension(p) == L".mpt");
}

// ---- 當掉紀錄 ----

TEST_CASE("judgePluginRun：正常結束不怪；filter 有跑到看出事的執行緒與模組；沒跑到時在 plugin 裡的都怪") {
    const std::wstring plugins = L"C:\\app\\plugins";
    PluginRunRecord rec;
    rec.busy = {{L"jxl", L"decode", L"C:\\a.jxl", 7}, {L"other", L"probe", L"C:\\b.x", 8}, {L"jxl", L"readSize", L"C:\\c.jxl", 9}};

    rec.cleanExit = true;
    CHECK(judgePluginRun(rec, plugins).empty());

    rec.cleanExit = false; // filter 沒跑到（__fastfail、被強制結束）：每個 plugin 算一次
    auto c = judgePluginRun(rec, plugins);
    REQUIRE(c.size() == 2);
    CHECK(c[0].pluginId == L"jxl");
    CHECK(c[0].func == L"decode");
    CHECK(c[0].exceptionCode == 0);
    CHECK(c[1].pluginId == L"other");

    rec.crashed = true; // 出事的執行緒在 plugin 裡
    rec.crashThreadId = 8;
    rec.exceptionCode = 0xC0000005;
    rec.crashModule = L"C:\\app\\plugins\\other\\other.dll";
    c = judgePluginRun(rec, plugins);
    REQUIRE(c.size() == 1);
    CHECK(c[0].pluginId == L"other");
    CHECK(c[0].path == L"C:\\b.x");
    CHECK(c[0].exceptionCode == 0xC0000005);

    rec.crashThreadId = 99; // 出事的執行緒不在 plugin 裡，但模組是 plugin 的（例如它自己開的執行緒）
    rec.crashModule = L"C:\\APP\\plugins\\jxl\\deps\\x.dll";
    c = judgePluginRun(rec, plugins);
    REQUIRE(c.size() == 1);
    CHECK(c[0].pluginId == L"jxl");
    CHECK(c[0].func == L"decode");

    rec.crashModule = L"C:\\app\\mpainter.exe"; // 主程式自己當掉：別的執行緒剛好在 plugin 裡也不怪
    CHECK(judgePluginRun(rec, plugins).empty());
    rec.crashModule = L"C:\\app\\plugins\\loose.dll"; // plugins 底下直接的檔不是任何 plugin
    CHECK(judgePluginRun(rec, plugins).empty());
    rec.crashModule.clear();
    CHECK(judgePluginRun(rec, plugins).empty());
}

TEST_CASE("PluginRun：行程在 plugin 裡死掉的紀錄，下次讀得到、讀完刪掉；正常結束與別的 exe 的不算") {
    const std::wstring runs = freshDir(L"runs");
    const std::wstring exeA = L"C:\\x\\a.exe", exeB = L"C:\\y\\b.exe";
    auto count = [&] {
        int n = 0;
        forEachDirEntry(runs, [&](const DirEntry&) { ++n; return true; });
        return n;
    };

    REQUIRE(startPluginRunLog(runs, exeA));
    CHECK(collectPluginCrashes(runs, exeA, L"C:\\x\\plugins").empty()); // 自己的不讀
    {
        PluginCallScope scope(L"jxl", "decode", L"C:\\pics\\a.jxl");
        stopPluginRunLogForTest(false); // 在 plugin 裡「死掉」
    }
    CHECK(count() == 1);
    CHECK(collectPluginCrashes(runs, exeB, L"C:\\y\\plugins").empty()); // 別的 exe 的不碰
    CHECK(count() == 1);
    auto c = collectPluginCrashes(runs, exeA, L"C:\\x\\plugins");
    REQUIRE(c.size() == 1);
    CHECK(c[0].pluginId == L"jxl");
    CHECK(c[0].func == L"decode");
    CHECK(c[0].path == L"C:\\pics\\a.jxl");
    CHECK(count() == 0);

    // 進出 plugin 之後才死掉：格子清掉了，不怪。
    REQUIRE(startPluginRunLog(runs, exeA));
    { PluginCallScope scope(L"jxl", "decode", L"C:\\pics\\a.jxl"); }
    stopPluginRunLogForTest(false);
    CHECK(collectPluginCrashes(runs, exeA, L"C:\\x\\plugins").empty());
    CHECK(count() == 0);

    // 在 plugin 裡時正常結束：不怪。
    REQUIRE(startPluginRunLog(runs, exeA));
    {
        PluginCallScope scope(L"jxl", "decode", L"C:\\pics\\a.jxl");
        markPluginRunClean();
        stopPluginRunLogForTest(false);
    }
    CHECK(collectPluginCrashes(runs, exeA, L"C:\\x\\plugins").empty());
    CHECK(count() == 0);

    // 不是我們格式的檔不碰。
    writeBytes(runs + L"\\notes.txt", "x");
    writeBytes(runs + L"\\123-zzzzzzzz.run", "x");
    CHECK(collectPluginCrashes(runs, exeA, L"C:\\x\\plugins").empty());
    CHECK(count() == 2);
}

// 真的讓子行程當掉：mp_plugin_crash_child 載入 plugins 資料夾裡的 test plugin，照檔案的指令在 plugin 裡當掉。
namespace {

struct ChildRun {
    DWORD exitCode = 0;
    std::vector<PluginCrash> crashes;
    std::wstring plugins, runs; // 子行程用的 plugins 資料夾與紀錄資料夾
};

// collect 是 false 時不讀紀錄（留給測試自己讀）。
ChildRun runCrashChild(const wchar_t* name, const std::string& file, const wchar_t* mode, bool collect = true) {
    const std::wstring base = freshDir(name);
    const std::wstring plugins = base + L"\\plugins", runs = base + L"\\runs", path = base + L"\\pic.mpt";
    installPlugin(plugins, L"mptest");
    writeBytes(path, file);
    std::wstring cmd = L"\"" + crashChildExe() + L"\" \"" + plugins + L"\" \"" + runs + L"\" " + mode + L" \"" + path + L"\"";
    STARTUPINFOW si{sizeof(si)};
    PROCESS_INFORMATION pi{};
    REQUIRE(CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi));
    CloseHandle(pi.hThread);
    if (WaitForSingleObject(pi.hProcess, 60000) != WAIT_OBJECT_0) {
        TerminateProcess(pi.hProcess, 99);
        WaitForSingleObject(pi.hProcess, 5000);
        CloseHandle(pi.hProcess);
        FAIL("crash child did not finish in 60 s");
    }
    ChildRun r;
    GetExitCodeProcess(pi.hProcess, &r.exitCode);
    CloseHandle(pi.hProcess);
    r.plugins = plugins;
    r.runs = runs;
    if (collect) r.crashes = collectPluginCrashes(runs, crashChildExe(), plugins);
    return r;
}

} // namespace

TEST_CASE("PluginRun 子行程：decode 存取違規 → 認出 plugin、函式、檔、例外代碼") {
    ChildRun r = runCrashChild(L"child-decode", mptBytes('1', 2, 2, 'A'), L"decode");
    CHECK(r.exitCode == 0xC0000005);
    REQUIRE(r.crashes.size() == 1);
    CHECK(r.crashes[0].pluginId == L"mptest");
    CHECK(r.crashes[0].func == L"decode");
    CHECK(r.crashes[0].path.find(L"pic.mpt") != std::wstring::npos);
    CHECK(r.crashes[0].exceptionCode == 0xC0000005);
}

TEST_CASE("PluginRun 子行程：readSize、probe 裡當掉也認得出來") {
    ChildRun r = runCrashChild(L"child-size", mptBytes('1', 2, 2, 'a'), L"size");
    REQUIRE(r.crashes.size() == 1);
    CHECK(r.crashes[0].func == L"readSize");
    r = runCrashChild(L"child-probe", mptBytes('1', 2, 2, 'P'), L"decode");
    REQUIRE(r.crashes.size() == 1);
    CHECK(r.crashes[0].func == L"probe");
}

TEST_CASE("PluginRun 子行程：__fastfail 不經過 filter，靠格子認出來（例外代碼 0）") {
    ChildRun r = runCrashChild(L"child-fastfail", mptBytes('1', 2, 2, 'F'), L"decode");
    CHECK(r.exitCode != 0);
    REQUIRE(r.crashes.size() == 1);
    CHECK(r.crashes[0].pluginId == L"mptest");
    CHECK(r.crashes[0].func == L"decode");
    CHECK(r.crashes[0].exceptionCode == 0);
}

TEST_CASE("PluginRun 子行程：主程式自己當掉、別的執行緒剛好在 plugin 裡 → 不怪 plugin；正常讀完也不怪") {
    ChildRun r = runCrashChild(L"child-host", mptBytes('1', 2, 2, 'W'), L"hostcrash");
    CHECK(r.exitCode == 0xC0000005);
    CHECK(r.crashes.empty());
    r = runCrashChild(L"child-ok", mptBytes('1', 2, 2, 'O'), L"decode");
    CHECK(r.exitCode == 0);
    CHECK(r.crashes.empty());
}

// ---- 啟用狀態與啟動流程 ----

TEST_CASE("pluginEnabled / enabledPluginIds：有紀錄看紀錄（只有 on 算），沒紀錄的只有隨附的算啟用") {
    Settings s;
    CHECK(pluginEnabled(s, L"jxl"));
    CHECK(pluginEnabled(s, L"JXL"));
    CHECK_FALSE(pluginEnabled(s, L"mptest"));
    for (PluginState st : {PluginState::Disabled, PluginState::Crashed, PluginState::Conflict}) {
        setPluginState(s, L"jxl", st);
        CHECK_FALSE(pluginEnabled(s, L"jxl"));
    }
    setPluginState(s, L"jxl", PluginState::Enabled);
    CHECK(pluginEnabled(s, L"jxl"));
    setPluginState(s, L"MPTEST", PluginState::Enabled);
    CHECK(pluginEnabled(s, L"mptest"));

    const std::wstring dir = freshDir(L"enabled-ids");
    installPlugin(dir, L"jxl");
    installPlugin(dir, L"mptest", manifestWith("format=M|.mmm\n").c_str());
    installPlugin(dir, L"zzz", manifestWith("format=Z|.zzz\n").c_str());
    const auto entries = scanPlugins(dir);
    CHECK(enabledPluginIds(entries, s) == std::vector<std::wstring>{L"jxl", L"mptest"});
    CHECK(enabledPluginIds(entries, Settings{}) == std::vector<std::wstring>{L"jxl"});
}

TEST_CASE("pluginDisplayName、pluginStaticCheck；介面版本不同的不算衝突") {
    const std::wstring dir = freshDir(L"static");
    installPlugin(dir, L"a", manifestWith("format=A|.mpt\n").c_str());
    installPlugin(dir, L"newapi", "api=3\nname=N\nversion=1\nformat=N|.mpt\n");
    installPlugin(dir, L"nomanifest");
    setManifest(dir + L"\\nomanifest\\nomanifest.dll", nullptr);
    const auto entries = scanPlugins(dir);
    REQUIRE(entries.size() == 3);
    CHECK(pluginDisplayName(entries, L"A") == L"X");
    CHECK(pluginDisplayName(entries, L"nomanifest") == L"nomanifest");
    CHECK(pluginDisplayName(entries, L"gone") == L"gone");
    // 同名的（同一個 plugin 裝了兩份）：加上 id
    const std::wstring dup = freshDir(L"static-dup");
    installPlugin(dup, L"jxl", "api=1\nname=JPEG XL\nversion=1\nformat=J|.jxl\n");
    installPlugin(dup, L"jxl2", "api=1\nname=jpeg xl\nversion=2\nformat=J|.jxl\n");
    installPlugin(dup, L"other", manifestWith("format=O|.oth\n").c_str());
    const auto dupEntries = scanPlugins(dup);
    CHECK(pluginDisplayName(dupEntries, L"jxl") == L"JPEG XL (jxl)");
    CHECK(pluginDisplayName(dupEntries, L"jxl2") == L"jpeg xl (jxl2)");
    CHECK(pluginDisplayName(dupEntries, L"other") == L"X");
    {
        mp::test::LangGuard guard; // 括號跟著語系
        setActiveLang(parseLangText(L"_code=zh-TW\n%s (%s)=%s（%s）\n"));
        CHECK(pluginDisplayName(dupEntries, L"jxl") == L"JPEG XL（jxl）");
        CHECK(pluginDisplayName(dupEntries, L"other") == L"X");
    }
    CHECK(pluginStaticCheck(entries[0]) == PluginLoadError::None);
    CHECK(pluginStaticCheck(entries[1]) == PluginLoadError::ApiMismatch);
    CHECK(pluginStaticCheck(entries[2]) == PluginLoadError::BadManifest);
    CHECK(findPluginConflicts(entries, {L"a", L"newapi"}).empty());
}

TEST_CASE("groupPluginConflicts：同一批 plugin（不看順序、不分大小寫）搶的副檔名合成一組") {
    const std::vector<PluginConflict> cs = {{L".a", {L"x", L"y"}}, {L".b", {L"y", L"z"}}, {L".c", {L"Y", L"X"}}};
    const auto g = groupPluginConflicts(cs);
    REQUIRE(g.size() == 2);
    CHECK(g[0].ids == std::vector<std::wstring>{L"x", L"y"});
    CHECK(g[0].extensions == std::vector<std::wstring>{L".a", L".c"});
    CHECK(g[1].ids == std::vector<std::wstring>{L"y", L"z"});
    CHECK(g[1].extensions == std::vector<std::wstring>{L".b"});
    CHECK(groupPluginConflicts({}).empty());
}

TEST_CASE("pluginConflictsIfEnabled：要勾的這個和已勾的搶哪些副檔名；已勾的之間原本的衝突不算") {
    const std::wstring dir = freshDir(L"if-enabled");
    installPlugin(dir, L"a", manifestWith("format=A|.mpt\n").c_str());
    installPlugin(dir, L"b", manifestWith("format=B|.mpt;.mpu\n").c_str());
    installPlugin(dir, L"c", manifestWith("format=C|.mpu\n").c_str());
    installPlugin(dir, L"d", manifestWith("format=D|.ddd\n").c_str());
    installPlugin(dir, L"e", manifestWith("format=E|.mpt\n").c_str());
    const auto entries = scanPlugins(dir);
    auto c = pluginConflictsIfEnabled(entries, {L"a", L"c"}, L"b");
    REQUIRE(c.size() == 2);
    CHECK(c[0].extension == L".mpt");
    CHECK(c[0].ids == std::vector<std::wstring>{L"a", L"b"});
    CHECK(c[1].extension == L".mpu");
    CHECK(c[1].ids == std::vector<std::wstring>{L"b", L"c"});
    CHECK(pluginConflictsIfEnabled(entries, {L"a", L"c"}, L"d").empty());
    CHECK(pluginConflictsIfEnabled(entries, {L"a", L"e"}, L"d").empty()); // a 和 e 本來就衝突：不算 d 的
    CHECK(pluginConflictsIfEnabled(entries, {L"B"}, L"b").empty());       // 已經勾著的自己
}

TEST_CASE("startPlugins：照設定載入；有要載入的才開當掉紀錄，第一個資料夾建不起來就用下一個") {
    const std::wstring base = freshDir(L"start");
    const std::wstring plugins = base + L"\\plugins", runs = base + L"\\runs";
    installPlugin(plugins, L"mptest");
    installPlugin(plugins, L"other", manifestWith("format=O|.oth\n").c_str());
    const std::wstring exe = L"C:\\x\\mpainter.exe";
    Settings s;
    PluginStartup r = startPlugins(plugins, {runs}, exe, s); // 都沒紀錄、都不是隨附的
    CHECK(r.entries.size() == 2);
    CHECK(r.results.empty());
    CHECK_FALSE(r.settingsChanged);
    CHECK(r.crashes.empty());
    CHECK(r.conflicts.empty());
    CHECK(pluginRunLogPath().empty());
    REQUIRE(activePlugins());
    CHECK(activePlugins()->plugins().empty());

    setPluginState(s, L"mptest", PluginState::Enabled);
    writeBytes(base + L"\\blocked", "x"); // 是檔案：底下建不了資料夾
    r = startPlugins(plugins, {base + L"\\blocked\\runs", runs}, exe, s);
    REQUIRE(r.results.size() == 1);
    CHECK(r.results[0].id == L"mptest");
    CHECK(r.results[0].error == PluginLoadError::None);
    CHECK_FALSE(r.settingsChanged);
    CHECK(pluginHandlesExtension(L".mpt"));
    CHECK_FALSE(pluginHandlesExtension(L".oth"));
    CHECK(pluginRunLogPath().find(runs + L"\\") == 0);
}

TEST_CASE("startPlugins：要啟用的互相衝突 → 全部標成衝突、寫進設定、不載入；其他的照常；下次不再報") {
    const std::wstring base = freshDir(L"start-conflict");
    const std::wstring plugins = base + L"\\plugins", runs = base + L"\\runs";
    installPlugin(plugins, L"a", manifestWith("format=A|.mpt\n").c_str());
    installPlugin(plugins, L"b", manifestWith("format=B|.mpt;.mpu\n").c_str());
    installPlugin(plugins, L"c", manifestWith("format=C|.ccc\n").c_str());
    Settings s;
    for (const wchar_t* id : {L"a", L"b", L"c"}) setPluginState(s, id, PluginState::Enabled);
    PluginStartup r = startPlugins(plugins, {runs}, L"C:\\x\\mpainter.exe", s);
    CHECK(r.settingsChanged);
    REQUIRE(r.conflicts.size() == 1);
    CHECK(r.conflicts[0].ids == std::vector<std::wstring>{L"a", L"b"});
    CHECK(r.conflicts[0].extensions == std::vector<std::wstring>{L".mpt"});
    CHECK(findPluginSetting(s, L"a")->state == PluginState::Conflict);
    CHECK(findPluginSetting(s, L"b")->state == PluginState::Conflict);
    CHECK(findPluginSetting(s, L"c")->state == PluginState::Enabled);
    REQUIRE(r.results.size() == 1);
    CHECK(r.results[0].id == L"c");
    CHECK_FALSE(pluginHandlesExtension(L".mpt"));
    CHECK(pluginHandlesExtension(L".ccc"));

    r = startPlugins(plugins, {runs}, L"C:\\x\\mpainter.exe", s);
    CHECK(r.conflicts.empty());
    CHECK_FALSE(r.settingsChanged);
}

TEST_CASE("startPlugins：上次在 plugin 裡當掉 → 標成當掉、寫進設定、不載入；同一個 plugin 只報一次；讀過就不再報") {
    const ChildRun first = runCrashChild(L"start-crash1", mptBytes('1', 2, 2, 'A'), L"decode", false);
    const ChildRun second = runCrashChild(L"start-crash2", mptBytes('1', 2, 2, 'A'), L"decode", false);
    REQUIRE(first.exitCode == 0xC0000005);
    REQUIRE(second.exitCode == 0xC0000005);
    Settings s;
    setPluginState(s, L"mptest", PluginState::Enabled);
    PluginStartup r = startPlugins(first.plugins, {first.runs, second.runs}, crashChildExe(), s);
    REQUIRE(r.crashes.size() == 1);
    CHECK(r.crashes[0].pluginId == L"mptest");
    CHECK(r.crashes[0].func == L"decode");
    CHECK(r.crashes[0].exceptionCode == 0xC0000005);
    CHECK(r.settingsChanged);
    CHECK(findPluginSetting(s, L"mptest")->state == PluginState::Crashed);
    CHECK(r.results.empty());
    CHECK_FALSE(pluginHandlesExtension(L".mpt"));
    CHECK(pluginRunLogPath().empty()); // 沒有要載入的

    r = startPlugins(first.plugins, {first.runs, second.runs}, crashChildExe(), s);
    CHECK(r.crashes.empty());
    CHECK_FALSE(r.settingsChanged);
}

// ---- 給使用者看的文字（英文是 key 本身；除錯資訊不翻譯）----

TEST_CASE("plugin 的訊息：名字清單、當掉、衝突；繁中的引號與括號跟語系走") {
    CHECK(quotedNameList({}).empty());
    CHECK(quotedNameList({L"A"}) == L"“A”");
    CHECK(quotedNameList({L"A", L"B"}) == L"“A” and “B”");
    CHECK(quotedNameList({L"A", L"B", L"C"}) == L"“A”, “B” and “C”");

    auto endsWith = [](const std::wstring& s, const std::wstring& tail) {
        return s.size() >= tail.size() && s.compare(s.size() - tail.size(), tail.size(), tail) == 0;
    };
    PluginCrash c{L"jxl", L"decode", L"C:\\pics\\photo.jxl", 0xC0000005};
    std::wstring t = pluginCrashText(L"JPEG XL", c);
    CHECK(t.find(L"while reading photo.jxl because the plugin “JPEG XL” failed") != std::wstring::npos);
    CHECK(endsWith(t, L" (jxl, decode, 0xC0000005)"));
    c = PluginCrash{L"jxl", L"", L"", 0};
    t = pluginCrashText(L"JPEG XL", c);
    CHECK(t.find(L"crashed because the plugin “JPEG XL” failed") != std::wstring::npos);
    CHECK(endsWith(t, L" (jxl, no exception record)"));

    PluginConflictGroup g{{L"a", L"b"}, {L".mpt", L".mpu"}};
    t = pluginConflictText({L"A", L"B"}, g);
    CHECK(t.find(L"The plugins “A” and “B” both want to open .mpt/.mpu files") == 0);
    CHECK(endsWith(t, L" (plugin conflict: a, b: .mpt, .mpu)"));
    g.ids.push_back(L"c");
    t = pluginConflictText({L"A", L"B", L"C"}, g);
    CHECK(t.find(L"The plugins “A”, “B” and “C” all want to open") == 0);
    CHECK(endsWith(t, L" (plugin conflict: a, b, c: .mpt, .mpu)"));

    setActiveLang(parseLangText(L"_code=zh-TW\n“%s”=「%s」\n%s and “%s”=%s和「%s」\n%s, “%s”=%s、「%s」\n%s (%s)=%s（%s）\n"));
    CHECK(quotedNameList({L"A", L"B", L"C"}) == L"「A」、「B」和「C」");
    CHECK(endsWith(pluginConflictText({L"A", L"B"}, PluginConflictGroup{{L"a", L"b"}, {L".mpt"}}), L"（plugin conflict: a, b: .mpt）"));
}

TEST_CASE("pluginProblemText / pluginStatusText：用不了的原因、設定頁的狀態") {
    const std::wstring dir = freshDir(L"problem");
    installPlugin(dir, L"good");
    installPlugin(dir, L"invalid", "api=1\nname=I\nversion=1\n");
    installPlugin(dir, L"newapi", "api=3\nname=N\nversion=1\nformat=N|.new\n");
    installPlugin(dir, L"nomanifest");
    setManifest(dir + L"\\nomanifest\\nomanifest.dll", nullptr);
    writeBytes(dir + L"\\notpe\\notpe.dll", "x");
    const auto entries = scanPlugins(dir);
    auto entry = [&](const wchar_t* id) -> const PluginEntry& {
        auto it = std::find_if(entries.begin(), entries.end(), [&](const PluginEntry& e) { return e.id == id; });
        REQUIRE(it != entries.end());
        return *it;
    };
    auto has = [](const std::wstring& s, const wchar_t* part) { return s.find(part) != std::wstring::npos; };

    const PluginEntry& good = entry(L"good");
    CHECK(pluginProblemText(good, nullptr).empty());
    const PluginLoadResult ok{L"good", PluginLoadError::None, 0};
    CHECK(pluginProblemText(good, &ok).empty());
    const PluginLoadResult failed{L"good", PluginLoadError::LoadFailed, 126};
    CHECK(has(pluginProblemText(good, &failed), L"The plugin could not be loaded: "));
    CHECK(has(pluginProblemText(good, &failed), L"(LoadLibrary error 126)"));
    const PluginLoadResult noEntry{L"good", PluginLoadError::NoEntry, 0};
    CHECK(pluginProblemText(good, &noEntry) == L"This is not an mpainter plugin. (no mpPluginGetFuncs export)");
    const PluginLoadResult incompatible{L"good", PluginLoadError::Incompatible, 0};
    CHECK(has(pluginProblemText(good, &incompatible), L"(mpPluginGetFuncs returned no usable table)"));
    CHECK(pluginProblemText(entry(L"invalid"), nullptr) == L"The plugin information is not valid. (bad manifest: no format)");
    CHECK(pluginProblemText(entry(L"newapi"), nullptr) ==
          L"This plugin was made for a different version of mpainter. (plugin api 3, mpainter api 1-2)");
    CHECK(pluginProblemText(entry(L"nomanifest"), nullptr) == L"This is not an mpainter plugin. (no MPAINTER_PLUGIN resource)");
    CHECK(has(pluginProblemText(entry(L"notpe"), nullptr), L"The plugin file could not be read. (manifest unreadable, error "));

    const PluginSetting crashed{L"good", PluginState::Crashed}, conflict{L"good", PluginState::Conflict};
    CHECK(pluginStatusText(good, true, nullptr, &ok) == L"Enabled");
    CHECK(pluginStatusText(good, true, nullptr, &failed) == L"Could not load");
    CHECK(pluginStatusText(good, true, &crashed, nullptr) == L"Enabled after restart");
    CHECK(pluginStatusText(good, false, nullptr, &ok) == L"Disabled after restart");
    CHECK(pluginStatusText(good, false, &crashed, nullptr) == L"Disabled: it crashed mpainter");
    CHECK(pluginStatusText(good, false, &conflict, nullptr) == L"Disabled: conflicts with another plugin");
    CHECK(pluginStatusText(good, false, nullptr, nullptr) == L"Disabled");
    CHECK(pluginStatusText(good, false, nullptr, &failed) == L"Disabled");
    CHECK(pluginStatusText(entry(L"newapi"), true, nullptr, nullptr) == L"Cannot be used");
    CHECK(pluginStatusText(entry(L"invalid"), false, nullptr, nullptr) == L"Cannot be used");
}
