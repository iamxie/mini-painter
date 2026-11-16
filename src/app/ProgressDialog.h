#pragma once
#include <windows.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <thread>

namespace mp {

// 「背景做事 + 進度對話框」的共用框架。work 在背景執行緒跑，UI 執行緒顯示對話框並繼續收訊息。
// 0.3 秒內做完就不顯示對話框（不閃一下）。
//
// 取消只有一種回報方式：使用者按了取消（或關了對話框）之後 cancel 是 true，work 要自己看它、儘快回來；
// 要中斷卡住的呼叫（等網路回應）就給 onCancel。對話框一律等 work 回來才關。
// work 裡不要查翻譯表、不要碰視窗：要顯示的字先在 UI 執行緒備好，透過 setText / setCaption / setPos 送給對話框。
struct ProgressState {
    std::wstring caption, text;
    std::function<void(ProgressState&)> work; // 背景執行緒
    std::function<void()> onCancel;           // UI 執行緒：使用者按下取消的那一刻（可為空）
    std::atomic<bool> cancel{false};
    HWND dlg = nullptr;                       // 對話框（建不出來時是 nullptr，這時 set* 什麼都不做）

    void setPos(uint64_t done, uint64_t total);
    void setText(const std::wstring& t);
    void setCaption(const std::wstring& t);

    // 以下由 runWithProgress 使用。
    std::thread worker;
    bool shown = false;
    bool done = false;
};

// 跑 st.work 並顯示進度對話框，做完才回來。owner 在這期間被停用（和模態對話框一樣）。
// 建不出對話框回 false，work 沒有被執行：呼叫端自己在目前的執行緒做（沒有進度、不能取消）。
bool runWithProgress(HWND owner, ProgressState& st);

} // namespace mp
