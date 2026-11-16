#pragma once
#include <windows.h>

#include <memory>
#include <string>

#include "core/Update.h"

namespace mp {

// 自動更新要主視窗幫忙的事。
class UpdateHost {
public:
    virtual HWND updateOwnerWindow() const = 0;         // 對話框的擁有者（主視窗）
    virtual std::wstring updateExePath() const = 0;     // 目前這個 exe 的完整路徑
    virtual std::wstring updateCheckUrl() const = 0;    // 設定檔指定的檢查網址；空字串 = 用預設的
    virtual bool updateVersionSkipped(const std::wstring& version) const = 0; // 使用者選過「略過這個版本」
    virtual void skipUpdateVersion(const std::wstring& version) = 0;          // 記下來並存設定
    virtual bool confirmDiscardBeforeUpdate() = 0;      // 有沒存的修改先問；回 false = 使用者取消
    virtual bool loadArchiveLibraryForUpdate() = 0;     // 解開發佈包要 7z.dll
    virtual std::wstring reopenAfterUpdate() const = 0; // 重開之後要開回哪個檔（沒有回空字串）
    // 檔案換好了：關閉這個視窗（不再問存檔），結束時用 relaunchArgs 啟動新的 exe。
    virtual void closeForRelaunch(const std::wstring& relaunchArgs) = 0;

protected:
    ~UpdateHost() = default;
};

// 請同一個 exe 的其他 mpainter 關閉用的訊息（跨行程，註冊的訊息）。wParam = 要更新的那個行程，lParam = 它的進度框。
// 收到的主視窗回 1 = 會關，0 = 不關（正忙，或使用者在存檔詢問按了取消）。
UINT closeForUpdateMessage();

// 新版本檢查與自動更新在視窗這一側的流程：查、問使用者、下載、關掉其他視窗、換檔。
// 不碰視窗的部分（查最新版、下載核對解開、換檔）在 core/Update、core/Install；對話框在 app/UpdateDialogs。
class UpdateFlow {
public:
    explicit UpdateFlow(UpdateHost& host) : host_(host) {}

    // 啟動時：背景查一次。有新版就對主視窗 post WM_MP_UPDATE_FOUND（主視窗收到後交給 found）。失敗不吭聲。
    void startBackgroundCheck();
    // 背景查到的新版。使用者略過的版本不留。
    void found(std::unique_ptr<ReleaseInfo> release);
    // 有還沒問使用者的新版。
    bool hasPending() const { return pending_ != nullptr; }
    // 問使用者要不要裝待問的那一版（略過 / 稍後 / 安裝）。呼叫前主視窗要先確定現在可以跳對話框。
    void offerPending();
    // 「關於」的檢查更新按鈕：有進度框地查一次，有新版就問，沒有就說已經是最新。
    void checkNow();

private:
    void offer(const ReleaseInfo& r);
    // 其他視窗會被關的提示 → 存檔確認 → 下載核對解開 → 關掉其他 mpainter → 換檔 → 請主視窗關閉並重啟。
    void install(const ReleaseInfo& r);
    // 請同一個 exe 的其他 mpainter 關閉並等它們結束；有一個不關回 false。
    bool closeOtherInstances();
    // 不能自動更新時說明原因，問要不要打開下載頁。
    void manualDownloadFallback(const std::wstring& reason, const std::wstring& pageUrl);

    UpdateHost& host_;
    std::unique_ptr<ReleaseInfo> pending_; // 背景查到、還沒問使用者的新版
};

} // namespace mp
