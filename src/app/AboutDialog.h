#pragma once
#include <windows.h>

namespace mp {

// 「關於」對話框（modal）：圖示、版本、授權、「檢查更新」按鈕。按了檢查更新回 true（對話框已關，呼叫端去查）。
bool showAboutDialog(HWND owner);

} // namespace mp
