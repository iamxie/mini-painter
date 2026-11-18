#include <catch2/catch_test_macros.hpp>

#include <windows.h>

#include <regex>

#include "core/Lang.h"
#include "core/TextUtil.h"
#include "test_util.h"

using namespace mp;

namespace {
using mp::test::tempPath;
using mp::test::writeBytes;
} // namespace

TEST_CASE("parseLangText：檔頭、註解、空白、跳脫") {
    LangTable t = parseLangText(L"# 註解\r\n_code = zh-TW\n_name=繁體中文\n\n; 另一種註解\nOpen…=開啟…\n  Save = 儲存  \n"
                                L"Line one\\nLine two=第一行\\n第二行\nA\\=B=甲=乙\nTab\\there=有\\t跳\nback\\\\slash=反\\\\斜\n沒有等號的行\n=沒有 key\n");
    CHECK(t.code == L"zh-TW");
    CHECK(t.name == L"繁體中文");
    CHECK(t.map.at(L"Open…") == L"開啟…");
    CHECK(t.map.at(L"Save") == L"儲存");
    CHECK(t.map.at(L"Line one\nLine two") == L"第一行\n第二行");
    CHECK(t.map.at(L"A=B") == L"甲=乙"); // key 的等號要跳脫，value 的不用
    CHECK(t.map.at(L"Tab\there") == L"有\t跳");
    CHECK(t.map.at(L"back\\slash") == L"反\\斜");
    CHECK(t.map.size() == 6);
}

TEST_CASE("tr：沒有語系時回傳 key，有語系時查表，缺 key 退回 key") {
    mp::test::LangGuard guard;
    CHECK(std::wstring(tr(L"Open…")) == L"Open…");
    LangTable t = parseLangText(L"_code=zh-TW\nOpen…=開啟…\n");
    setActiveLang(t);
    CHECK(std::wstring(tr(L"Open…")) == L"開啟…");
    CHECK(std::wstring(tr(L"Not translated")) == L"Not translated");
    CHECK(trs(L"Open…") == L"開啟…");
    CHECK(activeLang().code == L"zh-TW");
}

TEST_CASE("loadLangFile：UTF-8 含 BOM，不存在回傳 false") {
    std::wstring p = tempPath(L"lang.txt");
    writeBytes(p, "\xEF\xBB\xBF_code=ja-JP\n_name=\xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E\nSave=\xE4\xBF\x9D\xE5\xAD\x98\n");
    LangTable t;
    REQUIRE(loadLangFile(p, t));
    CHECK(t.code == L"ja-JP");
    CHECK(t.name == L"日本語");
    CHECK(t.map.at(L"Save") == L"保存");
    DeleteFileW(p.c_str());
    CHECK_FALSE(loadLangFile(p, t));
}

TEST_CASE("loadLangDir：只收有 _code 的 txt，依檔名排序") {
    std::wstring dir = tempPath(L"langdir");
    CreateDirectoryW(dir.c_str(), nullptr);
    writeBytes(dir + L"\\b-ko.txt", "_code=ko-KR\n_name=Korean\nSave=S\n");
    writeBytes(dir + L"\\a-ja.txt", "_code=ja-JP\n_name=Japanese\nSave=S\n");
    writeBytes(dir + L"\\nocode.txt", "Save=S\n");
    writeBytes(dir + L"\\readme.md", "_code=xx\n");
    auto list = loadLangDir(dir);
    REQUIRE(list.size() == 2);
    CHECK(list[0].code == L"ja-JP");
    CHECK(list[1].code == L"ko-KR");
    for (auto* n : {L"\\b-ko.txt", L"\\a-ja.txt", L"\\nocode.txt", L"\\readme.md"}) DeleteFileW((dir + n).c_str());
    RemoveDirectoryW(dir.c_str());
    CHECK(loadLangDir(dir).empty());
}

TEST_CASE("pickLanguage：使用者設定優先，不可用就退回系統") {
    std::vector<std::wstring> avail = {L"en", L"zh-TW", L"ja-JP"};
    CHECK(pickLanguage(L"ja-JP", L"zh-TW", avail) == L"ja-JP");
    CHECK(pickLanguage(L"ko-KR", L"zh-TW", avail) == L"zh-TW"); // 設定的語系檔不在了
    CHECK(pickLanguage(L"", L"zh-TW", avail) == L"zh-TW");
}

TEST_CASE("pickLanguage：zh 系只有繁中地區對到 zh-TW") {
    std::vector<std::wstring> avail = {L"en", L"zh-TW"};
    CHECK(pickLanguage(L"", L"zh-TW", avail) == L"zh-TW");
    CHECK(pickLanguage(L"", L"zh-HK", avail) == L"zh-TW");
    CHECK(pickLanguage(L"", L"zh-MO", avail) == L"zh-TW");
    CHECK(pickLanguage(L"", L"zh-Hant-TW", avail) == L"zh-TW");
    CHECK(pickLanguage(L"", L"zh-CN", avail) == L"en");
    CHECK(pickLanguage(L"", L"zh-SG", avail) == L"en");
    // lang/ 裡有 zh-CN.txt 就直接用
    CHECK(pickLanguage(L"", L"zh-CN", {L"en", L"zh-TW", L"zh-CN"}) == L"zh-CN");
}

