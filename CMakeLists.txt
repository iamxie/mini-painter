cmake_minimum_required(VERSION 3.25)
project(mpainter VERSION 0.8.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
add_compile_options(/utf-8 /W4 /wd4100 /wd4458 /EHsc)
add_compile_definitions(UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)

find_package(WebP CONFIG REQUIRED)
find_package(Catch2 CONFIG REQUIRED)
# bit7z 只是 7z.dll 的 C++ 包裝（靜態連結，MPL 2.0）；7z.dll 本體（LGPL）執行時從 exe 旁載入，不連進 exe。
find_package(bit7z CONFIG REQUIRED)
# 自動更新解析 GitHub API 回應（header-only，MIT）。
find_package(nlohmann_json CONFIG REQUIRED)
# 官方 7z.dll 的路徑。設了就在建置後複製到 exe 與測試程式旁；沒設時程式不支援壓縮檔、測試會失敗。
set(MP_7Z_DLL "" CACHE FILEPATH "官方 7z.dll（7-zip.org）的路徑，建置後複製到輸出目錄")
function(mp_copy_7z target)
    if(MP_7Z_DLL)
        add_custom_command(TARGET ${target} POST_BUILD
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${MP_7Z_DLL}" "$<TARGET_FILE_DIR:${target}>/7z.dll")
    endif()
endfunction()

# 純邏輯與影像處理，不碰視窗。測試只連這個。
add_library(mp_core STATIC
    src/core/Image.cpp
    src/core/ImageOps.cpp
    src/core/ImageIO.cpp
    src/core/Metadata.cpp
    src/core/FolderNav.cpp
    src/core/Annotation.cpp
    src/core/Document.cpp
    src/core/ColorAdjust.cpp
    src/core/Settings.cpp
    src/core/ShellReg.cpp
    src/core/Lang.cpp
    src/core/Spreads.cpp
    src/core/TempDir.cpp
    src/core/Archive.cpp
    src/core/Shelf.cpp
    src/core/Update.cpp
    src/core/Crop.cpp
)
target_include_directories(mp_core PUBLIC src)
target_link_libraries(mp_core PUBLIC WebP::webp WebP::webpdecoder WebP::webpdemux WebP::libwebpmux bit7z::bit7z nlohmann_json::nlohmann_json windowscodecs ole32 shlwapi propsys version advapi32 winhttp bcrypt)

option(MP_BUILD_APP "建置主程式（關掉時只建 core 和測試）" ON)
if(MP_BUILD_APP)
# 版本號只寫在 project(VERSION)，這裡產生 version.h 給 rc 與關於對話框。
configure_file(src/app/version.h.in "${CMAKE_BINARY_DIR}/generated/app/version.h" @ONLY)
add_executable(mpainter WIN32
    src/app/main.cpp
    src/app/MainWindow.cpp
    src/app/Popups.cpp
    src/app/InfoPanel.cpp
    src/app/Dialogs.cpp
    src/render/Renderer.cpp
    src/app/app.rc
)
target_include_directories(mpainter PRIVATE "${CMAKE_BINARY_DIR}/generated")
target_link_libraries(mpainter PRIVATE mp_core d2d1 dwrite comdlg32 comctl32 shell32 gdi32 user32 uxtheme)
# manifest 由 app.rc 提供，關掉連結器自動產生的那份，否則會重複。
target_link_options(mpainter PRIVATE /MANIFEST:NO)
mp_copy_7z(mpainter)
endif()

add_executable(mp_tests
    src/tests/test_geometry.cpp
    src/tests/test_annotation.cpp
    src/tests/test_document.cpp
    src/tests/test_image_ops.cpp
    src/tests/test_image_io.cpp
    src/tests/test_folder_nav.cpp
    src/tests/test_color_adjust.cpp
    src/tests/test_settings.cpp
    src/tests/test_shell_reg.cpp
    src/tests/test_lang.cpp
    src/tests/test_spreads.cpp
    src/tests/test_metadata_copy.cpp
    src/tests/test_temp_dir.cpp
    src/tests/test_archive.cpp
    src/tests/test_shelf.cpp
    src/tests/test_update.cpp
    src/tests/test_crop.cpp
)
target_link_libraries(mp_tests PRIVATE mp_core Catch2::Catch2WithMain ws2_32)
# 測試用的二進位 fixture（rar 沒有開源 writer，只能放現成檔）。
target_compile_definitions(mp_tests PRIVATE "MP_TEST_FIXTURES=L\"${CMAKE_SOURCE_DIR}/src/tests/fixtures\"")
mp_copy_7z(mp_tests)

enable_testing()
add_test(NAME mp_tests COMMAND mp_tests)
