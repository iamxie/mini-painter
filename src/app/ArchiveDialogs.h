#pragma once
#include <windows.h>

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "core/Archive.h"

namespace mp {

// 壓縮檔的密碼對話框（modal）。retry 為 true 時多一行「密碼不對」。回傳 false 表示取消。
bool showPasswordDialog(HWND owner, const std::wstring& archiveName, bool retry, std::wstring& password);

// 在背景執行緒跑 extractWithPasswords（先照 opt 解，要密碼就試 passwords），主執行緒顯示進度（0.3 秒內做完就不顯示），可取消。
// 試密碼時進度框換成「密碼 / 正在嘗試已儲存的密碼（i/n）」，試出來再換回解壓。取消時 result.status 是 Cancelled。
PasswordExtract extractWithProgress(HWND owner, const std::wstring& archivePath, const std::wstring& destDir,
                                    const ExtractOptions& opt, const std::vector<std::wstring>& passwords = {});

// 等背景正在做的解壓（work 在背景執行緒跑），主執行緒顯示和 extractWithProgress 一樣的進度對話框（0.3 秒內結束就不顯示）。
// work 拿到 progress(done, total)：要定時呼叫它，它回 false 表示使用者按了取消。建不出對話框時同步執行，progress 是空的。
void runUnpackWithProgress(HWND owner, const std::wstring& archivePath,
                           const std::function<void(const std::function<bool(uint64_t, uint64_t)>& progress)>& work);

} // namespace mp
