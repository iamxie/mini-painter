// core/Commands：指令表、可用條件、按鍵對應。
#include <catch2/catch_test_macros.hpp>

#include <set>
#include <string>

#include "core/Commands.h"

using namespace mp;

namespace {
// 和 winuser.h 同值。
constexpr unsigned kBack = 0x08, kPrior = 0x21, kNext = 0x22, kLeft = 0x25, kUp = 0x26, kRight = 0x27, kDown = 0x28, kDelete = 0x2E,
                   kNumpad0 = 0x60, kAdd = 0x6B, kSubtract = 0x6D, kOemPlus = 0xBB, kOemMinus = 0xBD, kEscape = 0x1B;

CommandState editing() { // 開著一張改過的圖、有選取、有復原與重做
    CommandState s;
    s.hasImage = s.hasView = s.dirty = s.canUndo = s.canRedo = s.hasSelection = s.hasPages = s.canBrowse = true;
    return s;
}
} // namespace

TEST_CASE("commands：每個 id 一列、名稱不是空的、不落在語言與模式的範圍裡") {
    std::set<int> seen;
    for (const CommandInfo& c : commands()) {
        INFO("command " << c.id);
        CHECK(seen.insert(c.id).second);
        CHECK(std::wstring(c.name).size() > 0);
        CHECK_FALSE(isLanguageCommand(c.id));
        CHECK_FALSE(isProfileCommand(c.id));
        CHECK(commandInfo(c.id) == &c);
    }
    CHECK(commandInfo(IDM_NONE) == nullptr);
    CHECK(commandInfo(IDM_LANG_FIRST) == nullptr);
    CHECK(std::wstring(commandInfo(IDM_SAVE)->shortcut) == L"Ctrl+S");
    CHECK(commandInfo(IDM_SETTINGS)->shortcut == nullptr);
}

TEST_CASE("B-23：語言選單再多個語系也不會撞到「關於」或別的指令") {
    for (int id = IDM_LANG_FIRST; id <= IDM_LANG_LAST; ++id) {
        INFO("language id " << id);
        CHECK(isLanguageCommand(id));
        CHECK(commandInfo(id) == nullptr); // v0.9.5：第 12 個語系的 id（161）就是「關於」
    }
    for (int id = IDM_PROFILE_FIRST; id <= IDM_PROFILE_LAST; ++id) CHECK(commandInfo(id) == nullptr);
    CHECK_FALSE(isLanguageCommand(IDM_ABOUT));
    CHECK_FALSE(isLanguageCommand(IDM_LANG_SYSTEM));
}

TEST_CASE("檢視模式的五個指令之間沒有別的指令（選單的單選圓點用這段範圍）") {
    const std::set<int> modes = {IDM_ZOOM100, IDM_ZOOMFIT, IDM_FITWIDTH, IDM_FITHEIGHT, IDM_TWOPAGE};
    for (int id : modes) {
        CHECK(id >= kViewModeCommandFirst);
        CHECK(id <= kViewModeCommandLast);
    }
    for (const CommandInfo& c : commands())
        if (c.id >= kViewModeCommandFirst && c.id <= kViewModeCommandLast) CHECK(modes.count(c.id) == 1);
}

