@echo off
rem 重造 src\tests\fixtures\ 的三個 RAR5 測試檔。需要 WinRAR（Rar.exe）。
rem 內容：a.png、sub\b.png、sub\中文 c.png（小 PNG）、Thumbs.db、readme.txt。
rem   plain.rar  沒有密碼
rem   pw.rar     -p123456   內容加密、檔頭明文
rem   hp.rar     -hp123456  檔頭也加密
rem WinRAR 7 已拿掉 -ma4，所以只有 RAR5。
rem 用法：先把那五個檔放進一個資料夾 in\，在它的上一層執行這個檔。
set RAR=C:\Program Files\WinRAR\Rar.exe
"%RAR%" a -r -ma5 -ep1 -idq plain.rar in\*
"%RAR%" a -r -ma5 -ep1 -idq -p123456 pw.rar in\*
"%RAR%" a -r -ma5 -ep1 -idq -hp123456 hp.rar in\*
