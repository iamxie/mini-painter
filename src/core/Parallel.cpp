#include "core/Parallel.h"

#include <windows.h>

#include <algorithm>
#include <exception>
#include <mutex>
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
    // 工作執行緒裡丟出的例外（多半是配置失敗）不能讓它逃出執行緒（那會讓整個程式結束）：
    // 記下第一個，等每一段都結束再在呼叫端丟出去。
    std::exception_ptr error;
    std::mutex errorLock;
    auto keep = [&](std::exception_ptr e) {
        std::lock_guard<std::mutex> lk(errorLock);
        if (!error) error = e;
    };
    std::vector<std::thread> pool;
    int chunk = (rows + threads - 1) / threads;
    int handed = 0; // [0, handed) 已經交給執行緒
    try {
        pool.reserve(static_cast<size_t>(threads));
        for (int t = 0; t < threads; ++t) {
            int y0 = t * chunk, y1 = std::min(rows, y0 + chunk);
            if (y0 >= y1) break;
            pool.emplace_back([=, &fn, &keep] {
                if (priority != THREAD_PRIORITY_ERROR_RETURN) SetThreadPriority(GetCurrentThread(), priority);
                try {
                    fn(y0, y1);
                } catch (...) {
                    keep(std::current_exception());
                }
            });
            handed = y1;
        }
    } catch (...) {
        // 建不出執行緒（資源不夠）：剩下的在這條執行緒做。
        if (handed < rows) {
            try {
                fn(handed, rows);
            } catch (...) {
                keep(std::current_exception());
            }
        }
    }
    for (auto& th : pool) th.join();
    if (error) std::rethrow_exception(error);
}

} // namespace mp
