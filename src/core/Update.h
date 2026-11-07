#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace mp {

// 新版本檢查與自動更新：查 GitHub Releases 最新版 → 下載發佈包 zip → 核對 .sha256 → 解開 →
// 把 exe 旁的檔案換掉（執行中的 exe / 已載入的 7z.dll 不能覆寫但能改名，所以舊檔先改名成 .old）→ 重啟。
// 這裡只有不碰視窗的部分；對話框與重啟在 src/app。

// 預設的檢查網址（ini 的 updateUrl= 可以改，給測試用）。
extern const wchar_t* const kDefaultUpdateUrl;

// "1.2.3" 或 "v1.2.3" → 三段數字。格式不對回 false。
struct Version {
    int major = 0, minor = 0, patch = 0;
};
bool parseVersion(const std::wstring& s, Version& out);
// a < b 回負數、相等 0、a > b 正數。
int compareVersions(const Version& a, const Version& b);
// latest 比 current 新。任一個解析不了回 false。
bool isNewerVersion(const std::wstring& latest, const std::wstring& current);

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

// .sha256 檔內容（"<64 位 hex>  檔名" 或只有 hex）→ 小寫 hex。找不到回空字串。
std::string parseSha256Text(const std::string& text);
// 檔案的 SHA-256（小寫 hex）。讀不到回空字串。
std::string sha256OfFile(const std::wstring& path);

// HTTP(S) GET（WinHTTP，會跟著轉址）。非 200 也算失敗，error 是給人看的英文訊息。
bool httpGetText(const std::wstring& url, std::string& body, std::wstring& error);
// 下載到檔案。progress(done, total) 回 false 取消（total 不知道時是 0）；取消時 error 為空字串。
bool httpDownload(const std::wstring& url, const std::wstring& path,
                  const std::function<bool(uint64_t done, uint64_t total)>& progress, std::wstring& error);

// 資料夾能不能建檔、改名、刪檔（試建一個暫存檔再刪）。
bool isDirWritable(const std::wstring& dir);

// 安裝：stagingDir 是發佈包解開的地方，appDir 是 exe 所在資料夾。
// 發佈包裡的 mpainter.exe 對應到 exeName（使用者可能把 exe 改了名）；其他照相對路徑放。
struct InstallStep {
    std::wstring src; // staging 裡的檔
    std::wstring dst; // 要放到的位置
    std::wstring old; // 執行時填：dst 原本的檔改名成什麼（原本沒有就空）
    bool placed = false;
};
std::vector<InstallStep> planInstall(const std::wstring& stagingDir, const std::wstring& appDir, const std::wstring& exeName);
// 逐一：dst 已存在就改名成 dst.old（被占住就 .old2、.old3…），再把 src 複製過去。
// 任何一步失敗就整批還原（新放的刪掉、舊檔改回原名）並回 false。
bool applyInstall(std::vector<InstallStep>& steps, std::wstring& error);
// 刪掉 dir（含 lang\）裡上次更新留下的 X.old / X.oldN；只刪 X 本身還在的那些（確定是換掉的舊檔）。
// 回傳刪不掉的數量（舊程式還沒結束時刪不掉，下次再試）。
int removeOldFiles(const std::wstring& dir);

} // namespace mp