TEST_CASE("commandEnabled：沒有圖、沒有修改、兩頁並排") {
    CommandState none; // 什麼都沒開
    for (int id : {IDM_SAVE, IDM_SAVEAS, IDM_UNDO, IDM_REDO, IDM_DELETE, IDM_ROTATE, IDM_ZOOMIN, IDM_ZOOMOUT, IDM_ZOOM100,
                   IDM_ZOOMFIT, IDM_FITWIDTH, IDM_FITHEIGHT, IDM_TWOPAGE, IDM_PREV, IDM_NEXT, IDM_PROFILE_SAVE})
        CHECK_FALSE(commandEnabled(id, none));
    for (int id : {IDM_OPEN, IDM_EXIT, IDM_SETTINGS, IDM_UPSCALE, IDM_RTL, IDM_FIRSTALONE, IDM_SHIFTONE, IDM_AUTONEXT,
                   IDM_MARKUP, IDM_INFO, IDM_ABOUT, IDM_LANG_SYSTEM, IDM_LANG_LAST, IDM_PROFILE_FIRST, IDM_PROFILE_MANAGE})
        CHECK(commandEnabled(id, none));

    CommandState s = editing();
    for (const CommandInfo& c : commands()) // 縮圖大小只在瀏覽資料夾時能選（test_browse.cpp）
        if (c.id != IDM_PROFILE_SAVE && !(c.id >= kThumbSizeCommandFirst && c.id <= kThumbSizeCommandLast)) CHECK(commandEnabled(c.id, s));

    // B-19：沒有修改時「儲存」不能用（快捷鍵也是這一份條件），「輸出成」照樣可以
    s.dirty = false;
    CHECK_FALSE(commandEnabled(IDM_SAVE, s));
    CHECK(commandEnabled(IDM_SAVEAS, s));
    s = editing();
    s.canUndo = false;
    CHECK_FALSE(commandEnabled(IDM_UNDO, s));
    CHECK(commandEnabled(IDM_REDO, s));
    s = editing();
    s.hasSelection = false;
    CHECK_FALSE(commandEnabled(IDM_DELETE, s));

    // 兩頁並排只看不編：復原、重做、儲存、刪除、旋轉、標示工具列都不能用；檢視與翻頁照常
    s = editing();
    s.twoPage = true;
    for (int id : {IDM_UNDO, IDM_REDO, IDM_SAVE, IDM_DELETE, IDM_ROTATE, IDM_MARKUP}) CHECK_FALSE(commandEnabled(id, s));
    for (int id : {IDM_SAVEAS, IDM_ZOOMIN, IDM_TWOPAGE, IDM_ZOOMFIT, IDM_PREV, IDM_NEXT, IDM_INFO, IDM_OPEN}) CHECK(commandEnabled(id, s));

    // 模式：有臨時調整才能「儲存目前設定到…」
    s = editing();
    CHECK_FALSE(commandEnabled(IDM_PROFILE_SAVE, s));
    s.profileModified = true;
    CHECK(commandEnabled(IDM_PROFILE_SAVE, s));

    // 翻頁：清單裡有圖就能按（到底時是提示或跳本），沒有就不能
    s = editing();
    s.hasPages = false;
    CHECK_FALSE(commandEnabled(IDM_PREV, s));
    CHECK_FALSE(commandEnabled(IDM_NEXT, s));
}

TEST_CASE("commandEnabled：兩頁並排、第一頁讀不到只有第二頁時，檢視指令照常、編輯與輸出不行") {
    CommandState s;
    s.hasView = true; // 第二頁讀得到：畫面上有東西
    s.twoPage = true;
    s.hasPages = true;
    for (int id : {IDM_ZOOMIN, IDM_ZOOMOUT, IDM_ZOOM100, IDM_ZOOMFIT, IDM_FITWIDTH, IDM_FITHEIGHT, IDM_TWOPAGE, IDM_PREV, IDM_NEXT}) {
        INFO("command " << id);
        CHECK(commandEnabled(id, s));
    }
    for (int id : {IDM_SAVE, IDM_SAVEAS, IDM_ROTATE, IDM_UNDO, IDM_REDO, IDM_DELETE}) {
        INFO("command " << id);
        CHECK_FALSE(commandEnabled(id, s));
    }
}

TEST_CASE("commandEnabled：裁切模式中復原 / 重做是取消裁切，沒有紀錄也能按") {
    CommandState s;
    s.hasImage = true;
    s.cropping = true;
    CHECK(commandEnabled(IDM_UNDO, s));
    CHECK(commandEnabled(IDM_REDO, s));
    s.twoPage = true;
    CHECK_FALSE(commandEnabled(IDM_UNDO, s));
}

TEST_CASE("B-08：拖曳到一半時所有指令都不能用") {
    CommandState s = editing();
    s.profileModified = true;
    s.dragging = true;
    for (const CommandInfo& c : commands()) {
        INFO("command " << c.id);
        CHECK_FALSE(commandEnabled(c.id, s));
    }
    CHECK_FALSE(commandEnabled(IDM_LANG_FIRST, s));
    CHECK_FALSE(commandEnabled(IDM_PROFILE_FIRST + 1, s));
}

