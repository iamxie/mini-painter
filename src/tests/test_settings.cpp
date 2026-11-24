#include <catch2/catch_test_macros.hpp>

#include <windows.h>

#include <atomic>
#include <string>
#include <thread>

#include "core/Settings.h"
#include "test_util.h"

using namespace mp;

namespace {
using mp::test::tempPath;
bool fileExists(const std::wstring& path) { return mp::test::exists(path); }
// saveSettings 用的暫存檔名（見 Settings.h）。
std::wstring saveTempOf(const std::wstring& path) { return path + L"." + std::to_wstring(GetCurrentProcessId()) + L".tmp"; }
} // namespace

TEST_CASE("parseSettings：預設值與各 key") {
    Settings d = parseSettings(L"");
    CHECK_FALSE(d.askedDefault);
    CHECK(d.tipShown == 0);
    CHECK(d.contextMenu);

    Settings s = parseSettings(L"[mpainter]\r\naskedDefault=1\r\ntipShown = 2\r\ncontextMenu=0\r\nlanguage = ja-JP \r\n; 註解\r\nunknown=x\r\n");
    CHECK(s.askedDefault);
    CHECK(s.tipShown == 2);
    CHECK_FALSE(s.contextMenu);
    CHECK(s.language == L"ja-JP");
    CHECK(d.language.empty());
    CHECK(d.view.viewMode == ViewMode::FitWindow);
    CHECK_FALSE(d.view.upscaleSmall);
    CHECK_FALSE(d.view.highQuality);
    CHECK_FALSE(d.view.scrollFlip);
    CHECK_FALSE(d.view.rightToLeft);
    CHECK(d.view.firstPageAlone);
}

TEST_CASE("parseSettings：檢視模式欄位，不認得的模式退回 fitwindow") {
    Settings s = parseSettings(L"viewMode=fitwidth\nupscaleSmall=1\nhighQuality=1\nscrollFlip=1\nrightToLeft=1\nfirstPageAlone=0\n");
    CHECK(s.view.viewMode == ViewMode::FitWidth);
    CHECK(s.view.upscaleSmall);
    CHECK(s.view.highQuality);
    CHECK(s.view.scrollFlip);
    CHECK(s.view.rightToLeft);
    CHECK_FALSE(s.view.firstPageAlone);
    CHECK(parseSettings(L"viewMode=twopage").view.viewMode == ViewMode::TwoPage);
    CHECK(parseSettings(L"viewMode=bogus").view.viewMode == ViewMode::FitWindow);
    CHECK(parseSettings(L"").view.warnMetadataLoss);
    CHECK_FALSE(parseSettings(L"warnMetadataLoss=0").view.warnMetadataLoss);
    CHECK_FALSE(parseSettings(serializeSettings(parseSettings(L"warnMetadataLoss=0"))).view.warnMetadataLoss);
    CHECK(parseSettings(L"").unzipDir.empty());
    CHECK(parseSettings(L"unzipDir= D:\\tmp\\unzip ").unzipDir == L"D:\\tmp\\unzip");
    CHECK(parseSettings(serializeSettings(parseSettings(L"unzipDir=D:\\tmp\\unzip"))).unzipDir == L"D:\\tmp\\unzip");
}

TEST_CASE("parseSettings：壞值不會炸，負數歸零") {
    Settings s = parseSettings(L"tipShown=abc\naskedDefault=maybe\n");
    CHECK(s.tipShown == 0);
    CHECK_FALSE(s.askedDefault);
    CHECK(parseSettings(L"tipShown=-5").tipShown == 0);
}

TEST_CASE("serializeSettings 與 parseSettings 互逆") {
    Settings s;
    s.askedDefault = true;
    s.tipShown = 3;
    s.contextMenu = false;
    s.language = L"zh-TW";
    Settings back = parseSettings(serializeSettings(s));
    CHECK(back.askedDefault == s.askedDefault);
    CHECK(back.tipShown == s.tipShown);
    CHECK(back.contextMenu == s.contextMenu);
    CHECK(back.language == s.language);
    CHECK(parseSettings(serializeSettings(Settings{})).language.empty());
    s.view.viewMode = ViewMode::FitHeight;
    s.view.upscaleSmall = s.view.highQuality = s.view.scrollFlip = s.view.rightToLeft = true;
    s.view.firstPageAlone = false;
    back = parseSettings(serializeSettings(s));
    CHECK(back.view.viewMode == ViewMode::FitHeight);
    CHECK(back.view.upscaleSmall);
    CHECK(back.view.highQuality);
    CHECK(back.view.scrollFlip);
    CHECK(back.view.rightToLeft);
    CHECK_FALSE(back.view.firstPageAlone);
}

TEST_CASE("loadSettings / saveSettings 檔案往返") {
    std::wstring p = tempPath(L"settings.ini");
    DeleteFileW(p.c_str());
    Settings s;
    CHECK(loadSettings(p, s) == SettingsLoad::Missing); // 不存在
    CHECK(s.tipShown == 0);

    s.tipShown = 1;
    s.askedDefault = true;
    REQUIRE(saveSettings(p, s));
    CHECK_FALSE(fileExists(saveTempOf(p))); // 暫存檔已經變成正式的檔
    Settings back;
    REQUIRE(loadSettings(p, back) == SettingsLoad::Loaded);
    CHECK(back.tipShown == 1);
    CHECK(back.askedDefault);
    CHECK(isPathWritable(p));

    SetFileAttributesW(p.c_str(), FILE_ATTRIBUTE_READONLY);
    CHECK_FALSE(isPathWritable(p));
    s.tipShown = 2;
    CHECK_FALSE(saveSettings(p, s));
    CHECK_FALSE(fileExists(saveTempOf(p)));
    REQUIRE(loadSettings(p, back) == SettingsLoad::Loaded);
    CHECK(back.tipShown == 1); // 存不進去時原檔不動
    SetFileAttributesW(p.c_str(), FILE_ATTRIBUTE_NORMAL);
    DeleteFileW(p.c_str());
}

