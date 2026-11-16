#include <catch2/catch_test_macros.hpp>

#include <windows.h>

#include <string>

#include "app/AppSettings.h"
#include "test_util.h"

using namespace mp;
using namespace mp::test;

// 一個視窗的設定（app/AppSettings）：exe 旁邊的 mpainter.ini 與 lang\。這裡用暫存資料夾假裝是 exe 所在的地方。

namespace {

// 一個空的「程式資料夾」，回傳裡面那個（不存在的）exe 的路徑。
std::wstring appFolder(const wchar_t* name) { return freshDir(name) + L"\\mpainter.exe"; }

std::wstring iniOf(const std::wstring& exe) { return exe.substr(0, exe.size() - 4) + L".ini"; }

} // namespace

TEST_CASE("AppSettings：沒有設定檔時是預設值；路徑照 exe 的位置") {
    LangGuard guard;
    const std::wstring exe = appFolder(L"appset_default");
    AppSettings s;
    s.load(exe, nullptr);
    CHECK(s.exePath() == exe);
    CHECK(s.exeDir() + L"mpainter.exe" == exe);
    CHECK(s.path() == iniOf(exe));
    CHECK_FALSE(s.unreadable());
    CHECK(s.writable());
    CHECK(s.values.undoLimit == Settings{}.undoLimit);
    // 沒有內建語系的模組、沒有 lang\：只有英文
    REQUIRE(s.languages().size() == 1);
    CHECK(s.languages()[0].code == L"en");
}

TEST_CASE("AppSettings：讀得到設定檔的值；存檔寫回同一個檔") {
    LangGuard guard;
    const std::wstring exe = appFolder(L"appset_roundtrip");
    writeBytes(iniOf(exe), "[mpainter]\nundoLimit=7\nlanguage=en\n");
    AppSettings s;
    s.load(exe, nullptr);
    CHECK(s.values.undoLimit == 7);
    s.values.undoLimit = 12;
    CHECK(s.save());
    AppSettings again;
    again.load(exe, nullptr);
    CHECK(again.values.undoLimit == 12);
}

TEST_CASE("AppSettings：兩個視窗輪流存不互蓋；別的視窗學到的密碼會收下來，檢視值各用各的") {
    LangGuard guard;
    const std::wstring exe = appFolder(L"appset_merge");
    writeBytes(iniOf(exe), "[mpainter]\nundoLimit=7\n");
    AppSettings a, b;
    a.load(exe, nullptr);
    b.load(exe, nullptr);
    a.values.undoLimit = 20;
    a.values.passwords.push_back(L"secret");
    REQUIRE(a.save());
    b.values.view.upscaleSmall = !b.values.view.upscaleSmall;
    const bool upscale = b.values.view.upscaleSmall;
    REQUIRE(b.save());
    // b 沒改 undoLimit：檔案裡留著 a 寫的；b 自己手上的值不跟著變（這個視窗繼續用自己的）
    AppSettings c;
    c.load(exe, nullptr);
    CHECK(c.values.undoLimit == 20);
    CHECK(c.values.view.upscaleSmall == upscale);
    CHECK(b.values.undoLimit == 7);
    // a 學到的密碼，b 存檔之後就拿到了
    REQUIRE(b.values.passwords.size() == 1);
    CHECK(b.values.passwords[0] == L"secret");
}

TEST_CASE("AppSettings：設定檔被別的程式獨佔時算讀不到，整次執行都不存") {
    LangGuard guard;
    const std::wstring exe = appFolder(L"appset_locked");
    writeBytes(iniOf(exe), "[mpainter]\nundoLimit=7\n");
    HANDLE lock = CreateFileW(iniOf(exe).c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    REQUIRE(lock != INVALID_HANDLE_VALUE);
    AppSettings s;
    s.load(exe, nullptr); // 重試約 1 秒
    CloseHandle(lock);
    CHECK(s.unreadable());
    CHECK(s.values.undoLimit == Settings{}.undoLimit); // 用預設值
    s.values.undoLimit = 55;
    CHECK_FALSE(s.save()); // 存了會蓋掉原本的設定：不存
    CHECK_FALSE(s.writable());
    CHECK(readBytes(iniOf(exe)) == "[mpainter]\nundoLimit=7\n");
}

TEST_CASE("AppSettings：lang\\ 裡的語系檔加進清單，設定指定的那個會啟用") {
    LangGuard guard;
    const std::wstring exe = appFolder(L"appset_lang");
    const std::wstring dir = exe.substr(0, exe.find_last_of(L'\\'));
    writeBytes(dir + L"\\lang\\xx.txt", "_code=xx-XX\n_name=Test Lang\nShapes=SHAPES-XX\n");
    writeBytes(iniOf(exe), "[mpainter]\nlanguage=xx-XX\n");
    AppSettings s;
    s.load(exe, nullptr);
    REQUIRE(s.languages().size() == 2);
    CHECK(s.languages()[0].code == L"en");
    CHECK(s.languages()[1].code == L"xx-XX");
    CHECK(s.languages()[1].name == L"Test Lang");
    CHECK(std::wstring(tr(L"Shapes")) == L"SHAPES-XX"); // 已經啟用
}
