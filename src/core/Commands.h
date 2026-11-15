#pragma once
#include <vector>

namespace mp {

// 指令：選單項目與鍵盤快捷鍵共用的 id（WM_COMMAND 的 id）。
// 每個指令的選單文字、快捷鍵、可用條件只寫在 Commands.cpp：選單、工具列、鍵盤都問這裡，
// 所以選單停用的指令，快捷鍵也不會動作。
enum CommandId : int {
    IDM_NONE = 0,
    // 檔案
    IDM_OPEN = 101,
    IDM_SAVE = 102,
    IDM_SAVEAS = 103, // 選單上是「輸出成…」：寫到另一個檔，編輯中的仍是原檔
    IDM_EXIT = 104,
    // 編輯
    IDM_UNDO = 111,
    IDM_REDO = 112,
    IDM_DELETE = 113,
    IDM_ROTATE = 114,
    IDM_SETTINGS = 115,
    // 顯示
    IDM_ZOOMIN = 121,
    IDM_ZOOMOUT = 122,
    // 檢視模式（單選）。選單用 CheckMenuRadioItem 畫圓點，範圍是 kViewModeCommandFirst..Last：
    // 這段號碼裡只能有這五個（Commands.cpp 有檢查）。
    IDM_ZOOM100 = 123, // 「實際大小」
    IDM_ZOOMFIT = 124, // 「符合視窗」
    IDM_FITWIDTH = 127,
    IDM_FITHEIGHT = 128,
    IDM_TWOPAGE = 129,
    IDM_PREV = 131,
    IDM_NEXT = 132,
    IDM_UPSCALE = 133,
    IDM_MARKUP = 135,
    IDM_INFO = 136,
    IDM_RTL = 137,
    IDM_FIRSTALONE = 138,
    IDM_SHIFTONE = 139,
    IDM_AUTONEXT = 140,
    // 語言：IDM_LANG_SYSTEM 是「跟隨系統」，IDM_LANG_FIRST 起每個語系一個 id
    IDM_LANG_SYSTEM = 141,
    IDM_ABOUT = 145,
    IDM_LANG_FIRST = 150,
    IDM_LANG_LAST = 199,
    // 模式（設定集）：顯示 › 模式 子選單
    IDM_PROFILE_SAVE = 211,
    IDM_PROFILE_MANAGE = 212,
    IDM_PROFILE_FIRST = 220,
    IDM_PROFILE_LAST = 249,
};
constexpr int kViewModeCommandFirst = IDM_ZOOM100, kViewModeCommandLast = IDM_TWOPAGE;

struct CommandInfo {
    int id;
    const wchar_t* name;     // 選單文字（英文翻譯 key）
    const wchar_t* shortcut; // 選單上顯示的快捷鍵；沒有就是 nullptr。不進翻譯表
};
// 全部固定的指令（不含語言與模式那兩段依清單產生的 id）。
const std::vector<CommandInfo>& commands();
// 某個指令那一列；不是固定指令回 nullptr。
const CommandInfo* commandInfo(int id);
inline bool isLanguageCommand(int id) { return id >= IDM_LANG_FIRST && id <= IDM_LANG_LAST; }
inline bool isProfileCommand(int id) { return id >= IDM_PROFILE_FIRST && id <= IDM_PROFILE_LAST; }

// 判斷指令能不能用要看的狀態。app 每次要判斷時填一份。
struct CommandState {
    bool hasImage = false;
    bool dirty = false;           // 有沒存的修改
    bool canUndo = false;
    bool canRedo = false;
    bool hasSelection = false;    // 有選取的標註物件
    bool twoPage = false;         // 兩頁並排：只看不編
    bool hasPages = false;        // 清單裡有圖：可以翻頁。到底了再翻會提示「已經是最後一張」或待命跳本，所以到底時也算可用
    bool cropping = false;        // 裁切模式中：復原 / 重做是「取消裁切」，沒有復原紀錄時也能按
    bool profileModified = false; // 目前的檢視值和所用的模式不同（而且那個模式還在）
    bool dragging = false;        // 滑鼠正拖著東西（平移、物件、控制點、塗鴉、裁切框）：這時任何指令都不動作
};
// 這個指令現在能不能用。選單的灰色、工具列按鈕、快捷鍵都用它。語言與模式的 id 一律可用。
bool commandEnabled(int id, const CommandState& s);

// 鍵盤：虛擬鍵碼加 Ctrl / Shift 的狀態對應到哪個指令；不是指令回 IDM_NONE。
// 選單沒寫出來的也在這裡：Ctrl+Shift+Z 重做、Backspace 刪除、PageUp / PageDown 與 ← → 翻頁。
// ← → 在有選取物件時是微移物件，那個情況由呼叫端先處理。
int commandForKey(unsigned vk, bool ctrl, bool shift);

} // namespace mp