TEST_CASE("saveSettings：失敗時回報是哪個系統錯誤，成功時是 0") {
    std::wstring p = tempPath(L"save_error.ini");
    DeleteFileW(p.c_str());
    unsigned long why = 99;
    REQUIRE(saveSettings(p, Settings{}, &why));
    CHECK(why == 0);
    // 原檔唯讀
    SetFileAttributesW(p.c_str(), FILE_ATTRIBUTE_READONLY);
    why = 0;
    CHECK_FALSE(saveSettings(p, Settings{}, &why));
    CHECK(why == ERROR_ACCESS_DENIED);
    SetFileAttributesW(p.c_str(), FILE_ATTRIBUTE_NORMAL);
    // 原檔被別的程式獨佔：暫存檔寫得出來，取代不了
    HANDLE lock = CreateFileW(p.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    REQUIRE(lock != INVALID_HANDLE_VALUE);
    why = 0;
    CHECK_FALSE(saveSettings(p, Settings{}, &why));
    CHECK((why == ERROR_SHARING_VIOLATION || why == ERROR_ACCESS_DENIED));
    CloseHandle(lock);
    DeleteFileW(p.c_str());
    // 資料夾不存在：連暫存檔都建不出來
    why = 0;
    CHECK_FALSE(saveSettings(tempPath(L"no-such-dir\\mpainter.ini"), Settings{}, &why));
    CHECK(why == ERROR_PATH_NOT_FOUND);
}

TEST_CASE("saveSettingsMerged：存不進去的原因傳得出來；檔案讀不到時原因是 0") {
    std::wstring p = tempPath(L"merged_error.ini");
    DeleteFileW(p.c_str());
    Settings written;
    unsigned long why = 99;
    REQUIRE(saveSettingsMerged(p, Settings{}, Settings{}, written, &why));
    CHECK(why == 0);
    SetFileAttributesW(p.c_str(), FILE_ATTRIBUTE_READONLY);
    CHECK_FALSE(saveSettingsMerged(p, Settings{}, Settings{}, written, &why));
    CHECK(why == ERROR_ACCESS_DENIED);
    SetFileAttributesW(p.c_str(), FILE_ATTRIBUTE_NORMAL);
    HANDLE lock = CreateFileW(p.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    REQUIRE(lock != INVALID_HANDLE_VALUE);
    why = 99;
    CHECK_FALSE(saveSettingsMerged(p, Settings{}, Settings{}, written, &why)); // 讀不到就不寫
    CHECK(why == 0);
    CloseHandle(lock);
    DeleteFileW(p.c_str());
}

TEST_CASE("loadSettings：檔案在但讀不到是 Unreadable，不是 Missing") {
    std::wstring p = tempPath(L"locked.ini");
    DeleteFileW(p.c_str());
    Settings s;
    s.tipShown = 3;
    s.passwords = {L"secret"};
    REQUIRE(saveSettings(p, s));
    // 別的程式獨佔開著
    HANDLE h = CreateFileW(p.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    REQUIRE(h != INVALID_HANDLE_VALUE);
    Settings back;
    back.tipShown = 9;
    CHECK(loadSettings(p, back) == SettingsLoad::Unreadable);
    CHECK(back.tipShown == 0); // out 是預設值
    CHECK(back.passwords.empty());
    // 這時存檔：取代不了，回 false，原檔內容還在，暫存檔不留
    CHECK_FALSE(saveSettings(p, Settings{}));
    CHECK_FALSE(fileExists(saveTempOf(p)));
    CloseHandle(h);
    REQUIRE(loadSettings(p, back) == SettingsLoad::Loaded);
    CHECK(back.tipShown == 3);
    CHECK(back.passwords == std::vector<std::wstring>{L"secret"});
    // 資料夾不存在也算 Missing
    CHECK(loadSettings(tempPath(L"no-such-dir\\mpainter.ini"), back) == SettingsLoad::Missing);
    DeleteFileW(p.c_str());
}

TEST_CASE("saveSettings：存檔途中另一邊讀到的一定是完整的內容") {
    // 兩個 mpainter 同開：一個在存設定，另一個剛好啟動在讀。讀到半份的話，缺的欄位變預設值，之後一存檔就把原本的蓋掉。
    std::wstring p = tempPath(L"race.ini");
    DeleteFileW(p.c_str());
    Settings s;
    for (int i = 0; i < 3000; ++i) s.passwords.push_back(L"password-" + std::to_wstring(i));
    REQUIRE(saveSettings(p, s));
    std::atomic<bool> done{false};
    std::atomic<int> saveFailed{0};
    std::thread writer([&] {
        for (int i = 0; i < 300; ++i)
            if (!saveSettings(p, s)) ++saveFailed;
        done = true;
    });
    int loaded = 0, partial = 0, missing = 0;
    while (!done) {
        Settings back;
        SettingsLoad r = loadSettings(p, back);
        if (r == SettingsLoad::Missing) ++missing;
        if (r != SettingsLoad::Loaded) continue;
        ++loaded;
        if (back.passwords.size() != s.passwords.size()) ++partial;
    }
    writer.join();
    CHECK(loaded > 0);
    CHECK(partial == 0);
    CHECK(missing == 0); // 取代的過程中檔案不會有「不在」的瞬間
    CHECK(saveFailed == 0);
    CHECK_FALSE(fileExists(saveTempOf(p)));
    DeleteFileW(p.c_str());
}

TEST_CASE("loadSettings 接受 BOM") {
    std::wstring p = tempPath(L"bom.ini");
    HANDLE h = CreateFileW(p.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    REQUIRE(h != INVALID_HANDLE_VALUE);
    const char bytes[] = "\xEF\xBB\xBFtipShown=2\n";
    DWORD w;
    WriteFile(h, bytes, sizeof(bytes) - 1, &w, nullptr);
    CloseHandle(h);
    Settings s;
    REQUIRE(loadSettings(p, s) == SettingsLoad::Loaded);
    CHECK(s.tipShown == 2);
    DeleteFileW(p.c_str());
}

TEST_CASE("isPathWritable：不存在的檔案看資料夾") {
    std::wstring p = tempPath(L"new-dir-probe\\mpainter.ini");
    CHECK_FALSE(isPathWritable(p)); // 資料夾不存在
    CHECK(isPathWritable(tempPath(L"probe.ini")));
}

TEST_CASE("settingsPathForExe") {
    CHECK(settingsPathForExe(L"C:\\Tools\\mpainter.exe") == L"C:\\Tools\\mpainter.ini");
    CHECK(settingsPathForExe(L"mpainter.exe") == L"mpainter.ini");
}

TEST_CASE("Settings：置頂檔名與密碼列表") {
    Settings d = parseSettings(L"");
    REQUIRE(d.pinnedNames.size() == 3);
    CHECK(d.pinnedNames[0] == L"cover.jpg");
    CHECK(d.passwords.empty());

    Settings s = parseSettings(L"pinnedNames= a.jpg | b.png ||\npassword=one\npassword= two words \npassword=\n");
    REQUIRE(s.pinnedNames.size() == 2);
    CHECK(s.pinnedNames[0] == L"a.jpg");
    CHECK(s.pinnedNames[1] == L"b.png");
    REQUIRE(s.passwords.size() == 2);
    CHECK(s.passwords[0] == L"one");
    CHECK(s.passwords[1] == L" two words "); // 前後空白是密碼的一部分，不 trim
    CHECK(parseSettings(L"password=\tx\t\r\n").passwords == std::vector<std::wstring>{L"\tx\t"}); // 只去掉行尾的 \r
    CHECK(parseSettings(L"password=   \n").passwords == std::vector<std::wstring>{L"   "}); // 全空白也是密碼
    CHECK(parseSettings(serializeSettings(parseSettings(L"password= a \n"))).passwords == std::vector<std::wstring>{L" a "});

    CHECK(parseSettings(L"pinnedNames=\n").pinnedNames.empty()); // 寫了空值 = 使用者清空，不是用預設

    s.passwords.push_back(L"p=w|x"); // = 和 | 都能往返
    Settings back = parseSettings(serializeSettings(s));
    CHECK(back.pinnedNames == s.pinnedNames);
    CHECK(back.passwords == s.passwords);
}

TEST_CASE("Settings：autoNextBook 預設關、能往返") {
    CHECK_FALSE(parseSettings(L"").view.autoNextBook);
    Settings s = parseSettings(L"autoNextBook=1\n");
    CHECK(s.view.autoNextBook);
    CHECK(parseSettings(serializeSettings(s)).view.autoNextBook);
    s.view.autoNextBook = false;
    CHECK_FALSE(parseSettings(serializeSettings(s)).view.autoNextBook);
}

TEST_CASE("Settings：更新相關 autoCheckUpdate 預設開、skipVersion、updateUrl 空的不寫") {
    Settings d = parseSettings(L"");
    CHECK(d.autoCheckUpdate);
    CHECK(d.skipVersion.empty());
    CHECK(d.updateUrl.empty());
    CHECK(serializeSettings(d).find(L"updateUrl=") == std::wstring::npos);
    Settings s = parseSettings(L"autoCheckUpdate=0\nskipVersion=0.7.0\nupdateUrl=http://127.0.0.1:8000/latest.json\n");
    CHECK_FALSE(s.autoCheckUpdate);
    CHECK(s.skipVersion == L"0.7.0");
    CHECK(s.updateUrl == L"http://127.0.0.1:8000/latest.json");
    Settings back = parseSettings(serializeSettings(s));
    CHECK_FALSE(back.autoCheckUpdate);
    CHECK(back.skipVersion == L"0.7.0");
    CHECK(back.updateUrl == L"http://127.0.0.1:8000/latest.json");
}

TEST_CASE("Settings：undoLimit 預設 99、能往返、超出範圍夾回、壞值維持預設") {
    CHECK(parseSettings(L"").undoLimit == 99);
    Settings s = parseSettings(L"undoLimit=20\n");
    CHECK(s.undoLimit == 20);
    CHECK(serializeSettings(s).find(L"undoLimit=20\n") != std::wstring::npos);
    CHECK(parseSettings(serializeSettings(s)).undoLimit == 20);
    CHECK(parseSettings(L"undoLimit=0\n").undoLimit == 1);
    CHECK(parseSettings(L"undoLimit=-3\n").undoLimit == 1);
    CHECK(parseSettings(L"undoLimit=100000\n").undoLimit == 999);
    CHECK(parseSettings(L"undoLimit=abc\n").undoLimit == 99);
    CHECK(parseSettings(L"undoLimit=\n").undoLimit == 99);
}

TEST_CASE("Settings：prefetchMB 預設 512、0 是關閉、能往返、超出範圍夾回、壞值維持預設") {
    CHECK(parseSettings(L"").prefetchMB == 512);
    Settings s = parseSettings(L"prefetchMB=0\n");
    CHECK(s.prefetchMB == 0);
    CHECK(serializeSettings(s).find(L"prefetchMB=0\n") != std::wstring::npos);
    s.prefetchMB = 2048;
    CHECK(parseSettings(serializeSettings(s)).prefetchMB == 2048);
    CHECK(parseSettings(L"prefetchMB=-5\n").prefetchMB == 0);
    CHECK(parseSettings(L"prefetchMB=9999999\n").prefetchMB == 65536);
    CHECK(parseSettings(L"prefetchMB=abc\n").prefetchMB == 512);
    CHECK(prefetchMBFromText(L"1024", 512) == 1024);
    CHECK(prefetchMBFromText(L"99999999999999999999", 512) == 65536);
    CHECK(prefetchMBFromText(L"", 300) == 300);
}

TEST_CASE("Settings：undoMemoryMB 預設 512、能往返、超出範圍夾回、壞值維持預設") {
    CHECK(parseSettings(L"").undoMemoryMB == 512);
    Settings s = parseSettings(L"undoMemoryMB=2048\n");
    CHECK(s.undoMemoryMB == 2048);
    CHECK(serializeSettings(s).find(L"undoMemoryMB=2048\n") != std::wstring::npos);
    CHECK(parseSettings(serializeSettings(s)).undoMemoryMB == 2048);
    CHECK(parseSettings(L"undoMemoryMB=1\n").undoMemoryMB == 16);
    CHECK(parseSettings(L"undoMemoryMB=9999999\n").undoMemoryMB == 65536);
    CHECK(parseSettings(L"undoMemoryMB=abc\n").undoMemoryMB == 512);
}

TEST_CASE("Settings：視窗位置 window=x,y,w,h,max") {
    Settings d = parseSettings(L"");
    CHECK(d.winW == 0);
    CHECK_FALSE(d.winMax);
    CHECK(serializeSettings(d).find(L"window=") == std::wstring::npos); // 沒紀錄就不寫
    Settings s = parseSettings(L"window=-8,10,1100,760,1\n");
    CHECK(s.winX == -8);
    CHECK(s.winY == 10);
    CHECK(s.winW == 1100);
    CHECK(s.winH == 760);
    CHECK(s.winMax);
    Settings back = parseSettings(serializeSettings(s));
    CHECK(back.winX == -8);
    CHECK(back.winW == 1100);
    CHECK(back.winMax);
    CHECK(parseSettings(L"window=1,2,0,5,0").winW == 0);   // 寬為 0 當沒紀錄
    CHECK(parseSettings(L"window=garbage").winW == 0);
    CHECK(parseSettings(L"window=1,2,3").winW == 0);       // 欄位不足
}

TEST_CASE("Profile：profileFromCurrent / applyProfile / profileModified") {
    Settings s = parseSettings(L"viewMode=fitwidth\nupscaleSmall=1\nhighQuality=1\nscrollFlip=1\nautoNextBook=1\nrightToLeft=1\nfirstPageAlone=0\nwarnMetadataLoss=0\nprofile=X\n");
    Profile p = profileFromCurrent(s);
    CHECK(p.name == L"X");
    CHECK(p.prefs.viewMode == ViewMode::FitWidth);
    CHECK(p.prefs.upscaleSmall);
    CHECK(p.prefs.highQuality);
    CHECK(p.prefs.scrollFlip);
    CHECK(p.prefs.autoNextBook);
    CHECK(p.prefs.rightToLeft);
    CHECK_FALSE(p.prefs.firstPageAlone);
    CHECK_FALSE(p.prefs.warnMetadataLoss);

    Profile q;
    q.name = L"Q";
    q.prefs.viewMode = ViewMode::TwoPage;
    q.prefs.rightToLeft = true;
    Settings t;
    applyProfile(t, q);
    CHECK(t.profileName == L"Q");
    CHECK(t.view == q.prefs);
    CHECK(t.view.viewMode == ViewMode::TwoPage);
    Profile r = Profile{};
    r.name = L"other name";
    CHECK(r.prefs == Profile{}.prefs); // 比的是內容，名字不算
    r.prefs.scrollFlip = true;
    CHECK(r.prefs != Profile{}.prefs);

    // 沒有這個模式 = 已調整；有且相同 = 沒調整；改一個欄位 = 已調整
    CHECK(profileModified(t));
    t.profiles.push_back(profileFromCurrent(t)); // 同名、同內容
    CHECK_FALSE(profileModified(t));
    t.view.upscaleSmall = true;
    CHECK(profileModified(t));
}

TEST_CASE("Profile：defaultProfiles 與 ensureProfiles") {
    auto d = defaultProfiles(L"一般模式", L"漫畫模式");
    REQUIRE(d.size() == 2);
    CHECK(d[0].name == L"一般模式");
    CHECK(d[0].prefs.viewMode == ViewMode::Actual);
    CHECK_FALSE(d[0].prefs.upscaleSmall);
    CHECK_FALSE(d[0].prefs.highQuality);
    CHECK_FALSE(d[0].prefs.autoNextBook);
    CHECK(d[0].prefs.warnMetadataLoss);
    CHECK(d[1].name == L"漫畫模式");
    CHECK(d[1].prefs.viewMode == ViewMode::TwoPage);
    CHECK(d[1].prefs.highQuality);
    CHECK(d[1].prefs.autoNextBook);
    CHECK_FALSE(d[1].prefs.warnMetadataLoss);

    Settings s = parseSettings(L"viewMode=fitwidth\n");
    CHECK(s.profiles.empty());
    CHECK(ensureProfiles(s, L"G", L"C"));
    REQUIRE(s.profiles.size() == 2);
    CHECK(s.profileName == L"G");
    CHECK(s.view.viewMode == ViewMode::Actual);     // 目前的值套成第一個模式
    CHECK_FALSE(profileModified(s));
    CHECK_FALSE(ensureProfiles(s, L"G", L"C")); // 第二次不動
    s.profileName = L"missing";
    CHECK(ensureProfiles(s, L"G", L"C"));
    CHECK(s.profileName == L"G");
    CHECK(findProfile(s, L"c") != nullptr); // 不分大小寫
    CHECK(findProfile(s, L"zzz") == nullptr);
}

TEST_CASE("Profile：ini 區段解析、重複名、沒名字、其他區段略過、往返") {
    Settings s = parseSettings(
        L"[mpainter]\nviewMode=actual\nprofile=B\n"
        L"[profile]\nname=A\nviewMode=fitheight\nautoNextBook=1\n"
        L"[profile]\nname= B \nviewMode=twopage\nrightToLeft=1\nfirstPageAlone=0\n"
        L"[profile]\nname=a\nviewMode=fitwidth\n"          // 重複（不分大小寫）→ 丟掉
        L"[profile]\nviewMode=fitwidth\n"                   // 沒名字 → 丟掉
        L"[profile]\nname=C\nviewMode=nonsense\n"          // viewMode 正規化
        L"[future]\nviewMode=fitwidth\nname=Z\n"           // 不認得的區段整段略過
        L"[mpainter]\nunzipDir=D:\\u\n");
    REQUIRE(s.profiles.size() == 3);
    CHECK(s.profiles[0].name == L"A");
    CHECK(s.profiles[0].prefs.viewMode == ViewMode::FitHeight);
    CHECK(s.profiles[0].prefs.autoNextBook);
    CHECK(s.profiles[0].prefs.firstPageAlone);
    CHECK(s.profiles[1].name == L"B");
    CHECK(s.profiles[1].prefs.viewMode == ViewMode::TwoPage);
    CHECK(s.profiles[1].prefs.rightToLeft);
    CHECK_FALSE(s.profiles[1].prefs.firstPageAlone);
    CHECK(s.profiles[2].name == L"C");
    CHECK(s.profiles[2].prefs.viewMode == ViewMode::FitWindow);
    CHECK(s.profileName == L"B");
    CHECK(s.view.viewMode == ViewMode::Actual);   // 頂層是目前的值，不被區段蓋掉
    CHECK(s.unzipDir == L"D:\\u");  // 回到 [mpainter] 後的 key 照收
    CHECK(profileModified(s));         // actual != twopage

    Settings back = parseSettings(serializeSettings(s));
    REQUIRE(back.profiles.size() == 3);
    CHECK(back.profiles[1].name == L"B");
    CHECK(back.profiles[1].prefs.rightToLeft);
    CHECK(back.profileName == L"B");
    CHECK(back.view.viewMode == ViewMode::Actual);
    CHECK(back.unzipDir == L"D:\\u");
    // 舊版 ini（沒有區段）：目前的值照舊、清單空
    Settings old = parseSettings(L"viewMode=fitwidth\nhighQuality=1\n");
    CHECK(old.view.viewMode == ViewMode::FitWidth);
    CHECK(old.view.highQuality);
    CHECK(old.profiles.empty());
    CHECK(old.profileName.empty());
}

// ---- reflection 批次 3：設定欄位的單一來源、設定面板的套用、多個視窗的三方合併 ----

TEST_CASE("ViewPrefs：每個開關都在 kViewFlags，ini 讀寫、比較都照表") {
    // 表裡的 key 就是欄位名；逐一打開，其他不受影響，ini 往返後一樣。
    ViewPrefs defaults;
    for (const ViewFlag& f : kViewFlags) {
        INFO("flag #" << (&f - kViewFlags));
        ViewPrefs v;
        v.*f.member = !(v.*f.member);
        CHECK(v != defaults);
        for (const ViewFlag& other : kViewFlags)
            if (other.member != f.member) CHECK(v.*other.member == defaults.*other.member);
        Settings s;
        s.view = v;
        std::wstring ini = serializeSettings(s);
        CHECK(ini.find(std::wstring(f.key) + L"=" + (v.*f.member ? L"1" : L"0") + L"\n") != std::wstring::npos);
        CHECK(parseSettings(ini).view == v);
        // [profile] 區段用同一套
        Profile p;
        p.name = L"P";
        p.prefs = v;
        s.profiles = {p};
        Settings back = parseSettings(serializeSettings(s));
        REQUIRE(back.profiles.size() == 1);
        CHECK(back.profiles[0].prefs == v);
    }
    // 檢視模式也算在比較裡。
    ViewPrefs m;
    m.viewMode = ViewMode::TwoPage;
    CHECK(m != defaults);
    CHECK(defaults == ViewPrefs{});
    // 預設值：兩個預設開的。
    CHECK(defaults.firstPageAlone);
    CHECK(defaults.warnMetadataLoss);
    CHECK_FALSE(defaults.upscaleSmall);
}

TEST_CASE("Settings 的大小：加欄位時的提醒") {
    // 這個數字變了表示 Settings 加（或拿掉）了欄位。檢視開關加在 MP_VIEW_FLAGS、數字欄位加在 kIntSettings 的話
    // ini 讀寫與 mergeSettings 會自動涵蓋；其他欄位要自己補 parseSettings、serializeSettings、mergeSettings，
    // 再更新這裡的數字。
    CHECK(sizeof(Settings) == 344);
}

TEST_CASE("viewModeKey / viewModeFromKey / viewModeName：每個模式往返，不認得的回 FitWindow") {
    for (int i = 0; i < kViewModeCount; ++i) {
        ViewMode m = static_cast<ViewMode>(i);
        CHECK(viewModeFromKey(viewModeKey(m)) == m);
        CHECK(std::wstring(viewModeName(m)).size() > 0);
    }
    CHECK(std::wstring(viewModeKey(ViewMode::TwoPage)) == L"twopage");
    CHECK(std::wstring(viewModeKey(ViewMode::Actual)) == L"actual");
    CHECK(std::wstring(viewModeName(ViewMode::FitWindow)) == L"Fit to Window");
    CHECK(viewModeFromKey(L"bogus") == ViewMode::FitWindow);
    CHECK(viewModeFromKey(L"") == ViewMode::FitWindow);
    CHECK(viewModeFromKey(L"TwoPage") == ViewMode::FitWindow); // ini 的名字分大小寫
}

TEST_CASE("kIntSettings：範圍、ini 往返、intSettingFromText") {
    for (const IntSetting& f : kIntSettings) {
        INFO("setting #" << (&f - kIntSettings));
        CHECK(f.min < f.max);
        Settings d;
        CHECK(d.*f.member >= f.min); // 預設值在範圍內
        CHECK(d.*f.member <= f.max);
        CHECK(intSettingFromText(f, std::to_wstring(f.max + 1000), 7) == f.max);
        CHECK(intSettingFromText(f, L"-5", 7) == f.min);
        CHECK(intSettingFromText(f, L"", 7) == 7);
        CHECK(intSettingFromText(f, L"abc", 7) == 7);
        Settings s;
        s.*f.member = f.min + 1;
        CHECK(parseSettings(serializeSettings(s)).*f.member == f.min + 1);
        CHECK(parseSettings(std::wstring(f.key) + L"=99999999999999").*f.member == f.max); // 讀進來時夾進範圍
    }
}

TEST_CASE("mergeDialogSettings：整份照抄，只有語言與目前的檢視值不動；回報哪些變了") {
    const std::wstring exeDir = L"C:\\Tools\\";
    Settings cur = parseSettings(L"language=zh-TW\nviewMode=fitwidth\nupscaleSmall=1\nprofile=A\n"
                                 L"[profile]\nname=A\nviewMode=actual\n[profile]\nname=B\nviewMode=twopage\n");
    const Settings before = cur;

    // 什麼都沒改
    DialogChanges none = mergeDialogSettings(cur, before, exeDir);
    CHECK_FALSE((none.contextMenu || none.pinnedNames || none.unzipRoot || none.undoLimits || none.prefetch ||
                 none.profiles || none.reapplyProfile || none.language));
    CHECK(serializeSettings(cur) == serializeSettings(before));

    // 面板改了一堆欄位
    Settings dlg = before;
    dlg.contextMenu = false;
    dlg.askedDefault = true;
    dlg.tipShown = 0;
    dlg.autoCheckUpdate = false;
    dlg.undoLimit = 20;
    dlg.undoMemoryMB = 64;
    dlg.prefetchMB = 0;
    dlg.unzipDir = L"D:\\tmp";
    dlg.pinnedNames = {L"000.jpg"};
    dlg.passwords = {L"pw"};
    dlg.language = L"ja-JP";
    dlg.view.viewMode = ViewMode::TwoPage; // 面板不該動目前的檢視值；就算動了也不收
    DialogChanges c = mergeDialogSettings(cur, dlg, exeDir);
    CHECK(c.contextMenu);
    CHECK(c.pinnedNames);
    CHECK(c.unzipRoot);
    CHECK(c.undoLimits);
    CHECK(c.prefetch);
    CHECK(c.language);
    CHECK_FALSE(c.profiles);
    CHECK_FALSE(c.reapplyProfile);
    CHECK_FALSE(cur.contextMenu);
    CHECK(cur.askedDefault);
    CHECK_FALSE(cur.autoCheckUpdate);
    CHECK(cur.undoLimit == 20);
    CHECK(cur.undoMemoryMB == 64);
    CHECK(cur.prefetchMB == 0);
    CHECK(cur.unzipDir == L"D:\\tmp");
    CHECK(cur.pinnedNames == std::vector<std::wstring>{L"000.jpg"});
    CHECK(cur.passwords == std::vector<std::wstring>{L"pw"});
    CHECK(cur.language == L"zh-TW");               // 語言由呼叫端最後處理
    CHECK(cur.view == before.view);                // 目前的檢視值不動
    CHECK(cur.view.viewMode == ViewMode::FitWidth);

    // 暫存位置：寫法不同但解析後是同一個資料夾，不算變
    Settings same = cur;
    same.unzipDir = L"d:\\TMP";
    CHECK_FALSE(mergeDialogSettings(cur, same, exeDir).unzipRoot);
    Settings blank = before, viaDefault = before;
    viaDefault.unzipDir = L"C:\\Tools\\temp"; // 空的 = exe 旁的 temp
    CHECK_FALSE(mergeDialogSettings(blank, viaDefault, exeDir).unzipRoot);
}

TEST_CASE("mergeDialogSettings：模式的變更與要不要重新套用") {
    const std::wstring exeDir = L"C:\\Tools\\";
    const Settings before = parseSettings(L"profile=A\nviewMode=actual\n"
                                          L"[profile]\nname=A\nviewMode=actual\n[profile]\nname=B\nviewMode=twopage\n");
    // 換到別的模式：要重新套用
    Settings cur = before, dlg = before;
    dlg.profileName = L"B";
    DialogChanges c = mergeDialogSettings(cur, dlg, exeDir);
    CHECK(c.profiles);
    CHECK(c.reapplyProfile);
    CHECK(cur.profileName == L"B");
    CHECK(cur.view.viewMode == ViewMode::Actual); // 套用是呼叫端的事，這裡不動檢視值

    // 改了正在用的模式的內容：要重新套用
    cur = before; dlg = before;
    dlg.profiles[0].prefs.highQuality = true;
    c = mergeDialogSettings(cur, dlg, exeDir);
    CHECK(c.profiles);
    CHECK(c.reapplyProfile);
    CHECK(cur.profiles[0].prefs.highQuality);

    // 改了沒在用的模式、新增一個模式：清單變了，但不必重新套用
    cur = before; dlg = before;
    dlg.profiles[1].prefs.rightToLeft = true;
    dlg.profiles.push_back(Profile{L"C", {}});
    c = mergeDialogSettings(cur, dlg, exeDir);
    CHECK(c.profiles);
    CHECK_FALSE(c.reapplyProfile);
    CHECK(cur.profiles.size() == 3);

    // 把正在用的模式改名（內容沒變）：名字只差大小寫以外的才算換了
    cur = before; dlg = before;
    dlg.profiles[0].name = L"Renamed";
    dlg.profileName = L"Renamed";
    c = mergeDialogSettings(cur, dlg, exeDir);
    CHECK(c.profiles);
    CHECK(c.reapplyProfile);
    CHECK(cur.profileName == L"Renamed");
}

TEST_CASE("mergeSettings：這個視窗改過的用自己的，沒改過的用檔案裡的") {
    const Settings base = parseSettings(L"language=zh-TW\nupscaleSmall=0\nautoNextBook=0\nundoLimit=99\nprofile=A\n"
                                        L"window=10,20,800,600,0\npinnedNames=cover.jpg\n"
                                        L"[profile]\nname=A\nviewMode=actual\n");
    // 兩邊都沒改：就是檔案裡的
    CHECK(serializeSettings(mergeSettings(base, base, base)) == serializeSettings(base));

    // 別的視窗改了 autoNextBook、語言、undoLimit、模式清單、置頂；這個視窗改了 upscaleSmall、視窗位置
    Settings disk = base;
    disk.view.autoNextBook = true;
    disk.language = L"ja-JP";
    disk.undoLimit = 20;
    disk.profiles.push_back(Profile{L"B", {}});
    disk.pinnedNames = {L"000.png"};
    Settings mine = base;
    mine.view.upscaleSmall = true;
    mine.winX = 300;
    mine.winMax = true;
    Settings out = mergeSettings(base, mine, disk);
    CHECK(out.view.upscaleSmall);            // 我改的
    CHECK(out.view.autoNextBook);            // 別人改的：每個檢視開關各自算
    CHECK(out.language == L"ja-JP");
    CHECK(out.undoLimit == 20);
    CHECK(out.profiles.size() == 2);
    CHECK(out.pinnedNames == std::vector<std::wstring>{L"000.png"});
    CHECK(out.winX == 300);                  // 視窗位置五個數字一組
    CHECK(out.winY == 20);
    CHECK(out.winMax);

    // 兩邊改了同一個欄位：這個視窗的贏（後存的贏，但只限那一個欄位）
    Settings both = base;
    both.undoLimit = 50;
    CHECK(mergeSettings(base, both, disk).undoLimit == 50);
    // 每一種欄位都照同一條規則
    Settings all = base;
    all.askedDefault = true;
    all.tipShown = 2;
    all.contextMenu = false;
    all.autoCheckUpdate = false;
    all.skipVersion = L"1.2.3";
    all.updateUrl = L"http://x";
    all.unzipDir = L"D:\\t";
    all.profileName = L"Z";
    all.undoMemoryMB = 32;
    all.prefetchMB = 16;
    all.view.viewMode = ViewMode::FitHeight;
    all.profiles[0].prefs.scrollFlip = true;
    all.pinnedNames = {L"mine.jpg"};
    all.language = L"en";
    Settings o = mergeSettings(base, all, disk);
    CHECK(o.askedDefault);
    CHECK(o.tipShown == 2);
    CHECK_FALSE(o.contextMenu);
    CHECK_FALSE(o.autoCheckUpdate);
    CHECK(o.skipVersion == L"1.2.3");
    CHECK(o.updateUrl == L"http://x");
    CHECK(o.unzipDir == L"D:\\t");
    CHECK(o.profileName == L"Z");
    CHECK(o.undoMemoryMB == 32);
    CHECK(o.prefetchMB == 16);
    CHECK(o.view.viewMode == ViewMode::FitHeight);
    CHECK(o.profiles.size() == 1);           // 模式清單整份：我改過，用我的
    CHECK(o.profiles[0].prefs.scrollFlip);
    CHECK(o.pinnedNames == std::vector<std::wstring>{L"mine.jpg"});
    CHECK(o.language == L"en");
    CHECK(o.undoLimit == 20);                // 我沒改的還是檔案裡的
}

TEST_CASE("mergeSettings：密碼逐筆合併，刪掉的不會被救回來") {
    using List = std::vector<std::wstring>;
    Settings base, mine, disk;
    base.passwords = {L"a", L"b", L"c"};
    // 這個視窗刪了 b、加了 mine；別的視窗刪了 c、加了 theirs
    mine.passwords = {L"a", L"c", L"mine"};
    disk.passwords = {L"a", L"b", L"theirs"};
    CHECK(mergeSettings(base, mine, disk).passwords == List{L"a", L"theirs", L"mine"});
    // 沒人動：檔案裡的順序
    CHECK(mergeSettings(base, base, base).passwords == List{L"a", L"b", L"c"});
    // 兩邊加了同一個：只留一筆
    mine.passwords = {L"a", L"b", L"c", L"same"};
    disk.passwords = {L"a", L"b", L"c", L"same"};
    CHECK(mergeSettings(base, mine, disk).passwords == List{L"a", L"b", L"c", L"same"});
    // 這個視窗全刪：檔案裡別人後來加的還在
    mine.passwords = {};
    disk.passwords = {L"a", L"b", L"c", L"new"};
    CHECK(mergeSettings(base, mine, disk).passwords == List{L"new"});
    // 空白是密碼的一部分
    base.passwords = {};
    mine.passwords = {L" x "};
    disk.passwords = {L"x"};
    CHECK(mergeSettings(base, mine, disk).passwords == List{L"x", L" x "});
}

TEST_CASE("Settings：plugin 的狀態 plugin=<id>|on/off/crashed/conflict，往返、壞行略過、重複留第一個") {
    Settings s = parseSettings(L"[mpainter]\nplugin=jxl|on\nplugin=Foo Bar|off\nplugin=c|crashed\nplugin=d|conflict\n"
                               L"plugin=JXL|off\nplugin=e|maybe\nplugin=|on\nplugin=noBar\nplugin= f | on \n");
    REQUIRE(s.plugins.size() == 5);
    CHECK(s.plugins[0].id == L"jxl");
    CHECK(s.plugins[0].state == PluginState::Enabled); // 後面的 JXL|off 是重複，略過
    CHECK(s.plugins[1].id == L"Foo Bar");
    CHECK(s.plugins[1].state == PluginState::Disabled);
    CHECK(s.plugins[2].state == PluginState::Crashed);
    CHECK(s.plugins[3].state == PluginState::Conflict);
    CHECK(s.plugins[4].id == L"f");
    CHECK(s.plugins[4].state == PluginState::Enabled);

    Settings back = parseSettings(serializeSettings(s));
    REQUIRE(back.plugins.size() == s.plugins.size());
    for (size_t i = 0; i < s.plugins.size(); ++i) {
        CHECK(back.plugins[i].id == s.plugins[i].id);
        CHECK(back.plugins[i].state == s.plugins[i].state);
    }
    // 沒有紀錄就不寫
    CHECK(serializeSettings(Settings{}).find(L"plugin=") == std::wstring::npos);
}

TEST_CASE("findPluginSetting / setPluginState：id 不分大小寫；沒有就加在最後") {
    Settings s;
    CHECK(findPluginSetting(s, L"jxl") == nullptr);
    setPluginState(s, L"jxl", PluginState::Crashed);
    setPluginState(s, L"other", PluginState::Enabled);
    setPluginState(s, L"JXL", PluginState::Enabled);
    REQUIRE(s.plugins.size() == 2);
    CHECK(s.plugins[0].id == L"jxl"); // 原本的名字不變
    REQUIRE(findPluginSetting(s, L"Jxl"));
    CHECK(findPluginSetting(s, L"Jxl")->state == PluginState::Enabled);
}

TEST_CASE("mergeSettings：plugin 逐個合併，這個視窗改過、加的、刪的照自己的，其他照檔案") {
    Settings base, mine, disk;
    setPluginState(base, L"a", PluginState::Enabled);
    setPluginState(base, L"b", PluginState::Enabled);
    setPluginState(base, L"c", PluginState::Enabled);
    mine = base;
    disk = base;
    setPluginState(mine, L"a", PluginState::Disabled);    // 這個視窗關了 a
    mine.plugins.erase(mine.plugins.begin() + 2);          // 這個視窗刪了 c 的紀錄
    setPluginState(mine, L"new", PluginState::Enabled);    // 這個視窗加了 new
    setPluginState(disk, L"b", PluginState::Crashed);      // 別的視窗：b 當掉被停用
    setPluginState(disk, L"theirs", PluginState::Conflict); // 別的視窗加的
    setPluginState(disk, L"A", PluginState::Crashed);      // 兩邊都改了 a：這個視窗的贏
    const Settings out = mergeSettings(base, mine, disk);
    auto state = [&](const wchar_t* id) {
        const PluginSetting* p = findPluginSetting(out, id);
        REQUIRE(p);
        return p->state;
    };
    CHECK(state(L"a") == PluginState::Disabled);
    CHECK(state(L"b") == PluginState::Crashed);
    CHECK(findPluginSetting(out, L"c") == nullptr);
    CHECK(state(L"new") == PluginState::Enabled);
    CHECK(state(L"theirs") == PluginState::Conflict);
    CHECK(out.plugins.size() == 4);
    // 沒人動：照檔案
    CHECK(mergeSettings(base, base, disk).plugins.size() == disk.plugins.size());
}

TEST_CASE("mergeDialogSettings：plugin 狀態照抄，回報有沒有變（順序不算）") {
    Settings cur, dlg;
    setPluginState(cur, L"a", PluginState::Enabled);
    setPluginState(cur, L"b", PluginState::Crashed);
    setPluginState(dlg, L"B", PluginState::Crashed);
    setPluginState(dlg, L"A", PluginState::Enabled);
    Settings c1 = cur;
    CHECK_FALSE(mergeDialogSettings(c1, dlg, L"C:\\x\\").plugins);
    setPluginState(dlg, L"b", PluginState::Enabled);
    Settings c2 = cur;
    CHECK(mergeDialogSettings(c2, dlg, L"C:\\x\\").plugins);
    REQUIRE(findPluginSetting(c2, L"b"));
    CHECK(findPluginSetting(c2, L"b")->state == PluginState::Enabled);
    setPluginState(dlg, L"c", PluginState::Disabled); // 多一筆紀錄也算變了
    Settings c3 = cur;
    CHECK(mergeDialogSettings(c3, dlg, L"C:\\x\\").plugins);
}

TEST_CASE("saveSettingsMerged：兩個視窗輪流存，互不蓋掉（v0.9.5：後存的整份蓋掉先存的）") {
    auto path = tempPath(L"merged.ini");
    DeleteFileW(path.c_str());
    // 檔案不存在：直接寫
    Settings first;
    first.passwords = {L"alpha"};
    Settings written;
    REQUIRE(saveSettingsMerged(path, Settings{}, first, written));
    CHECK(written.passwords == std::vector<std::wstring>{L"alpha"});

    // A、B 兩個視窗都從這個檔開始
    Settings a, b;
    REQUIRE(loadSettings(path, a) == SettingsLoad::Loaded);
    REQUIRE(loadSettings(path, b) == SettingsLoad::Loaded);
    Settings baseA = a, baseB = b;
    // A 打開「小圖放大」、學到一個密碼
    a.view.upscaleSmall = true;
    a.passwords.push_back(L"fromA");
    REQUIRE(saveSettingsMerged(path, baseA, a, written));
    baseA = a;
    // B 打開「接著看下一本」
    b.view.autoNextBook = true;
    REQUIRE(saveSettingsMerged(path, baseB, b, written));
    CHECK(written.view.upscaleSmall); // A 的還在
    CHECK(written.view.autoNextBook);
    CHECK(written.passwords == std::vector<std::wstring>{L"alpha", L"fromA"});
    baseB = b; // B 沒有收下 A 的檢視值：它自己畫面上的還是原本的
    // B 再存一次（例如關視窗時存視窗位置）：不會把 A 的改回去
    b.winX = 5; b.winW = 800; b.winH = 600;
    REQUIRE(saveSettingsMerged(path, baseB, b, written));
    Settings onDisk;
    REQUIRE(loadSettings(path, onDisk) == SettingsLoad::Loaded);
    CHECK(onDisk.view.upscaleSmall);
    CHECK(onDisk.view.autoNextBook);
    CHECK(onDisk.passwords == std::vector<std::wstring>{L"alpha", L"fromA"});
    CHECK(onDisk.winW == 800);
    CHECK_FALSE(fileExists(saveTempOf(path)));

    // 檔案在但讀不到：不寫，回 false，原檔不動
    HANDLE lock = CreateFileW(path.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    REQUIRE(lock != INVALID_HANDLE_VALUE);
    a.view.rightToLeft = true;
    CHECK_FALSE(saveSettingsMerged(path, baseA, a, written));
    CloseHandle(lock);
    REQUIRE(loadSettings(path, onDisk) == SettingsLoad::Loaded);
    CHECK_FALSE(onDisk.view.rightToLeft);
    DeleteFileW(path.c_str());
}

TEST_CASE("saveSettingsMerged：兩條執行緒同時各改各的欄位，都不會掉") {
    auto path = tempPath(L"merged_race.ini");
    DeleteFileW(path.c_str());
    REQUIRE(saveSettings(path, Settings{}));
    auto worker = [&](int Settings::*member, int from) {
        Settings mine, base;
        loadSettings(path, mine);
        base = mine;
        for (int i = 0; i < 40; ++i) {
            mine.*member = from + i;
            Settings written;
            if (saveSettingsMerged(path, base, mine, written)) base = mine;
        }
    };
    std::thread t1(worker, &Settings::undoLimit, 100);
    std::thread t2(worker, &Settings::prefetchMB, 200);
    t1.join();
    t2.join();
    Settings end;
    REQUIRE(loadSettings(path, end) == SettingsLoad::Loaded);
    CHECK(end.undoLimit == 139);  // 各自最後一次寫的值都在
    CHECK(end.prefetchMB == 239);
    DeleteFileW(path.c_str());
}