TEST_CASE("pickLanguage：其他語言比對語言部分，沒有退回 en") {
    std::vector<std::wstring> avail = {L"en", L"zh-TW", L"ja-JP", L"de"};
    CHECK(pickLanguage(L"", L"ja", avail) == L"ja-JP");
    CHECK(pickLanguage(L"", L"ja-JP", avail) == L"ja-JP");
    CHECK(pickLanguage(L"", L"de-AT", avail) == L"de");
    CHECK(pickLanguage(L"", L"en-US", avail) == L"en");
    CHECK(pickLanguage(L"", L"fr-FR", avail) == L"en");
    CHECK(pickLanguage(L"", L"", avail) == L"en");
    CHECK(pickLanguage(L"", L"JA-jp", avail) == L"ja-JP"); // 大小寫不分，回傳清單裡的寫法
}

TEST_CASE("formatSpecs：列出會吃引數的轉換；不合法的格式回 false") {
    std::wstring s;
    CHECK(formatSpecs(L"plain text", s));
    CHECK(s.empty());
    CHECK(formatSpecs(L"%s: %d%%", s));
    CHECK(s == L"s,d");
    CHECK(formatSpecs(L"%.0f × %.0f pixels/inch", s));
    CHECK(s == L"f,f");
    CHECK(formatSpecs(L"%llu bytes / %lu / %08X / %-5.2ls / %I64d", s));
    CHECK(s == L"llu,lu,X,ls,I64d");
    CHECK(formatSpecs(L"%*d %.*s", s)); // 寫 * 的寬度、精度也吃一個引數
    CHECK(s == L"*,d,*,s");
    CHECK(formatSpecs(L"100%% sure", s));
    CHECK(s.empty());
    CHECK_FALSE(formatSpecs(L"100%", s));      // 結尾單獨的 %
    CHECK(formatSpecs(L"50% off", s));         // 空格是旗標、o 是轉換：對 printf 來說是合法的
    CHECK(s == L"o");
    CHECK_FALSE(formatSpecs(L"%n", s));        // 一律不收
    CHECK_FALSE(formatSpecs(L"%s %n", s));
    CHECK_FALSE(formatSpecs(L"% 以上", s));    // % 後面不是轉換
    CHECK_FALSE(formatSpecs(L"%5", s));
}

TEST_CASE("parseLangText：佔位符和 key 不一樣的翻譯丟掉，記在 rejected") {
    LangTable t = parseLangText(L"_code=xx\n"
                                L"Cannot open: %s=無法開啟：%s\n"          // 一樣：收
                                L"Saved %d of %d=存了 %d 個，共 %d 個\n"   // 一樣：收
                                L"Bad: %s=壞：%d\n"                        // 型別不同
                                L"More: %s=多：%s %s\n"                    // 多一個
                                L"Less: %s=少\n"                           // 少一個
                                L"Swap: %s %d=換：%d %s\n"                 // 順序不同
                                L"N: %s=%n%s\n"                            // %n
                                L"Plain=普通\n"                            // 都沒有：收
                                L"Plain2=意外的 %s\n"                      // key 沒有，翻譯多了
                                L"Percent=100%\n"                          // 翻譯不是合法的格式
                                L"Half 50%=一半 50%\n"                     // key 本身不是合法的格式：不檢查
                                L"Rate %d%%=比率 %d%%\n");                 // %% 不算
    CHECK(t.map.count(L"Cannot open: %s") == 1);
    CHECK(t.map.count(L"Saved %d of %d") == 1);
    CHECK(t.map.count(L"Plain") == 1);
    CHECK(t.map.count(L"Half 50%") == 1);
    CHECK(t.map.count(L"Rate %d%%") == 1);
    CHECK(t.map.size() == 5);
    CHECK(t.rejected == std::vector<std::wstring>{L"Bad: %s", L"More: %s", L"Less: %s", L"Swap: %s %d", L"N: %s", L"Plain2", L"Percent"});
    mp::test::LangGuard guard;
    setActiveLang(t);
    CHECK(std::wstring(tr(L"Bad: %s")) == L"Bad: %s"); // 丟掉的顯示英文
    CHECK(std::wstring(tr(L"Plain")) == L"普通");
}

TEST_CASE("隨程式發佈的語系檔：沒有任何一行因為佔位符被丟掉") {
    for (auto* name : {L"zh-TW.txt", L"ja-JP.txt"}) {
        LangTable t;
        REQUIRE(loadLangFile(std::wstring(MP_SOURCE_LANG_DIR) + L"/" + name, t));
        INFO("rejected: " << t.rejected.size());
        CHECK(t.rejected.empty());
        CHECK(t.map.size() > 300);
        CHECK_FALSE(t.code.empty());
    }
}

TEST_CASE("隨程式發佈的中日文語系檔：括號一律全形，只有選單的存取鍵「(&F)」是半形") {
    const std::wregex accessKey(LR"(\(&.\))");
    for (auto* name : {L"zh-TW.txt", L"ja-JP.txt"}) {
        LangTable t;
        REQUIRE(loadLangFile(std::wstring(MP_SOURCE_LANG_DIR) + L"/" + name, t));
        for (const auto& [key, value] : t.map) {
            const std::wstring rest = std::regex_replace(value, accessKey, L"");
            INFO(wideToUtf8(name) << ": " << wideToUtf8(key) << " = " << wideToUtf8(value));
            CHECK(rest.find_first_of(L"()") == std::wstring::npos);
        }
    }
}
