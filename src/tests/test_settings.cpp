#include <catch2/catch_test_macros.hpp>

#include <windows.h>

#include "core/Settings.h"

using namespace mp;

namespace {
std::wstring tempPath(const wchar_t* name) {
    wchar_t dir[MAX_PATH];
    GetTempPathW(MAX_PATH, dir);
    return std::wstring(dir) + L"mp_test_" + name;
}
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
    CHECK(d.viewMode == L"fitwindow");
    CHECK_FALSE(d.upscaleSmall);
    CHECK_FALSE(d.highQuality);
    CHECK_FALSE(d.scrollFlip);
    CHECK_FALSE(d.rightToLeft);
    CHECK(d.firstPageAlone);
}

TEST_CASE("parseSettings：檢視模式欄位，不認得的模式退回 fitwindow") {
    Settings s = parseSettings(L"viewMode=fitwidth\nupscaleSmall=1\nhighQuality=1\nscrollFlip=1\nrightToLeft=1\nfirstPageAlone=0\n");
    CHECK(s.viewMode == L"fitwidth");
    CHECK(s.upscaleSmall);
    CHECK(s.highQuality);
    CHECK(s.scrollFlip);
    CHECK(s.rightToLeft);
    CHECK_FALSE(s.firstPageAlone);
    CHECK(parseSettings(L"viewMode=twopage").viewMode == L"twopage");
    CHECK(parseSettings(L"viewMode=bogus").viewMode == L"fitwindow");
    CHECK(parseSettings(L"").warnMetadataLoss);
    CHECK_FALSE(parseSettings(L"warnMetadataLoss=0").warnMetadataLoss);
    CHECK_FALSE(parseSettings(serializeSettings(parseSettings(L"warnMetadataLoss=0"))).warnMetadataLoss);
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
    s.viewMode = L"fitheight";
    s.upscaleSmall = s.highQuality = s.scrollFlip = s.rightToLeft = true;
    s.firstPageAlone = false;
    back = parseSettings(serializeSettings(s));
    CHECK(back.viewMode == L"fitheight");
    CHECK(back.upscaleSmall);
    CHECK(back.highQuality);
    CHECK(back.scrollFlip);
    CHECK(back.rightToLeft);
    CHECK_FALSE(back.firstPageAlone);
}

