@echo off
rem ASCII only in this file: cmd.exe reads it in the OEM code page and UTF-8 text breaks line parsing.
rem
rem Rebuilds the three RAR5 test archives in src\tests\fixtures\. Needs Rar.exe from WinRAR
rem (there is no open-source rar writer).
rem Source files are in src\tests\fixtures\in\: a.png, sub\b.png, sub\<CJK name> c.png (tiny PNGs),
rem Thumbs.db, readme.txt.
rem   plain.rar  no password
rem   pw.rar     -p123456   data encrypted, headers readable
rem   hp.rar     -hp123456  headers encrypted too
rem WinRAR 7 dropped -ma4, so RAR5 only.
rem Usage: scripts\make_rar_fixtures.cmd (from any directory). If Rar.exe is elsewhere: set RAR=<path> first.
setlocal
if not defined RAR set RAR=C:\Program Files\WinRAR\Rar.exe
if not exist "%RAR%" (
    echo Rar.exe not found: "%RAR%". Install WinRAR or set RAR to the path of Rar.exe.
    exit /b 1
)
cd /d "%~dp0..\src\tests\fixtures" || exit /b 1
if not exist in\a.png (
    echo source files not found in "%CD%\in"
    exit /b 1
)
del /q plain.rar pw.rar hp.rar 2>nul
"%RAR%" a -r -ma5 -ep1 -idq plain.rar in\* || exit /b 1
"%RAR%" a -r -ma5 -ep1 -idq -p123456 pw.rar in\* || exit /b 1
"%RAR%" a -r -ma5 -ep1 -idq -hp123456 hp.rar in\* || exit /b 1
echo done: plain.rar pw.rar hp.rar
