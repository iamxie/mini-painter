#include "core/Parallel.h"

#include <windows.h>

#include <algorithm>
#include <thread>
#include <vector>

namespace mp {

void parallelRows(int rows, const std::function<void(int, int)>& fn) {
    unsigned hw = std::thread::hardware_concurrency();
    int threads = static_cast<int>(std::clamp<unsigned>(hw ? hw : 2, 1, 16));
    if (rows < 64) threads = 1;
    if (threads == 1) { fn(0, rows); return; }
    // 新執行緒預設是一般優先順序，不會繼承呼叫端的；照抄過去。
    int priority = GetThreadPriority(GetCurrentThread());
    std::vector<std::thread> pool;
    int chunk = (rows + threads - 1) / threads;
    for (int t = 0; t < threads; ++t) {
        int y0 = t * chunk, y1 = std::min(rows, y0 + chunk);
        if (y0 >= y1) break;
        pool.emplace_back([=, &fn] {
            if (priority != THREAD_PRIORITY_ERROR_RETURN) SetThreadPriority(GetCurrentThread(), priority);
            fn(y0, y1);
        });
    }
    for (auto& th : pool) th.join();
}

} // namespace mp
