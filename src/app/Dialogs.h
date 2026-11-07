#pragma once
#include <windows.h>

#include <functional>
#include <string>
#include <utility>
#include <vector>

#include "core/Archive.h"
#include "core/ColorAdjust.h"
#include "core/Image.h"
#include "core/Settings.h"

namespace mp {

// 調整尺寸對話框的結果。
struct ResizeResult {
    int width = 0, height = 0;   // 新的像素尺寸（resample 為 false 時與原尺寸相同）
    double dpi = 72;             // 像素/英寸
    bool resample = true;
};

// 把對話框標題與每個子控制項的文字當 key 查 tr()，有翻譯就換掉。
// 對話框範本（app.rc）一律寫英文，在 WM_INITDIALOG 一開始呼叫這個。
void translateDialog(HWND dlg);

// 顯示「調整尺寸」對話框（modal）。回傳 false 表示取消。
bool showResizeDialog(HWND owner, int curW, int curH, double curDpi, ResizeResult& out);

// 「調整顏色」是 modeless 面板，透過這組回呼和主視窗互動。
struct ColorAdjustHost {
    virtual ~ColorAdjustHost() = default;
    virtual const Image& baseImage() = 0;                       // 調整前的底圖
    virtual void previewChanged(const ColorAdjustParams& p) = 0; // 參數變了，主視窗更新預覽
    virtual void commitAdjust(const ColorAdjustParams& p) = 0;   // 按「好」
    virtual void cancelAdjust() = 0;                             // 按取消或關閉
    virtual void beginPickWhite() = 0;                           // 按滴管，等使用者點圖片
};

// 開啟調整顏色面板。同時只會有一個。
void showColorAdjustPanel(HWND owner, ColorAdjustHost* host, float uiScale);
bool isColorAdjustPanelOpen();
// 面板的視窗，給訊息迴圈呼叫 IsDialogMessage 用。沒開時是 nullptr。
HWND colorAdjustPanelHwnd();
// 主視窗在使用者點了圖片後呼叫，更新面板的參數顯示。
void colorAdjustPanelSetParams(const ColorAdjustParams& p);
void closeColorAdjustPanel();

// 「關於」對話框（modal）：圖示、版本、授權。
void showAboutDialog(HWND owner);

// 「設定」（modal PropertySheet：一般 / 模式 / 壓縮檔 / Windows 整合，取消 / 確定 / 套用）。
struct SettingsDialogInfo {
    std::wstring iniPath;
    bool iniWritable = true;
    bool isWin11 = false;
    std::function<void()> onSetDefault; // 按「設為預設圖片程式…」
    std::vector<std::pair<std::wstring, std::wstring>> languages; // (代碼, 名稱)，語言下拉的順序；「跟隨系統」由對話框自己加在最前面
    std::wstring exeDir;                // 算預設暫存位置用
    std::wstring unzipRootInUse;        // 目前實際使用中的暫存根目錄（顯示路徑與用量）
    int startPage = 0;                  // 0 一般、1 模式、2 壓縮檔、3 Windows 整合
    // 按「套用」或「確定」：s 是整份新設定（目前的檢視值照抄進來，對話框不改它們）。對話框還開著，呼叫端自己 diff 後套用。
    std::function<void(const Settings& s)> onApply;
};
void showSettingsDialog(HWND owner, const Settings& s, const SettingsDialogInfo& info);

// 壓縮檔的密碼對話框（modal）。retry 為 true 時多一行「密碼不對」。回傳 false 表示取消。
bool showPasswordDialog(HWND owner, const std::wstring& archiveName, bool retry, std::wstring& password);
// 在背景執行緒跑 extractImages，主執行緒顯示進度（0.3 秒內解完就不顯示），可取消。回傳解壓結果。
ArchiveResult extractWithProgress(HWND owner, const std::wstring& archivePath, const std::wstring& destDir,
                                  const ExtractOptions& opt);
// 背景執行緒逐一用 testPassword 試 passwords，主執行緒顯示「正在嘗試密碼（i/n）」（同樣 0.3 秒內結束就不顯示），可取消。
// 回傳第一個對的索引；都不對回 -1；取消時 cancelled 為 true。
int tryPasswordsWithProgress(HWND owner, const std::wstring& archivePath, const std::vector<std::wstring>& passwords,
                             bool& cancelled);

} // namespace mp