TEST_CASE("loadSettings / saveSettings 檔案往返") {
    std::wstring p = tempPath(L"settings.ini");
    DeleteFileW(p.c_str());
    Settings s;
    CHECK_FALSE(loadSettings(p, s)); // 不存在
    CHECK(s.tipShown == 0);

    s.tipShown = 1;
    s.askedDefault = true;
    REQUIRE(saveSettings(p, s));
    Settings back;
    REQUIRE(loadSettings(p, back));
    CHECK(back.tipShown == 1);
    CHECK(back.askedDefault);
    CHECK(isPathWritable(p));

    SetFileAttributesW(p.c_str(), FILE_ATTRIBUTE_READONLY);
    CHECK_FALSE(isPathWritable(p));
    CHECK_FALSE(saveSettings(p, s));
    SetFileAttributesW(p.c_str(), FILE_ATTRIBUTE_NORMAL);
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
    REQUIRE(loadSettings(p, s));
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
    CHECK_FALSE(parseSettings(L"").autoNextBook);
    Settings s = parseSettings(L"autoNextBook=1\n");
    CHECK(s.autoNextBook);
    CHECK(parseSettings(serializeSettings(s)).autoNextBook);
    s.autoNextBook = false;
    CHECK_FALSE(parseSettings(serializeSettings(s)).autoNextBook);
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

TEST_CASE("Profile：profileFromCurrent / applyProfile / samePrefs / profileModified") {
    Settings s = parseSettings(L"viewMode=fitwidth\nupscaleSmall=1\nhighQuality=1\nscrollFlip=1\nautoNextBook=1\nrightToLeft=1\nfirstPageAlone=0\nwarnMetadataLoss=0\nprofile=X\n");
    Profile p = profileFromCurrent(s);
    CHECK(p.name == L"X");
    CHECK(p.viewMode == L"fitwidth");
    CHECK(p.upscaleSmall);
    CHECK(p.highQuality);
    CHECK(p.scrollFlip);
    CHECK(p.autoNextBook);
    CHECK(p.rightToLeft);
    CHECK_FALSE(p.firstPageAlone);
    CHECK_FALSE(p.warnMetadataLoss);

    Profile q;
    q.name = L"Q";
    q.viewMode = L"bogus";
    Settings t;
    applyProfile(t, q);
    CHECK(t.profileName == L"Q");
    CHECK(t.viewMode == L"fitwindow"); // 不認得的退回
    CHECK(samePrefs(profileFromCurrent(t), Profile{}));
    Profile r = Profile{};
    r.name = L"other name";
    CHECK(samePrefs(r, Profile{})); // 不比名字
    r.scrollFlip = true;
    CHECK_FALSE(samePrefs(r, Profile{}));

    // 沒有這個模式 = 已調整；有且相同 = 沒調整；改一個欄位 = 已調整
    CHECK(profileModified(t));
    t.profiles.push_back(profileFromCurrent(t)); // 同名、同內容（viewMode 已正規化成 fitwindow）
    CHECK_FALSE(profileModified(t));
    t.upscaleSmall = true;
    CHECK(profileModified(t));
}

TEST_CASE("Profile：defaultProfiles 與 ensureProfiles") {
    auto d = defaultProfiles(L"一般模式", L"漫畫模式");
    REQUIRE(d.size() == 2);
    CHECK(d[0].name == L"一般模式");
    CHECK(d[0].viewMode == L"actual");
    CHECK_FALSE(d[0].upscaleSmall);
    CHECK_FALSE(d[0].highQuality);
    CHECK_FALSE(d[0].autoNextBook);
    CHECK(d[0].warnMetadataLoss);
    CHECK(d[1].name == L"漫畫模式");
    CHECK(d[1].viewMode == L"twopage");
    CHECK(d[1].highQuality);
    CHECK(d[1].autoNextBook);
    CHECK_FALSE(d[1].warnMetadataLoss);

    Settings s = parseSettings(L"viewMode=fitwidth\n");
    CHECK(s.profiles.empty());
    CHECK(ensureProfiles(s, L"G", L"C"));
    REQUIRE(s.profiles.size() == 2);
    CHECK(s.profileName == L"G");
    CHECK(s.viewMode == L"actual");     // 目前的值套成第一個模式
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
    CHECK(s.profiles[0].viewMode == L"fitheight");
    CHECK(s.profiles[0].autoNextBook);
    CHECK(s.profiles[0].firstPageAlone);
    CHECK(s.profiles[1].name == L"B");
    CHECK(s.profiles[1].viewMode == L"twopage");
    CHECK(s.profiles[1].rightToLeft);
    CHECK_FALSE(s.profiles[1].firstPageAlone);
    CHECK(s.profiles[2].name == L"C");
    CHECK(s.profiles[2].viewMode == L"fitwindow");
    CHECK(s.profileName == L"B");
    CHECK(s.viewMode == L"actual");   // 頂層是目前的值，不被區段蓋掉
    CHECK(s.unzipDir == L"D:\\u");  // 回到 [mpainter] 後的 key 照收
    CHECK(profileModified(s));         // actual != twopage

    Settings back = parseSettings(serializeSettings(s));
    REQUIRE(back.profiles.size() == 3);
    CHECK(back.profiles[1].name == L"B");
    CHECK(back.profiles[1].rightToLeft);
    CHECK(back.profileName == L"B");
    CHECK(back.viewMode == L"actual");
    CHECK(back.unzipDir == L"D:\\u");
    // 舊版 ini（沒有區段）：目前的值照舊、清單空
    Settings old = parseSettings(L"viewMode=fitwidth\nhighQuality=1\n");
    CHECK(old.viewMode == L"fitwidth");
    CHECK(old.highQuality);
    CHECK(old.profiles.empty());
    CHECK(old.profileName.empty());
}
