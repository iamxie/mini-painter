#pragma once
#include <vector>

namespace mp {

// 兩頁並排時一屏顯示哪些頁（資料夾清單的索引）。second 為 -1 表示單獨一頁。
struct Spread {
    int first = -1;
    int second = -1;
    bool alone() const { return second < 0; }
};

// 依每頁是否「寬大於高」決定配對：
// - 寬圖自動單獨一屏，之後重新配對（跨頁圖本身就是對齊的錨點）。
// - firstAlone：第一張單獨一屏（封面）。
// - shiftOne：配對開始前再讓一張單獨，手動修正錯位。
// 回傳順序就是閱讀順序；是否右到左由顯示端決定。
std::vector<Spread> buildSpreads(const std::vector<bool>& wide, bool firstAlone, bool shiftOne);

// 某一頁在哪一屏。找不到回傳 -1。
int spreadIndexOf(const std::vector<Spread>& spreads, int page);

} // namespace mp
