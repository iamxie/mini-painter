#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "core/Http.h"
#include "core/Install.h"
#include "core/Sha256.h"
#include "core/UpdateError.h"
#include "core/Version.h"

namespace mp {

// 新版本檢查與自動更新：查 GitHub Releases 最新版 → 下載發佈包 zip → 核對 .sha256 → 解開 →
// 把 exe 旁的檔案換掉 → 重啟。這裡只有不碰視窗的部分；對話框與重啟在 src/app。
// 零件各自一個檔：Version（版本比較）、Http（WinHTTP）、Sha256、Install（換檔、其他行程）。
// 這個檔是 GitHub 的回應格式，以及把零件接起來的兩段流程（fetchLatestRelease、downloadUpdate）。

// 預設的檢查網址（ini 的 updateUrl= 可以改，給測試用）。
extern const wchar_t* const kDefaultUpdateUrl;

// GitHub「最新 release」API 回應裡用得到的部分。
struct ReleaseInfo {
    std::wstring version; // 標籤去掉 v，例如 0.7.0
    std::wstring notes;   // release 說明（Markdown 原文）
    std::wstring pageUrl; // release 網頁
    std::wstring zipUrl;  // 發佈包 mini-painter-<版本>-win64.zip；沒有這個 asset 時空字串
    std::wstring shaUrl;  // 同名 .sha256；沒有時空字串
};
// 解析 API 回應（UTF-8 JSON）。JSON 壞掉或標籤不是版本號回 false。asset 缺了不算失敗（zipUrl / shaUrl 留空）。
bool parseLatestRelease(const std::string& json, ReleaseInfo& out);
// 發佈包 zip 的檔名。
std::wstring packageZipName(const std::wstring& version);

// 查 url 的最新版。連不上回連線的錯誤；回應不是預期的格式回 BadResponse；cancel 被取消回 Cancelled。
UpdateError fetchLatestRelease(const std::wstring& url, ReleaseInfo& out, HttpCancel* cancel = nullptr);

// downloadUpdate 做到哪了。
enum class DownloadStage { Downloading, Verifying, Unpacking };
struct DownloadHooks {
    std::function<void(DownloadStage)> onStage;                  // 換階段時呼叫
    std::function<void(uint64_t done, uint64_t total)> progress; // 下載進度（total 不知道時是 0）
    HttpCancel* cancel = nullptr;                                // 取消用；階段之間也會看
};
// 下載 release 的發佈包到 workDir、核對 .sha256、解到 workDir\pkg，並確認裡面有 mpainter.exe。
// 成功時 stagingDir 是解開的資料夾（接著交給 planInstall）。workDir 要先建好；失敗時裡面可能留著下載到一半的東西，呼叫端整個刪掉。
// 回呼都在呼叫的那條執行緒上。
UpdateError downloadUpdate(const ReleaseInfo& release, const std::wstring& workDir, std::wstring& stagingDir,
                           const DownloadHooks& hooks = {});

} // namespace mp