TEST_CASE("commandForKey：目前的按鍵，含選單沒寫出來的") {
    // Ctrl 組合
    CHECK(commandForKey('Z', true, false) == IDM_UNDO);
    CHECK(commandForKey('Z', true, true) == IDM_REDO);  // Ctrl+Shift+Z
    CHECK(commandForKey('Y', true, false) == IDM_REDO);
    CHECK(commandForKey('Y', true, true) == IDM_REDO);
    CHECK(commandForKey('S', true, false) == IDM_SAVE);
    CHECK(commandForKey('S', true, true) == IDM_SAVEAS);
    CHECK(commandForKey('O', true, false) == IDM_OPEN);
    CHECK(commandForKey('O', true, true) == IDM_OPEN);
    CHECK(commandForKey('R', true, false) == IDM_ROTATE);
    CHECK(commandForKey(kOemPlus, true, false) == IDM_ZOOMIN);
    CHECK(commandForKey(kOemPlus, true, true) == IDM_ZOOMIN); // Ctrl+Shift+= 也就是 Ctrl++
    CHECK(commandForKey(kAdd, true, false) == IDM_ZOOMIN);
    CHECK(commandForKey(kOemMinus, true, false) == IDM_ZOOMOUT);
    CHECK(commandForKey(kSubtract, true, false) == IDM_ZOOMOUT);
    CHECK(commandForKey('0', true, false) == IDM_ZOOM100);
    CHECK(commandForKey(kNumpad0, true, false) == IDM_ZOOM100);
    CHECK(commandForKey('9', true, false) == IDM_ZOOMFIT);
    // 不按 Ctrl
    CHECK(commandForKey(kLeft, false, false) == IDM_PREV);
    CHECK(commandForKey(kRight, false, false) == IDM_NEXT);
    CHECK(commandForKey(kPrior, false, false) == IDM_PREV);
    CHECK(commandForKey(kNext, false, false) == IDM_NEXT);
    CHECK(commandForKey(kDelete, false, false) == IDM_DELETE);
    CHECK(commandForKey(kBack, false, false) == IDM_DELETE);
    // 不是指令的
    CHECK(commandForKey('Z', false, false) == IDM_NONE);
    CHECK(commandForKey('S', false, true) == IDM_NONE);
    CHECK(commandForKey(kLeft, true, false) == IDM_NONE);   // Ctrl+← 沒有綁
    CHECK(commandForKey(kDelete, true, false) == IDM_NONE);
    CHECK(commandForKey(kUp, false, false) == IDM_NONE);    // ↑ ↓ 是捲動，不是指令
    CHECK(commandForKey(kDown, false, false) == IDM_NONE);
    CHECK(commandForKey(kEscape, false, false) == IDM_NONE);
    CHECK(commandForKey('Q', true, false) == IDM_NONE);
    // 選單上寫的快捷鍵都真的綁了
    CHECK(commandForKey('S', true, false) == IDM_SAVE);
    for (const CommandInfo& c : commands()) {
        if (!c.shortcut || c.id == IDM_EXIT) continue; // Alt+F4 是系統的
        bool bound = false;
        for (unsigned vk = 1; vk < 256 && !bound; ++vk)
            for (int m = 0; m < 4 && !bound; ++m) bound = commandForKey(vk, (m & 1) != 0, (m & 2) != 0) == c.id;
        INFO("command " << c.id);
        CHECK(bound);
    }
}

TEST_CASE("editLockFor / rotateAllowed：壓縮檔裡的圖能轉不能改；plugin 讀進來的與寫不出來的格式只能輸出成，在壓縮檔裡也一樣") {
    CHECK(editLockFor(false, false, false) == EditLock::None);
    CHECK(editLockFor(true, false, false) == EditLock::Archive);
    CHECK(editLockFor(false, true, false) == EditLock::Plugin);
    CHECK(editLockFor(true, true, false) == EditLock::Plugin);
    CHECK(editLockFor(false, false, true) == EditLock::Format);
    CHECK(editLockFor(true, false, true) == EditLock::Format);
    CHECK(editLockFor(false, true, true) == EditLock::Plugin);
    CHECK(rotateAllowed(EditLock::None));
    CHECK(rotateAllowed(EditLock::Archive));
    CHECK_FALSE(rotateAllowed(EditLock::Plugin));
    CHECK_FALSE(rotateAllowed(EditLock::Format));
}
