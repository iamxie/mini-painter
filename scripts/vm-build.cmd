@echo off
rem 在 VM 上建置。用法：vm-build.cmd [額外的 cmake 參數，例如 -DMP_BUILD_APP=OFF]
rem 由 scripts\vm-sync.sh 上傳到 C:\work\mini-painter\ 後執行。
set BT=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools
set CMAKE=%BT%\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe
set NINJA=%BT%\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe
set PATH=%PATH%;C:\tools\git\cmd
set VCPKG_ROOT=C:\tools\vcpkg
call "%BT%\VC\Auxiliary\Build\vcvars64.bat" >nul
cd /d C:\work\mini-painter
"%CMAKE%" -S . -B build -G Ninja -DMP_BUILD_APP=ON -DCMAKE_MAKE_PROGRAM="%NINJA%" -DCMAKE_BUILD_TYPE=Release -DCMAKE_TOOLCHAIN_FILE=C:\tools\vcpkg\scripts\buildsystems\vcpkg.cmake -DVCPKG_TARGET_TRIPLET=x64-windows-static -DMP_7Z_DLL=C:/work/7zip/7z.dll %* || exit /b 1
"%CMAKE%" --build build || exit /b 1
echo === tests ===
build\mp_tests.exe || exit /b 1
echo === build ok ===
