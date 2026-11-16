#pragma once
#include <functional>

namespace mp {

// 把 [0, rows) 切給幾條執行緒跑 fn(y0, y1)，跑完才回來。工作執行緒用呼叫端的優先順序：
// 預先解碼的背景執行緒（低優先）呼叫時也是低優先，不跟 UI 搶 CPU。
void parallelRows(int rows, const std::function<void(int, int)>& fn);

} // namespace mp
