#pragma once
#include <string>

#include "core/IoError.h"
#include "core/Metadata.h"

// core 回傳的代碼與英文翻譯 key，在這裡變成顯示給使用者看的句子。不碰視窗，測試程式也編這個檔。
namespace mp {

// 翻譯後的格式字串填一個 / 兩個 %s。key 是字串常值（scripts/lang_check.py 靠 fmt1( / fmt2( 找到它）。
std::wstring fmt1(const wchar_t* key, const std::wstring& a);
std::wstring fmt2(const wchar_t* key, const std::wstring& a, const std::wstring& b);

// 讀寫圖檔的錯誤：說明句，帶 HRESULT 的加「(HRESULT 0x…)」，帶系統錯誤碼的加系統的說明（磁碟滿、被占用、沒權限…）。
std::wstring ioErrorText(const IoError& e);
// Win32 錯誤碼的系統說明（系統語言），去掉結尾的換行與句點。查不到回 L"error <碼>"。
std::wstring systemErrorText(unsigned long code);

// 資訊面板的欄位名與值。
std::wstring metaLabelText(const MetaItem& item);
std::wstring metaValueText(const MetaValue& value);

// 檔案大小，例如「1.5 KB」；不到 1 KB 的單位會翻譯。
std::wstring fileSizeText(unsigned long long bytes);

} // namespace mp
