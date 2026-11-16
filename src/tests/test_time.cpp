#include <catch2/catch_test_macros.hpp>

#include <windows.h>
#include <objbase.h>

#include <chrono>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

#include "core/ImageIO.h"
#include "test_util.h"

using namespace mp;
using namespace mp::test;

// 時間量測，平常不跑（標籤開頭是點）：
//   mp_tests "[.time-wic]"
// 量「對一本書的每一頁讀寬高」與「連續解小圖」各花多久，分兩種執行緒：
//   已經初始化過 COM 的（像 UI 執行緒）、沒有初始化過的（像預先解碼與換本預備的背景執行緒）。

namespace {

constexpr int kPages = 300;

double msSince(std::chrono::steady_clock::time_point t0) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
}

std::vector<std::wstring> makeBook() {
    const std::wstring dir = freshDir(L"time_wic");
    std::vector<std::wstring> files;
    for (int i = 0; i < kPages; ++i) {
        std::wstring path = dir + L"\\" + std::to_wstring(1000 + i) + L".png";
        writePng(path, 64, 96);
        files.push_back(path);
    }
    return files;
}

void measure(const char* thread, const std::vector<std::wstring>& files) {
    int w = 0, h = 0;
    readImageSize(files[0], w, h); // 第一次要載入解碼器，不算
    auto t0 = std::chrono::steady_clock::now();
    for (const auto& f : files) REQUIRE(readImageSize(f, w, h));
    const double sizeMs = msSince(t0);
    t0 = std::chrono::steady_clock::now();
    for (const auto& f : files) REQUIRE(loadImage(f).image != nullptr);
    const double loadMs = msSince(t0);
    std::printf("TIME %-22s readImageSize %d files: %.1f ms (%.3f ms each); loadImage: %.1f ms (%.3f ms each)\n", thread, kPages,
                sizeMs, sizeMs / kPages, loadMs, loadMs / kPages);
    std::fflush(stdout);
}

} // namespace

TEST_CASE("時間：一本 300 頁，逐頁讀寬高與解碼", "[.time-wic]") {
    const auto files = makeBook();
    // 沒有初始化過 COM 的執行緒：背景執行緒就是這樣。
    std::thread([&] { measure("thread without COM", files); }).join();
    // 已經初始化過的：UI 執行緒。
    std::thread([&] {
        const HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        measure("thread with COM", files);
        if (SUCCEEDED(hr)) CoUninitialize();
    }).join();
}
