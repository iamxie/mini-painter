@echo off
rem ASCII only in this file: cmd.exe reads it in the OEM code page and UTF-8 text breaks line parsing.
rem
rem Builds and tests on the VM. Usage: vm-build.cmd [extra cmake arguments, e.g. -DMP_BUILD_APP=OFF]
rem scripts\vm-sync.sh uploads this to C:\work\mini-painter\ and runs it.
rem All build settings are in the "release" preset of CMakePresets.json; this file only prepares the
rem environment. CI (build.yml) uses the same preset.
set BT=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools
set PATH=%PATH%;C:\tools\git\cmd
call "%BT%\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 goto :novc
where cl >nul 2>nul
if errorlevel 1 goto :novc
set PATH=%BT%\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin;%BT%\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja;%PATH%
rem Forward slashes: the \t in C:\tools would be read as an escape inside CMake strings.
set VCPKG_ROOT=C:/tools/vcpkg
if not defined MP_7Z_DLL set MP_7Z_DLL=C:/work/7zip/7z.dll
cd /d C:\work\mini-painter || exit /b 1
cmake --preset release %* || exit /b 1
rem -k 0: keep going after a failed file, so one round shows every compile error.
cmake --build --preset release -- -k 0 || exit /b 1
echo === tests ===
ctest --preset release || exit /b 1
echo === build ok ===
exit /b 0

:novc
echo vcvars64.bat failed or cl.exe is not on PATH: "%BT%"
exit /b 1
