#include "core/Commands.h"

#include "core/Lang.h"

namespace mp {

// 語言與模式的兩段 id 不和任何固定指令重疊（以前「關於」的 id 落在語言那一段裡，第 12 個語系會開成「關於」）。
static_assert(IDM_LANG_FIRST <= IDM_LANG_LAST && IDM_PROFILE_FIRST <= IDM_PROFILE_LAST, "ranges");
static_assert(IDM_LANG_LAST < IDM_PROFILE_SAVE, "language ids must stay below the profile commands");
static_assert(IDM_ABOUT < IDM_LANG_FIRST && IDM_LANG_SYSTEM < IDM_LANG_FIRST, "fixed commands must stay out of the language range");
static_assert(IDM_PROFILE_MANAGE < IDM_PROFILE_FIRST, "fixed commands must stay out of the profile range");

namespace {

// 虛擬鍵碼（和 winuser.h 的 VK_* 同值；core 的標頭不 include <windows.h>）。
constexpr unsigned kVkBack = 0x08, kVkPrior = 0x21, kVkNext = 0x22, kVkLeft = 0x25, kVkRight = 0x27, kVkDelete = 0x2E,
                   kVkNumpad0 = 0x60, kVkAdd = 0x6B, kVkSubtract = 0x6D, kVkOemPlus = 0xBB, kVkOemMinus = 0xBD;

enum class Shift { Any, No, Yes };

struct KeyBinding {
    unsigned vk;
    bool ctrl;
    Shift shift;
    int id;
};

const KeyBinding kKeys[] = {
    {'Z', true, Shift::No, IDM_UNDO},
    {'Z', true, Shift::Yes, IDM_REDO},
    {'Y', true, Shift::Any, IDM_REDO},
    {'S', true, Shift::No, IDM_SAVE},
    {'S', true, Shift::Yes, IDM_SAVEAS},
    {'O', true, Shift::Any, IDM_OPEN},
    {'R', true, Shift::Any, IDM_ROTATE},
    {kVkOemPlus, true, Shift::Any, IDM_ZOOMIN},
    {kVkAdd, true, Shift::Any, IDM_ZOOMIN},
    {kVkOemMinus, true, Shift::Any, IDM_ZOOMOUT},
    {kVkSubtract, true, Shift::Any, IDM_ZOOMOUT},
    {'0', true, Shift::Any, IDM_ZOOM100},
    {kVkNumpad0, true, Shift::Any, IDM_ZOOM100},
    {'9', true, Shift::Any, IDM_ZOOMFIT},
    {kVkLeft, false, Shift::Any, IDM_PREV},
    {kVkRight, false, Shift::Any, IDM_NEXT},
    {kVkPrior, false, Shift::Any, IDM_PREV},
    {kVkNext, false, Shift::Any, IDM_NEXT},
    {kVkDelete, false, Shift::Any, IDM_DELETE},
    {kVkBack, false, Shift::Any, IDM_DELETE},
};

} // namespace

const std::vector<CommandInfo>& commands() {
    static const std::vector<CommandInfo> table = {
        {IDM_OPEN, trKey(L"Open…"), L"Ctrl+O"},
        {IDM_SAVE, trKey(L"Save"), L"Ctrl+S"},
        {IDM_SAVEAS, trKey(L"Export As…"), L"Ctrl+Shift+S"},
        {IDM_EXIT, trKey(L"Exit"), L"Alt+F4"},
        {IDM_UNDO, trKey(L"Undo"), L"Ctrl+Z"},
        {IDM_REDO, trKey(L"Redo"), L"Ctrl+Y"},
        {IDM_DELETE, trKey(L"Delete Selected Object"), L"Del"},
        {IDM_ROTATE, trKey(L"Rotate 90° Counterclockwise"), L"Ctrl+R"},
        {IDM_SETTINGS, trKey(L"Settings…"), nullptr},
        {IDM_ZOOMIN, trKey(L"Zoom In"), L"Ctrl++"},
        {IDM_ZOOMOUT, trKey(L"Zoom Out"), L"Ctrl+-"},
        {IDM_ZOOMFIT, trKey(L"Fit to Window"), L"Ctrl+9"},
        {IDM_FITWIDTH, trKey(L"Fit to Width"), nullptr},
        {IDM_FITHEIGHT, trKey(L"Fit to Height"), nullptr},
        {IDM_ZOOM100, trKey(L"Actual Size"), L"Ctrl+0"},
        {IDM_TWOPAGE, trKey(L"Two Pages Side by Side"), nullptr},
        {IDM_UPSCALE, trKey(L"Enlarge Small Images to Fit"), nullptr},
        {IDM_RTL, trKey(L"Right to Left (Manga)"), nullptr},
        {IDM_FIRSTALONE, trKey(L"First Page Alone"), nullptr},
        {IDM_SHIFTONE, trKey(L"Shift Pairing by One Page"), nullptr},
        {IDM_PREV, trKey(L"Previous Image"), L"←"},
        {IDM_NEXT, trKey(L"Next Image"), L"→"},
        {IDM_AUTONEXT, trKey(L"Continue to the next folder or archive at the end"), nullptr},
        {IDM_MARKUP, trKey(L"Markup Toolbar"), nullptr},
        {IDM_INFO, trKey(L"Info Panel"), nullptr},
        {IDM_LANG_SYSTEM, trKey(L"Follow System"), nullptr},
        {IDM_ABOUT, trKey(L"About mpainter…"), nullptr},
        {IDM_PROFILE_MANAGE, trKey(L"Manage Modes…"), nullptr},
        {IDM_PROFILE_SAVE, trKey(L"Save Current Settings to “%s”"), nullptr}, // %s 是模式名，選單打開時才填
    };
    return table;
}

const CommandInfo* commandInfo(int id) {
    for (const auto& c : commands())
        if (c.id == id) return &c;
    return nullptr;
}

bool commandEnabled(int id, const CommandState& s) {
    if (s.dragging) return false;
    switch (id) {
    case IDM_SAVE: return s.hasImage && s.dirty && !s.twoPage;
    case IDM_SAVEAS: return s.hasImage;
    case IDM_UNDO: return (s.canUndo || s.cropping) && !s.twoPage;
    case IDM_REDO: return (s.canRedo || s.cropping) && !s.twoPage;
    case IDM_DELETE: return s.hasSelection && !s.twoPage;
    case IDM_ROTATE: return s.hasImage && !s.twoPage;
    case IDM_MARKUP: return !s.twoPage;
    case IDM_ZOOMIN:
    case IDM_ZOOMOUT:
    case IDM_ZOOM100:
    case IDM_ZOOMFIT:
    case IDM_FITWIDTH:
    case IDM_FITHEIGHT:
    case IDM_TWOPAGE: return s.hasView;
    case IDM_PREV:
    case IDM_NEXT: return s.hasPages;
    case IDM_PROFILE_SAVE: return s.profileModified;
    default: return true;
    }
}

int commandForKey(unsigned vk, bool ctrl, bool shift) {
    for (const KeyBinding& k : kKeys) {
        if (k.vk != vk || k.ctrl != ctrl) continue;
        if (k.shift == Shift::Any || (k.shift == Shift::Yes) == shift) return k.id;
    }
    return IDM_NONE;
}

} // namespace mp
