cmake_minimum_required(VERSION 3.25)
project(mpainter VERSION 1.1.1 LANGUAGES C CXX) # C：DNG SDK 附的 libjpeg

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
add_compile_options(/utf-8 /W4 /WX /wd4100 /wd4458 /EHsc)
add_compile_definitions(UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)

# 版本號寫在兩處（這裡的 project(VERSION) 與 vcpkg.json 的 version-string），不一致就在 configure 階段停下來。
file(READ "${CMAKE_SOURCE_DIR}/vcpkg.json" MP_VCPKG_JSON)
string(JSON MP_VCPKG_VERSION GET "${MP_VCPKG_JSON}" "version-string")
if(NOT MP_VCPKG_VERSION STREQUAL PROJECT_VERSION)
    message(FATAL_ERROR "vcpkg.json version-string (${MP_VCPKG_VERSION}) does not match project(VERSION ${PROJECT_VERSION}) in CMakeLists.txt")
endif()

find_package(WebP CONFIG REQUIRED)
find_package(Catch2 CONFIG REQUIRED)
# bit7z 只是 7z.dll 的 C++ 包裝（靜態連結，MPL 2.0）；7z.dll 本體（LGPL）執行時從 exe 旁載入，不連進 exe。
find_package(bit7z CONFIG REQUIRED)
# 自動更新解析 GitHub API 回應（header-only，MIT）。
find_package(nlohmann_json CONFIG REQUIRED)
# 官方 7z.dll 的路徑。設了就在建置後複製到 exe 與測試程式旁；沒設時程式不支援壓縮檔、測試會失敗。
set(MP_7Z_DLL "" CACHE FILEPATH "官方 7z.dll（7-zip.org）的路徑，建置後複製到輸出目錄")
if(NOT MP_7Z_DLL)
    message(WARNING "MP_7Z_DLL is not set: mpainter cannot open archives and the archive tests in mp_tests will fail. Pass -DMP_7Z_DLL=<path to the official 7z.dll>.")
elseif(NOT EXISTS "${MP_7Z_DLL}")
    message(FATAL_ERROR "MP_7Z_DLL does not exist: ${MP_7Z_DLL}")
