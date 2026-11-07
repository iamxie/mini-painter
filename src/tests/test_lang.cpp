#include <catch2/catch_test_macros.hpp>

#include <windows.h>

#include <fstream>

#include "core/Lang.h"

using namespace mp;

namespace {
std::wstring tempPath(const wchar_t* name) {
    wchar_t dir[MAX_PATH];
    GetTempPathW(MAX_PATH, dir);
    return std::wstring(dir) + L"mp_test_" + name;
}
void writeBytes(const std::wstring& path, const std::string& bytes) {
    std::ofstream f(path, std::ios::binary);
    f.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}
struct ResetLang {
    ~ResetLang() { setActiveLang({}); }
};
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
    ResetLang guard;
    setActiveLang({});
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
