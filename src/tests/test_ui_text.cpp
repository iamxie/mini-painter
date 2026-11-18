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
    setActiveLang(parseLangText(L"_code=zh-TW\nCould not decode this file=無法解碼這個檔案\n%s (HRESULT 0x%08X)=%s（HRESULT 0x%08X）\n"
                                L"Could not replace the original file=無法取代原檔\n%s (%s)=%s（%s）\n"));
    CHECK(ioErrorText(e) == L"無法解碼這個檔案（HRESULT 0x88982F50）");
    // 系統的說明也跟著語系用全形括號（說明本身是系統的語言）
    CHECK(ioErrorText(r) == L"無法取代原檔（" + systemErrorText(ERROR_ACCESS_DENIED) + L"）");
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

TEST_CASE("flattenErrorText：說明句加物件種類（英文）、HRESULT 與系統的說明；查不到說明就只留代碼") {
    CHECK(flattenErrorText(nullptr, 0) == L"Could not draw the annotations onto the image");
    CHECK(flattenErrorText(L"Mosaic", 0) == L"Could not draw the annotations onto the image [Mosaic]");
    // 系統查不到說明的碼：只留代碼，不出現 "error <數字>"
    const long unknown = 0x2FFFFFFF;
    CHECK(flattenErrorText(L"Rect", unknown) == L"Could not draw the annotations onto the image [Rect] (HRESULT 0x2FFFFFFF)");
    // E_OUTOFMEMORY、D2DERR_RECREATE_TARGET（Direct2D 自己的碼，Win10 也查得到說明）：說明接在後面
    for (long hr : {static_cast<long>(E_OUTOFMEMORY), static_cast<long>(0x8899000C)}) {
        CAPTURE(hr);
        const std::wstring text = flattenErrorText(L"Text", hr);
        wchar_t code[16];
        swprintf(code, 16, L"0x%08X", static_cast<unsigned>(hr));
        const std::wstring head = std::wstring(L"Could not draw the annotations onto the image [Text] (HRESULT ") + code + L") (";
        CHECK(text.rfind(head, 0) == 0);
        CHECK(text.size() > head.size() + 1);
        CHECK(text.back() == L')');
        // 不出現 systemErrorText 查不到時的 "error <十進位代碼>"。只看 "error " 不行：英文系統的說明本身就有這個字
        // （D2DERR_RECREATE_TARGET 是 "There has been a presentation error..."）。
        CHECK(text.find(L"error " + std::to_wstring(static_cast<unsigned long>(hr))) == std::wstring::npos);
    }

    mp::test::LangGuard guard;
    setActiveLang(parseLangText(L"_code=zh-TW\nCould not draw the annotations onto the image=無法把標註畫到圖上\n"
                                L"%s (HRESULT 0x%08X)=%s（HRESULT 0x%08X）\n%s (%s)=%s（%s）\n"));
    CHECK(flattenErrorText(L"Mosaic", unknown) == L"無法把標註畫到圖上 [Mosaic]（HRESULT 0x2FFFFFFF）");
    CHECK(flattenErrorText(L"Text", E_OUTOFMEMORY) == L"無法把標註畫到圖上 [Text]（HRESULT 0x8007000E）（" +
                                                         systemErrorText(static_cast<unsigned long>(E_OUTOFMEMORY)) + L"）");
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

TEST_CASE("archiveErrorText：代碼翻成說明，Windows 的錯誤用系統的說明，其他的用 7-Zip 的原文") {
    ArchiveError e;
    e.reason = ArchiveFailure::CrcError;
    CHECK(archiveErrorText(e) == archiveFailureMessage(ArchiveFailure::CrcError)); // 沒有翻譯表：英文 key
    {
        setActiveLang(parseLangText(L"_code=zh-TW\nThe checksum does not match (the archive is damaged)=檢查碼不符\n"));
        CHECK(archiveErrorText(e) == L"檢查碼不符");
        setActiveLang({});
    }
    e.reason = ArchiveFailure::System;
    e.systemError = ERROR_ACCESS_DENIED;
    CHECK(archiveErrorText(e) == systemErrorText(ERROR_ACCESS_DENIED));
    e.systemError = 0; // 沒有錯誤碼：退回一般的說明
    CHECK(archiveErrorText(e) == archiveFailureMessage(ArchiveFailure::System));
    e.reason = ArchiveFailure::Other;
    e.detail = L"Some 7-Zip text";
    CHECK(archiveErrorText(e) == L"Some 7-Zip text");
    e.detail.clear();
    CHECK(archiveErrorText(e) == archiveFailureMessage(ArchiveFailure::Other));
    CHECK(archiveErrorText({}).empty());
}

TEST_CASE("updateErrorText：路徑與狀態碼填進句子，系統錯誤加系統的說明；下載的錯誤前面加「下載失敗」") {
    UpdateError e;
    CHECK(updateErrorText(e).empty());
    e.code = UpdateErrorCode::Cancelled;
    CHECK(updateErrorText(e).empty());

    e.code = UpdateErrorCode::HttpStatus;
    e.httpStatus = 403;
    CHECK(updateErrorText(e) == L"The server answered HTTP 403");
    CHECK(updateFailureText(e) == L"The download failed: The server answered HTTP 403");

    e = {};
    e.code = UpdateErrorCode::RenameFailed;
    e.detail = L"C:\\app\\7z.dll";
    e.systemError = ERROR_ACCESS_DENIED;
    CHECK(updateErrorText(e) == L"Could not rename C:\\app\\7z.dll (" + systemErrorText(ERROR_ACCESS_DENIED) + L")");
    CHECK(updateFailureText(e) == updateErrorText(e)); // 不是下載的錯誤：不加前綴

    e = {};
    e.code = UpdateErrorCode::UnpackFailed;
    e.archive.reason = ArchiveFailure::CrcError;
    CHECK(updateErrorText(e) == std::wstring(L"Could not unpack the update: ") + archiveFailureMessage(ArchiveFailure::CrcError));

    e = {};
    e.code = UpdateErrorCode::ChecksumMismatch;
    CHECK(updateFailureText(e) == L"The downloaded file is damaged (checksum mismatch). Nothing was changed.");

    setActiveLang(parseLangText(L"_code=zh-TW\nThe server answered HTTP %d=伺服器回應 HTTP %d\nThe download failed: %s=下載失敗：%s\n"
                                L"Could not rename %s=無法改名 %s\n%s (%s)=%s（%s）\n"));
    e = {};
    e.code = UpdateErrorCode::HttpStatus;
    e.httpStatus = 500;
    CHECK(updateFailureText(e) == L"下載失敗：伺服器回應 HTTP 500");
    e = {};
    e.code = UpdateErrorCode::RenameFailed;
    e.detail = L"C:\\app\\7z.dll";
    e.systemError = ERROR_ACCESS_DENIED;
    CHECK(updateErrorText(e) == L"無法改名 C:\\app\\7z.dll（" + systemErrorText(ERROR_ACCESS_DENIED) + L"）");
    setActiveLang({});
}

TEST_CASE("editLockText：壓縮檔與 plugin 各一句說明，沒鎖是空字串") {
    CHECK(editLockText(EditLock::None).empty());
    CHECK(editLockText(EditLock::Archive).find(L"inside an archive and is read-only") != std::wstring::npos);
    const std::wstring plugin = editLockText(EditLock::Plugin);
    CHECK(plugin.find(L"opened by a plugin and is read-only") != std::wstring::npos);
    CHECK(plugin.find(L"Export As") != std::wstring::npos);
}
