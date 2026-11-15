// app/UiText：core 的代碼與翻譯 key 變成顯示用的句子。
#include <catch2/catch_test_macros.hpp>

#include <windows.h>

#include <string>

#include "app/UiText.h"
#include "core/Lang.h"
#include "test_util.h"

using namespace mp;

TEST_CASE("fmt1 / fmt2：填進翻譯後的格式；沒有翻譯用 key") {
    CHECK(fmt1(L"Cannot open: %s", L"x.png") == L"Cannot open: x.png");
    CHECK(fmt2(L"In use: %s (%s)", L"D:\\t", L"1.5 KB") == L"In use: D:\\t (1.5 KB)");
    CHECK(fmt1(L"Cannot open: %s", std::wstring(5000, L'a')).size() == 5013); // 長引數不截斷
    mp::test::LangGuard guard;
    setActiveLang(parseLangText(L"_code=zh-TW\nCannot open: %s=無法開啟：%s\nIn use: %s (%s)=使用中：%s（%s）\n"));
    CHECK(fmt1(L"Cannot open: %s", L"x.png") == L"無法開啟：x.png");
    CHECK(fmt2(L"In use: %s (%s)", L"D:\\t", L"1.5 KB") == L"使用中：D:\\t（1.5 KB）");
}

TEST_CASE("B-12：佔位符對不上的翻譯在載入時就丟掉，fmt1 用英文原文，不會讀到不存在的引數") {
    mp::test::LangGuard guard;
    // v0.9.5：第一種翻譯讓 swprintf 讀到垃圾指標（SIGSEGV），第二種（%n）直接結束行程。
    for (const wchar_t* bad : {L"%s %s %s %s", L"%n%s", L"%d", L"沒有佔位符", L"100% %s"}) {
        LangTable t = parseLangText(std::wstring(L"_code=xx\nCannot open: %s=") + bad + L"\n");
        CHECK(t.rejected == std::vector<std::wstring>{L"Cannot open: %s"});
        setActiveLang(t);
        CHECK(fmt1(L"Cannot open: %s", L"x.png") == L"Cannot open: x.png");
    }
}

TEST_CASE("ioErrorText：說明句、HRESULT、系統錯誤、副檔名") {
    IoError e;
    CHECK(ioErrorText(e).empty());
    e.code = IoErrorCode::InvalidWebP;
    CHECK(ioErrorText(e) == L"Not a valid WebP file");
    e.code = IoErrorCode::DecodeFailed;
    e.hresult = static_cast<long>(0x88982F50);
    CHECK(ioErrorText(e) == L"Could not decode this file (HRESULT 0x88982F50)");
    IoError s;
    s.code = IoErrorCode::UnsupportedSaveFormat;
    s.detail = L".gif";
    CHECK(ioErrorText(s) == L"Saving this format is not supported: .gif");
    IoError r;
    r.code = IoErrorCode::ReplaceFailed;
    r.systemError = ERROR_ACCESS_DENIED;
    std::wstring text = ioErrorText(r);
    CHECK(text.rfind(L"Could not replace the original file (", 0) == 0);
    CHECK(text.back() == L')');
    CHECK(text.find(systemErrorText(ERROR_ACCESS_DENIED)) != std::wstring::npos);

    mp::test::LangGuard guard;
    setActiveLang(parseLangText(L"_code=zh-TW\nCould not decode this file=無法解碼這個檔案\n%s (HRESULT 0x%08X)=%s（HRESULT 0x%08X）\n"));
    CHECK(ioErrorText(e) == L"無法解碼這個檔案（HRESULT 0x88982F50）");
}

TEST_CASE("systemErrorText：有說明、沒有結尾的換行與句點；查不到的碼給數字") {
    std::wstring denied = systemErrorText(ERROR_ACCESS_DENIED);
    REQUIRE_FALSE(denied.empty());
    CHECK(denied.back() != L'\n');
    CHECK(denied.back() != L'\r');
    CHECK(denied.back() != L'.');
    CHECK(denied != systemErrorText(ERROR_DISK_FULL));
    CHECK(systemErrorText(0x2FFFFFFF) == L"error 805306367");
}

TEST_CASE("metaLabelText / metaValueText / fileSizeText") {
    MetaItem keyed{L"Make", true, {}};
    MetaItem literal{L"Tag 0x9999", false, {}};
    MetaValue text;
    text.text = L"TestCam";
    MetaValue bytes;
    bytes.kind = MetaValue::Kind::ByteCount;
    bytes.number = 1234;
    MetaValue yes;
    yes.kind = MetaValue::Kind::Bool;
    yes.number = 1;
    MetaValue no = yes;
    no.number = 0;
    MetaValue unknown;
    unknown.kind = MetaValue::Kind::UnknownType;
    unknown.number = 72;
    CHECK(metaLabelText(keyed) == L"Make");
    CHECK(metaLabelText(literal) == L"Tag 0x9999");
    CHECK(metaValueText(text) == L"TestCam");
    CHECK(metaValueText(bytes) == L"(1234 bytes)");
    CHECK(metaValueText(yes) == L"Yes");
    CHECK(metaValueText(no) == L"No");
    CHECK(metaValueText(unknown) == L"(type 72)");
    CHECK(fileSizeText(500) == L"500 bytes");
    CHECK(fileSizeText(1536) == L"1.5 KB");

    mp::test::LangGuard guard;
    setActiveLang(parseLangText(L"_code=zh-TW\nMake=製造商\nTag 0x9999=不該被翻\n(%lu bytes)=（%lu 位元組）\nYes=是\n%llu bytes=%llu 位元組\n"));
    CHECK(metaLabelText(keyed) == L"製造商");
    CHECK(metaLabelText(literal) == L"Tag 0x9999"); // 不是 key 的照字面
    CHECK(metaValueText(bytes) == L"（1234 位元組）");
    CHECK(metaValueText(yes) == L"是");
    CHECK(metaValueText(no) == L"No");
    CHECK(fileSizeText(500) == L"500 位元組");
}
