#include "app/UpdateFlow.h"

#include <commctrl.h>
#include <shellapi.h>

#include <algorithm>
#include <thread>
#include <vector>

#include "app/AppIds.h"
#include "app/DialogUtil.h"
#include "app/UiText.h"
#include "app/UpdateDialogs.h"
#include "app/version.h"
#include "core/Lang.h"
#include "core/Launch.h"
#include "core/PathUtil.h"
#include "core/TempDir.h"

namespace mp {

namespace {

// 這些行程的主視窗。
std::vector<HWND> mainWindowsOf(const std::vector<unsigned long>& pids) {
    struct Ctx { const std::vector<unsigned long>* pids; std::vector<HWND> out; } ctx{&pids, {}};
    EnumWindows([](HWND h, LPARAM lp) -> BOOL {
        auto* c = reinterpret_cast<Ctx*>(lp);
        DWORD pid = 0;
        GetWindowThreadProcessId(h, &pid);
        wchar_t cls[64];
        if (std::find(c->pids->begin(), c->pids->end(), pid) != c->pids->end() && GetClassNameW(h, cls, 64) &&
            wcscmp(cls, kMainWindowClass) == 0)
            c->out.push_back(h);
        return TRUE;
    }, reinterpret_cast<LPARAM>(&ctx));
    return ctx.out;
}

std::wstring exeDirOf(const std::wstring& exePath) {
    const size_t slash = exePath.find_last_of(L"\\/");
    return slash == std::wstring::npos ? L"" : exePath.substr(0, slash + 1);
}

} // namespace

UINT closeForUpdateMessage() {
    static const UINT msg = RegisterWindowMessageW(L"mpainter.CloseForUpdate");
    return msg;
}

void UpdateFlow::startBackgroundCheck() {
    // 背景執行緒只拿值，不碰 this：視窗先關了也不會出事（PostMessage 失敗就自己 delete）。
    std::wstring url = host_.updateCheckUrl();
    if (url.empty()) url = kDefaultUpdateUrl;
    HWND hwnd = host_.updateOwnerWindow();
    std::thread([url, hwnd] {
        ReleaseInfo r;
        if (!fetchLatestRelease(url, r).ok()) return; // 自動檢查失敗不吭聲
        if (!isNewerVersion(r.version, MP_VERSION_WSTRING)) return;
        auto* p = new ReleaseInfo(std::move(r));
        if (!PostMessageW(hwnd, WM_MP_UPDATE_FOUND, 0, reinterpret_cast<LPARAM>(p))) delete p;
    }).detach();
}

void UpdateFlow::found(std::unique_ptr<ReleaseInfo> release) {
    if (!release || host_.updateVersionSkipped(release->version)) return;
    pending_ = std::move(release);
}

void UpdateFlow::offerPending() {
    if (!pending_) return;
    const ReleaseInfo r = *pending_;
    pending_.reset();
    offer(r);
}

void UpdateFlow::checkNow() {
    const HWND hwnd = host_.updateOwnerWindow();
    std::wstring url = host_.updateCheckUrl();
    if (url.empty()) url = kDefaultUpdateUrl;
    ReleaseInfo r;
    UpdateError err = checkUpdateWithProgress(hwnd, url, r);
    if (!err.ok()) {
        if (!err.cancelled())
            MessageBoxW(hwnd, fmt1(L"Could not check for updates: %s", updateErrorText(err)).c_str(), kAppName, MB_ICONWARNING);
        return;
    }
    if (!isNewerVersion(r.version, MP_VERSION_WSTRING)) {
        MessageBoxW(hwnd, fmt1(L"mpainter %s is the latest version.", MP_VERSION_WSTRING).c_str(), kAppName, MB_ICONINFORMATION);
        return;
    }
    offer(r);
}

void UpdateFlow::offer(const ReleaseInfo& r) {
    switch (showUpdateDialog(host_.updateOwnerWindow(), r, MP_VERSION_WSTRING)) {
    case UpdateChoice::Skip: host_.skipUpdateVersion(r.version); break;
    case UpdateChoice::Install: install(r); break;
    case UpdateChoice::Later: break;
    }
}

void UpdateFlow::manualDownloadFallback(const std::wstring& reason, const std::wstring& pageUrl) {
    const HWND hwnd = host_.updateOwnerWindow();
    std::wstring msg = reason + L"\n\n" + tr(L"Open the download page to update by hand?");
    if (MessageBoxW(hwnd, msg.c_str(), kAppName, MB_YESNO | MB_ICONINFORMATION) == IDYES)
        ShellExecuteW(hwnd, L"open", pageUrl.empty() ? kReleasesPage : pageUrl.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

bool UpdateFlow::closeOtherInstances() {
    auto pids = otherProcessesOfImage(host_.updateExePath(), GetCurrentProcessId());
    if (pids.empty()) return true;
    std::vector<HANDLE> procs;
    for (auto pid : pids) {
        if (HANDLE h = OpenProcess(SYNCHRONIZE, FALSE, pid)) procs.push_back(h);
        AllowSetForegroundWindow(pid); // 讓那邊的存檔詢問能跳到前面
    }
    bool ok = closeWindowsWithProgress(host_.updateOwnerWindow(), closeForUpdateMessage(), mainWindowsOf(pids), procs);
    for (HANDLE h : procs) CloseHandle(h);
    return ok;
}

void UpdateFlow::install(const ReleaseInfo& r) {
    const HWND hwnd = host_.updateOwnerWindow();
    const std::wstring exePath = host_.updateExePath();
    const std::wstring app = exeDirOf(exePath);
    // 不能自動更新的情況：先講清楚原因，再給下載頁。
    if (r.zipUrl.empty() || r.shaUrl.empty()) {
        manualDownloadFallback(tr(L"This release cannot be installed automatically."), r.pageUrl);
        return;
    }
    if (!host_.loadArchiveLibraryForUpdate()) {
        manualDownloadFallback(tr(L"7z.dll is missing next to mpainter.exe, so the update cannot be unpacked."), r.pageUrl);
        return;
    }
    // 下載之前就能知道的先看：exe 所在的資料夾能不能寫。發佈包解開之後再照實際要放的每個資料夾檢查一次。
    if (!isDirWritable(app)) {
        manualDownloadFallback(fmt1(L"mpainter cannot write to its folder:\n%s", app), r.pageUrl);
        return;
    }
    // 同一個 exe 還有別的 mpainter 開著：先說清楚全部都會被關掉。
    auto others = otherProcessesOfImage(exePath, GetCurrentProcessId());
    if (!others.empty()) {
        wchar_t msg[640];
        swprintf(msg, 640, tr(L"%d other mpainter windows are open. Installing the update closes all of them (each asks to save unsaved changes first); only this one opens again. Continue?"),
                 static_cast<int>(others.size()));
        if (!askOk(hwnd, msg, TD_INFORMATION_ICON, true)) return;
    }
    if (!host_.confirmDiscardBeforeUpdate()) return;

    wchar_t tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    std::wstring work = std::wstring(tmp) + L"mpainter\\update-" + std::to_wstring(GetCurrentProcessId());
    removeDirectoryRecursive(work);
    if (!createDirectories(work)) {
        MessageBoxW(hwnd, fmt1(L"Update failed: %s", L"cannot create " + work).c_str(), kAppName, MB_ICONWARNING);
        return;
    }
    std::wstring staging;
    UpdateError err = downloadUpdateWithProgress(hwnd, r, work, staging);
    if (!err.ok()) {
        removeDirectoryRecursive(work);
        if (!err.cancelled()) MessageBoxW(hwnd, fmt1(L"Update failed: %s", updateFailureText(err)).c_str(), kAppName, MB_ICONWARNING);
        return;
    }
    auto steps = planInstall(staging, app, fileNameOf(exePath));
    if (UpdateError blocked = installTargetsWritable(steps); !blocked.ok()) {
        removeDirectoryRecursive(work);
        manualDownloadFallback(updateErrorText(blocked), r.pageUrl);
        return;
    }
    // 換檔前把其他 mpainter 關掉（下載期間新開的也算）；有一個沒關就整個取消。
    if (!closeOtherInstances()) {
        removeDirectoryRecursive(work);
        SetForegroundWindow(hwnd); // 使用者剛在別的視窗按了取消，焦點在那邊
        askOk(hwnd, tr(L"Another mpainter window was not closed, so the update was cancelled. Nothing was changed."), TD_WARNING_ICON, false);
        return;
    }
    err = applyInstall(steps);
    removeDirectoryRecursive(work);
    if (!err.ok()) {
        MessageBoxW(hwnd, fmt1(L"Update failed: %s", updateErrorText(err) + L"\n\n" + tr(L"Nothing was changed.")).c_str(), kAppName,
                    MB_ICONWARNING);
        return;
    }
    host_.closeForRelaunch(afterUpdateArgs(GetCurrentProcessId(), host_.reopenAfterUpdate()));
}

} // namespace mp
