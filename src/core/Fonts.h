#pragma once
// 字型名稱與文字語系只寫在這裡。app.rc 也 include 這個檔（對話框範本的 FONT），所以上半只能有 #define。
// 現在一律是繁中的字型與語系（日文介面的漢字因此是中文字形）。要讓它跟著介面語言走，改這裡與用到的地方。

// 介面的字型：工具列、資訊面板、彈出選單、提示（Direct2D 畫的），與對話框範本。
#define MP_UI_FONT "Microsoft JhengHei UI"
// 對話框範本的字級（點）。
#define MP_DIALOG_FONT_PT 9
// 文字方塊（標註）的預設字型，使用者可以在「格式」選單改。
#define MP_TEXT_FONT "Microsoft JhengHei"
// DirectWrite 的 locale：選到的字型沒有某個字時，退回哪個語系的字形。
#define MP_TEXT_LOCALE "zh-TW"

#ifndef RC_INVOKED
namespace mp {
// L"" 接一般字串常值會合併成寬字元字串。
inline constexpr wchar_t kUiFont[] = L"" MP_UI_FONT;
inline constexpr wchar_t kTextFont[] = L"" MP_TEXT_FONT;
inline constexpr wchar_t kTextLocale[] = L"" MP_TEXT_LOCALE;
} // namespace mp
#endif
