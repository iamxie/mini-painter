#pragma once
#include <string>
#include <vector>

#include "core/Archive.h"
#include "core/IoError.h"
#include "core/Metadata.h"
#include "core/Plugins.h"
#include "core/Settings.h"
#include "core/UpdateError.h"

// core 回傳的代碼與英文翻譯 key，在這裡變成顯示給使用者看的句子。不碰視窗，測試程式也編這個檔。
namespace mp {

// 翻譯後的格式字串填一個 / 兩個 %s。key 是字串常值（scripts/lang_check.py 靠 fmt1( / fmt2( 找到它）。
std::wstring fmt1(const wchar_t* key, const std::wstring& a);
std::wstring fmt2(const wchar_t* key, const std::wstring& a, const std::wstring& b);

// 讀寫圖檔的錯誤：說明句，帶 HRESULT 的加「(HRESULT 0x…)」，帶系統錯誤碼的加系統的說明（磁碟滿、被占用、沒權限…）。
std::wstring ioErrorText(const IoError& e);
// 壓縮檔解不開的原因：認得的種類是翻譯好的說明，Windows 的錯誤是系統的說明，其他的是 7-Zip 給的原文。
std::wstring archiveErrorText(const ArchiveError& e);
// 新版本檢查與自動更新的錯誤：說明句（路徑、HTTP 狀態碼已經填進去），帶系統錯誤碼的加系統的說明。
std::wstring updateErrorText(const UpdateError& e);
// 下載發佈包失敗時給使用者看的句子：連線與寫檔的錯誤前面加「下載失敗：」，其他的（檔案損毀、解不開）本身就是完整的句子。
std::wstring updateFailureText(const UpdateError& e);
// Win32 錯誤碼的系統說明（系統語言），去掉結尾的換行與句點。查不到回 L"error <碼>"。
std::wstring systemErrorText(unsigned long code);
// 標註壓不上去（render/Flatten 的 FlattenResult）時給使用者看的原因：說明句，加上失敗的物件種類（英文，不翻譯）、
// 「(HRESULT 0x…)」與系統的說明（查得到才加）。kind 是 nullptr、hr 是 0 的部分不加。
std::wstring flattenErrorText(const wchar_t* kind, long hr);

// 資訊面板的欄位名與值。
std::wstring metaLabelText(const MetaItem& item);
std::wstring metaValueText(const MetaValue& value);

// 檔案大小，例如「1.5 KB」；不到 1 KB 的單位會翻譯。
std::wstring fileSizeText(unsigned long long bytes);

// ---- 讀圖 plugin ----

// 名字加上引號接成一串，例如「A」、「A」和「B」、「A」、「B」和「C」。
std::wstring quotedNameList(const std::vector<std::wstring>& names);
// 啟動時停用 plugin 的說明（對話框）。name / names 是顯示名（pluginDisplayName）。最後附英文的除錯資訊（不翻譯）：
// 當掉的是「(id, 函式, 例外代碼)」，衝突的是「(plugin conflict: id, id: 副檔名)」。
std::wstring pluginCrashText(const std::wstring& name, const PluginCrash& c);
std::wstring pluginConflictText(const std::vector<std::wstring>& names, const PluginConflictGroup& g);
// 這個 plugin 用不了的原因：資訊有問題、介面版本不同、或啟動時載入失敗（loaded 是這次啟動的載入結果，沒試過是 nullptr）。
// 說明句加英文除錯資訊；沒問題回空字串。
std::wstring pluginProblemText(const PluginEntry& e, const PluginLoadResult* loaded);
// 設定頁的狀態欄。checked：面板上勾著沒有；record：設定裡的紀錄（沒有是 nullptr）；loaded 同上。
std::wstring pluginStatusText(const PluginEntry& e, bool checked, const PluginSetting* record, const PluginLoadResult* loaded);

} // namespace mp
