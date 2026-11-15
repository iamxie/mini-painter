#pragma once
#include <string>

namespace mp {

// undo 的兩個上限：預設值、允許的範圍、文字 → 數值。Document（執行上限）與 Settings（存 ini、設定面板）共用。

// undo 最多留幾步：預設值與設定面板允許的範圍。
constexpr int kUndoLimitDefault = 99, kUndoLimitMin = 1, kUndoLimitMax = 999;
// 把設定值夾進 [kUndoLimitMin, kUndoLimitMax]。
int clampUndoLimit(int n);
// 使用者輸入或 ini 裡的文字 → 上限。不是數字（含空字串）回 fallback，數字夾進範圍。
int undoLimitFromText(const std::wstring& text, int fallback);

// undo / redo 歷史裡的底圖最多佔多少記憶體（MB = MiB）：預設值與允許的範圍。
constexpr int kUndoMemoryDefaultMB = 512, kUndoMemoryMinMB = 16, kUndoMemoryMaxMB = 65536;
int clampUndoMemoryMB(int n);
int undoMemoryFromText(const std::wstring& text, int fallback);

} // namespace mp