endif()
function(mp_copy_7z target)
    if(MP_7Z_DLL)
        add_custom_command(TARGET ${target} POST_BUILD
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${MP_7Z_DLL}" "$<TARGET_FILE_DIR:${target}>/7z.dll")
    endif()
endfunction()

# 純邏輯與影像處理，不碰視窗。
add_library(mp_core STATIC
    src/core/TextUtil.cpp
    src/core/PathUtil.cpp
    src/core/TextFile.cpp
    src/core/SysInfo.cpp
    src/core/IoError.cpp
    src/core/ImageFormat.cpp
    src/core/Image.cpp
    src/core/Parallel.cpp
    src/core/ImageOps.cpp
    src/core/ImageIO.cpp
    src/core/Metadata.cpp
    src/core/FolderNav.cpp
    src/core/Annotation.cpp
    src/core/Document.cpp
    src/core/Editor.cpp
    src/core/ResizeModel.cpp
    src/core/ColorAdjust.cpp
    src/core/Settings.cpp
    src/core/ShellReg.cpp
    src/core/Lang.cpp
    src/core/Spreads.cpp
    src/core/TempDir.cpp
    src/core/Archive.cpp
    src/core/Shelf.cpp
    src/core/UpdateError.cpp
    src/core/Version.cpp
    src/core/Sha256.cpp
    src/core/Http.cpp
    src/core/Install.cpp
    src/core/Update.cpp
    src/core/Crop.cpp
    src/core/ViewFit.cpp
    src/core/BookPrep.cpp
    src/core/Prefetch.cpp
    src/core/Commands.cpp
    src/core/Viewport.cpp
    src/core/ViewState.cpp
    src/core/Paging.cpp
    src/core/Launch.cpp
    src/core/PluginManifest.cpp
    src/core/PluginRun.cpp
    src/core/Plugins.cpp
)
target_include_directories(mp_core PUBLIC src)
# 都是實作細節：core 的標頭不露出這些函式庫的型別。
target_link_libraries(mp_core PRIVATE WebP::webp WebP::webpdecoder WebP::webpdemux WebP::libwebpmux bit7z::bit7z nlohmann_json::nlohmann_json windowscodecs ole32 shlwapi propsys version advapi32 winhttp bcrypt shell32)

# Direct2D / DirectWrite 包裝。主程式與測試共用同一份。
add_library(mp_render STATIC
    src/render/RenderDevice.cpp
    src/render/Surface.cpp
    src/render/BitmapCache.cpp
    src/render/AnnotationPainter.cpp
    src/render/Flatten.cpp
    src/render/Renderer.cpp
)
target_link_libraries(mp_render PUBLIC mp_core PRIVATE d2d1 dwrite)

# JPEG XL 讀圖 plugin（隨附、預設啟用）：建到 plugins\jxl\jxl.dll，主程式與測試程式旁的 plugins\ 底下。
# libjxl（BSD-3-Clause）與它的相依 highway（Apache-2.0）、brotli（MIT）、lcms2（MIT）都靜態連結進這個 DLL。
# vcpkg 的 libjxl 只給 pkg-config，這裡直接找標頭與函式庫；靜態版要定義三個 *_STATIC_DEFINE（pkg-config 的 Cflags 寫的）。
find_path(MP_JXL_INCLUDE jxl/decode.h REQUIRED)
set(MP_JXL_LIBS "")
foreach(lib jxl jxl_cms jxl_threads hwy brotlienc brotlidec brotlicommon lcms2)
    find_library(MP_LIB_${lib} ${lib} REQUIRED)
    list(APPEND MP_JXL_LIBS "${MP_LIB_${lib}}")
endforeach()
set(MP_JXL_DEFS JXL_STATIC_DEFINE JXL_CMS_STATIC_DEFINE JXL_THREADS_STATIC_DEFINE)
add_library(mp_plugin_jxl SHARED src/plugins/jxl/jxl.cpp src/plugins/jxl/jxl.rc)
set_target_properties(mp_plugin_jxl PROPERTIES OUTPUT_NAME jxl
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/plugins/jxl"
    ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/plugin-implib") # 匯入函式庫用不到，別和 libjxl 的 jxl.lib 放在一起
target_include_directories(mp_plugin_jxl PRIVATE src)
target_include_directories(mp_plugin_jxl SYSTEM PRIVATE "${MP_JXL_INCLUDE}")
target_compile_definitions(mp_plugin_jxl PRIVATE ${MP_JXL_DEFS})
target_link_libraries(mp_plugin_jxl PRIVATE ${MP_JXL_LIBS})

# Adobe DNG SDK 1.7.1（DNG SDK License Agreement，src/third_party/dng_sdk/LICENSE.txt）與它附的 libjpeg 9c（IJG）。
# 第三方原始碼照 SDK 自己的 Windows 設定編：換掉我們的警告設定（/W4 /WX）與全域定義（UNICODE 等）。
# 不帶 XMP SDK（qDNGUseXMP=0）：排除 dng_xmp_sdk.cpp 與只做寫入的 dng_update_meta.cpp，dng_jxl.cpp 改了兩處
# （src/third_party/dng_sdk/MODIFICATIONS.txt）。zlib 用 vcpkg 的；JPEG XL 用上面那一份 libjxl。
find_package(ZLIB REQUIRED)
set(MP_DNG_DIR "${CMAKE_SOURCE_DIR}/src/third_party/dng_sdk")
file(GLOB MP_DNG_SDK_SRC CONFIGURE_DEPENDS "${MP_DNG_DIR}/source/*.cpp")
list(REMOVE_ITEM MP_DNG_SDK_SRC "${MP_DNG_DIR}/source/dng_validate.cpp" "${MP_DNG_DIR}/source/dng_xmp_sdk.cpp"
    "${MP_DNG_DIR}/source/dng_update_meta.cpp")
set(MP_LIBJPEG_SRC jaricom jcapimin jcapistd jcarith jccoefct jccolor jcdctmgr jchuff jcinit jcmainct jcmarker jcmaster
    jcomapi jcparam jcprepct jcsample jctrans jdapimin jdapistd jdarith jdatadst jdatasrc jdcoefct jdcolor jddctmgr jdhuff
    jdinput jdmainct jdmarker jdmaster jdmerge jdpostct jdsample jdtrans jerror jfdctflt jfdctfst jfdctint jidctflt jidctfst
    jidctint jquant1 jquant2 jutils jmemmgr jmemnobs)
list(TRANSFORM MP_LIBJPEG_SRC PREPEND "${MP_DNG_DIR}/libjpeg/")
list(TRANSFORM MP_LIBJPEG_SRC APPEND ".c")
add_library(mp_dng_sdk STATIC ${MP_DNG_SDK_SRC} ${MP_LIBJPEG_SRC})
set_property(TARGET mp_dng_sdk PROPERTY COMPILE_OPTIONS /W0 /EHsc /bigobj /MP)
set_property(TARGET mp_dng_sdk PROPERTY COMPILE_DEFINITIONS _CRT_SECURE_NO_DEPRECATE _CRT_SECURE_NO_WARNINGS _CRT_NONSTDC_NO_WARNINGS)
# 用到 SDK 標頭的地方要和 SDK 一樣的旗標，所以是 PUBLIC。標頭當系統標頭：不套我們的 /W4 /WX。
target_compile_definitions(mp_dng_sdk PUBLIC WIN32 WIN64 qWinOS=1 qMacOS=0 qVisualC=1 qDNGUseStdInt=1 qDNGUseLibJPEG=1
    qDNGUseXMP=0 qDNGXMPFiles=0 qDNGXMPDocOps=0 qDNGValidateTarget=0 qDNGValidate=0 qDNGPrintMessages=0 qDNGDebug=0
    ${MP_JXL_DEFS})
target_include_directories(mp_dng_sdk SYSTEM PUBLIC "${MP_DNG_DIR}/source" "${MP_DNG_DIR}/libjpeg" "${MP_JXL_INCLUDE}")
target_link_libraries(mp_dng_sdk PUBLIC ZLIB::ZLIB ${MP_JXL_LIBS})

# DNG 讀圖 plugin（隨附、預設啟用）：plugins\dng\dng.dll。
add_library(mp_plugin_dng SHARED src/plugins/dng/dng.cpp src/plugins/dng/DngRender.cpp src/plugins/dng/dng.rc)
set_target_properties(mp_plugin_dng PROPERTIES OUTPUT_NAME dng
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/plugins/dng"
    ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/plugin-implib")
target_include_directories(mp_plugin_dng PRIVATE src)
target_link_libraries(mp_plugin_dng PRIVATE mp_dng_sdk)

option(MP_BUILD_APP "建置主程式（關掉時只建 core 和測試）" ON)
if(MP_BUILD_APP)
# 版本號只寫在 project(VERSION)，這裡產生 version.h 給 rc 與關於對話框。
configure_file(src/app/version.h.in "${CMAKE_BINARY_DIR}/generated/app/version.h" @ONLY)
add_executable(mpainter WIN32
    src/app/main.cpp
    src/app/MainWindow.cpp
    src/app/Popups.cpp
    src/app/InfoPanel.cpp
    src/app/ToolbarIcons.cpp
    src/app/Toolbar.cpp
    src/app/DialogUtil.cpp
    src/app/ProgressDialog.cpp
    src/app/ImageDialogs.cpp
    src/app/AboutDialog.cpp
    src/app/ArchiveDialogs.cpp
    src/app/UpdateDialogs.cpp
    src/app/UpdateFlow.cpp
    src/app/SettingsDialog.cpp
    src/app/AppSettings.cpp
    src/app/UiText.cpp
    src/app/app.rc
)
target_include_directories(mpainter PRIVATE "${CMAKE_BINARY_DIR}/generated")
target_link_libraries(mpainter PRIVATE mp_core mp_render d2d1 dwrite comdlg32 comctl32 shell32 gdi32 user32 uxtheme)
# manifest 由 app.rc 提供，關掉連結器自動產生的那份，否則會重複。
target_link_options(mpainter PRIVATE /MANIFEST:NO)
mp_copy_7z(mpainter)
endif()

add_executable(mp_tests
    src/tests/test_util.cpp
    src/tests/test_geometry.cpp
    src/tests/test_annotation.cpp
    src/tests/test_document.cpp
    src/tests/test_editor.cpp
    src/tests/test_resize_model.cpp
    src/tests/test_image_ops.cpp
    src/tests/test_image_io.cpp
    src/tests/test_image_format.cpp
    src/tests/test_metadata.cpp
    src/tests/test_text_path.cpp
    # app 層裡不碰視窗的部分（代碼與翻譯 key 變成顯示用的句子）也編進來測。
    src/tests/test_ui_text.cpp
    src/app/UiText.cpp
    # 資訊面板：畫在記憶體裡的 render target 上，測版面、捲動、折疊。
    src/tests/test_info_panel.cpp
    src/app/InfoPanel.cpp
    # 工具列圖示的資料表與照表畫的結果。
    src/tests/test_toolbar_icons.cpp
    src/app/ToolbarIcons.cpp
    src/tests/test_toolbar.cpp
    src/app/Toolbar.cpp
    # 一個視窗的設定：讀檔、存檔時和別的視窗合併、可選的語系。
    src/tests/test_app_settings.cpp
    src/app/AppSettings.cpp
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
    src/tests/test_view_fit.cpp
    src/tests/test_prefetch.cpp
    src/tests/test_commands.cpp
    src/tests/test_viewport.cpp
    src/tests/test_view_state.cpp
    src/tests/test_paging.cpp
    # Renderer 的點陣圖快取（畫在記憶體裡的 render target）。src/app 仍然不測。
    src/tests/test_renderer.cpp
    # 存檔寫出去的像素與每種標註的繪製。
    src/tests/test_render_pixels.cpp
    # 記憶體量測（隱藏標籤，平常不跑）。
    src/tests/test_mem.cpp
    # 時間量測（隱藏標籤，平常不跑）。
    src/tests/test_time.cpp
    # 讀圖 plugin（用下面的 mp_test_plugin.dll 與 mp_plugin_crash_child.exe）。
    src/tests/test_plugins.cpp
    # JPEG XL plugin：測試裡用 libjxl 的編碼器現造 .jxl。
    src/tests/test_plugin_jxl.cpp
    # DNG plugin：解碼的部分直接連進來測（例如換執行緒數），也經過 loadImage 走真的 plugin。
    src/tests/test_plugin_dng.cpp
    src/plugins/dng/DngRender.cpp
)
target_include_directories(mp_tests SYSTEM PRIVATE "${MP_JXL_INCLUDE}")
target_compile_definitions(mp_tests PRIVATE ${MP_JXL_DEFS})
target_link_libraries(mp_tests PRIVATE ${MP_JXL_LIBS} mp_dng_sdk)
add_dependencies(mp_tests mp_plugin_jxl mp_plugin_dng)
# bit7z：測試自己用它現場產 zip / 7z 當 fixture。
target_link_libraries(mp_tests PRIVATE mp_core mp_render bit7z::bit7z Catch2::Catch2WithMain ws2_32 d2d1 dwrite shell32)
# 測試用的二進位 fixture（rar 沒有開源 writer，只能放現成檔）。建置後複製到測試程式旁的 fixtures\，
# 測試先找那裡；找不到（例如只搬了 exe）才用原始碼樹的路徑。
target_compile_definitions(mp_tests PRIVATE "MP_TEST_FIXTURES=L\"${CMAKE_SOURCE_DIR}/src/tests/fixtures\"")
# 隨程式發佈的語系檔：測試檢查每一行的佔位符。
target_compile_definitions(mp_tests PRIVATE "MP_SOURCE_LANG_DIR=L\"${CMAKE_SOURCE_DIR}/src/lang\"")
file(GLOB MP_FIXTURE_FILES CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/src/tests/fixtures/*.rar" "${CMAKE_SOURCE_DIR}/src/tests/fixtures/*.dng")
add_custom_command(TARGET mp_tests POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E make_directory "$<TARGET_FILE_DIR:mp_tests>/fixtures"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different ${MP_FIXTURE_FILES} "$<TARGET_FILE_DIR:mp_tests>/fixtures")
mp_copy_7z(mp_tests)

# 測試用的讀圖 plugin，與「plugin 害程式當掉」測試用的子行程。輸出在 mp_tests 旁邊，只給測試用、不發佈。
add_library(mp_test_plugin SHARED src/tests/plugin/test_plugin.cpp src/tests/plugin/test_plugin.rc)
target_include_directories(mp_test_plugin PRIVATE src)
add_executable(mp_plugin_crash_child src/tests/plugin/crash_child.cpp)
target_link_libraries(mp_plugin_crash_child PRIVATE mp_core)
add_dependencies(mp_tests mp_test_plugin mp_plugin_crash_child)

# ctest --preset release 跑這個。隨機順序：測試之間有順序相依時會露出來（Catch2 會印出 seed，可以重現）。
enable_testing()
add_test(NAME mp_tests COMMAND mp_tests --order rand)
