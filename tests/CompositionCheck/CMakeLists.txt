# Headless check of the Windows hosting options (composition mode, hit regions, popups) and of the default mode.
# Build Photino.Native first (x64, the same configuration), with its NuGet packages restored into ../../packages.
#   cmake -S tests/CompositionCheck -B <build dir> -A x64
#   cmake --build <build dir> --config Release
#   <build dir>/Release/DesktopRun.exe <out file> <build dir>/Release/CompositionCheck.exe
#     (runs every mode on a Win32 desktop of its own, windows never shown; exit code 0 = pass, results in <out file>)
# Never run CompositionCheck.exe on the user's own desktop: it posts input to its windows.
cmake_minimum_required(VERSION 3.20)
project(CompositionCheck CXX)

set(CMAKE_CXX_STANDARD 17)
set(ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../..)
file(GLOB WEBVIEW2_DIR LIST_DIRECTORIES true ${ROOT}/packages/Microsoft.Web.WebView2.*/build/native/include)
file(GLOB WIL_DIR LIST_DIRECTORIES true ${ROOT}/packages/Microsoft.Windows.ImplementationLibrary.*/include)

add_executable(CompositionCheck check.cpp)
target_include_directories(CompositionCheck PRIVATE ${ROOT}/Photino.Native ${WEBVIEW2_DIR} ${WIL_DIR})
target_compile_definitions(CompositionCheck PRIVATE UNICODE _UNICODE NOMINMAX
    CHECK_WEB_DIR=L"${CMAKE_CURRENT_SOURCE_DIR}/web")
target_link_libraries(CompositionCheck PRIVATE
    ${ROOT}/Photino.Native/x64/$<CONFIG>/Photino.Native.lib d3d11 dxgi)
add_custom_command(TARGET CompositionCheck POST_BUILD COMMAND ${CMAKE_COMMAND} -E copy_if_different
    ${ROOT}/Photino.Native/x64/$<CONFIG>/Photino.Native.dll ${ROOT}/Photino.Native/x64/$<CONFIG>/WebView2Loader.dll
    $<TARGET_FILE_DIR:CompositionCheck>)

add_executable(DesktopRun desktop_run.cpp)
target_compile_definitions(DesktopRun PRIVATE UNICODE _UNICODE)
