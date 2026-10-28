cmake_minimum_required(VERSION 3.22)
project(HarmonyCast VERSION 2.2.2 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

if(WIN32)
    add_compile_definitions(NOMINMAX WIN32_LEAN_AND_MEAN)
endif()

if(APPLE)
    list(APPEND CMAKE_PREFIX_PATH "/opt/homebrew/opt/qt" "/opt/homebrew/opt/ffmpeg")
endif()

# The Server 2012 build uses Qt 5: Qt 6 requires Windows 10 or newer, and
# Qt 6.6's QtGui eagerly imports d3d12.dll even for raster QWidget apps.
option(HARMONYCAST_SERVER2012_COMPAT "Build the Windows Server 2012/2012 R2 compatibility edition" OFF)
set(HARMONYCAST_QT_MAJOR_VERSION "6" CACHE STRING "Qt major version (5 or 6)")
set_property(CACHE HARMONYCAST_QT_MAJOR_VERSION PROPERTY STRINGS 5 6)
if(HARMONYCAST_SERVER2012_COMPAT)
    if(NOT WIN32)
        message(FATAL_ERROR "HARMONYCAST_SERVER2012_COMPAT is a Windows-only edition")
    endif()
    set(HARMONYCAST_QT_MAJOR_VERSION "5")
endif()
if(HARMONYCAST_QT_MAJOR_VERSION STREQUAL "5")
    find_package(Qt5 5.15 REQUIRED COMPONENTS Core Gui Widgets Network Svg WebSockets)
elseif(HARMONYCAST_QT_MAJOR_VERSION STREQUAL "6")
    find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets Network Svg SvgWidgets WebSockets)
else()
    message(FATAL_ERROR "HARMONYCAST_QT_MAJOR_VERSION must be 5 or 6")
endif()
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

# Find FFmpeg
find_path(FFMPEG_INCLUDE_DIR libavcodec/avcodec.h
    PATHS /opt/homebrew/opt/ffmpeg/include /usr/local/include /usr/include
          $ENV{FFMPEG_DIR}/include
          ${FFMPEG_DIR}/include)
find_library(AVCODEC_LIBRARY NAMES avcodec avcodec.lib
    PATHS /opt/homebrew/opt/ffmpeg/lib /usr/local/lib /usr/lib
          $ENV{FFMPEG_DIR}/lib
          ${FFMPEG_DIR}/lib)
find_library(AVUTIL_LIBRARY NAMES avutil avutil.lib
    PATHS /opt/homebrew/opt/ffmpeg/lib /usr/local/lib /usr/lib
          $ENV{FFMPEG_DIR}/lib
          ${FFMPEG_DIR}/lib)
find_library(SWSCALE_LIBRARY NAMES swscale swscale.lib
    PATHS /opt/homebrew/opt/ffmpeg/lib /usr/local/lib /usr/lib
          $ENV{FFMPEG_DIR}/lib
          ${FFMPEG_DIR}/lib)

if(NOT FFMPEG_INCLUDE_DIR OR NOT AVCODEC_LIBRARY)
    message(FATAL_ERROR "FFmpeg (libavcodec, libavutil, libswscale) not found!")
endif()

message(STATUS "Using Qt ${HARMONYCAST_QT_MAJOR_VERSION}")
message(STATUS "Found FFmpeg include: ${FFMPEG_INCLUDE_DIR}")
message(STATUS "Found libavcodec: ${AVCODEC_LIBRARY}")

# Core Library Sources
set(CORE_HEADERS
    include/harmonycast/Types.h
    include/harmonycast/HdcClient.h
    include/harmonycast/DeviceManager.h
    include/harmonycast/PacketDemuxer.h
    include/harmonycast/VideoDecoder.h
    include/harmonycast/VideoPipeline.h
    include/harmonycast/FrameMailbox.h
    include/harmonycast/FramePresentation.h
    include/harmonycast/ControlChannel.h
    include/harmonycast/HiLogParser.h
    include/harmonycast/HiLogSession.h
    include/harmonycast/StreamSession.h
    include/harmonycast/FlowLayout.h
    include/harmonycast/IconManager.h
    include/harmonycast/ScreenView.h
    include/harmonycast/WebViewDebugger.h
    include/harmonycast/NetworkDeviceDialog.h
    include/harmonycast/LogHighlighter.h
    include/harmonycast/Logger.h
    include/harmonycast/CrashHandler.h
    include/harmonycast/UpdateChecker.h
    include/harmonycast/UpdateDialog.h
    include/harmonycast/MainWindow.h
)

set(CORE_SOURCES
    src/HdcClient.cpp
    src/DeviceManager.cpp
    src/PacketDemuxer.cpp
    src/VideoDecoder.cpp
    src/VideoPipeline.cpp
    src/ControlChannel.cpp
    src/HiLogParser.cpp
    src/HiLogSession.cpp
    src/StreamSession.cpp
    src/FlowLayout.cpp
    src/IconManager.cpp
    src/ScreenView.cpp
    src/WebViewDebugger.cpp
    src/NetworkDeviceDialog.cpp
    src/LogHighlighter.cpp
    src/Logger.cpp
    src/CrashHandler.cpp
    src/UpdateChecker.cpp
    src/UpdateDialog.cpp
    src/MainWindow.cpp
)

add_library(harmonycast_core STATIC ${CORE_SOURCES} ${CORE_HEADERS})

target_include_directories(harmonycast_core
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
    PRIVATE
        ${FFMPEG_INCLUDE_DIR}
)

target_link_libraries(harmonycast_core
    PUBLIC
        Qt${HARMONYCAST_QT_MAJOR_VERSION}::Core
        Qt${HARMONYCAST_QT_MAJOR_VERSION}::Gui
        Qt${HARMONYCAST_QT_MAJOR_VERSION}::Widgets
        Qt${HARMONYCAST_QT_MAJOR_VERSION}::Network
        Qt${HARMONYCAST_QT_MAJOR_VERSION}::Svg
        Qt${HARMONYCAST_QT_MAJOR_VERSION}::WebSockets
    PRIVATE
        ${AVCODEC_LIBRARY}
        ${AVUTIL_LIBRARY}
        ${SWSCALE_LIBRARY}
)
if(HARMONYCAST_QT_MAJOR_VERSION STREQUAL "6")
    target_link_libraries(harmonycast_core PUBLIC Qt6::SvgWidgets)
endif()

if(HARMONYCAST_SERVER2012_COMPAT)
    target_compile_definitions(harmonycast_core PUBLIC
        HARMONYCAST_SERVER2012_COMPAT=1 WINVER=0x0602 _WIN32_WINNT=0x0602 NOMINMAX)
endif()

if(APPLE)
    target_link_libraries(harmonycast_core PRIVATE "-framework VideoToolbox" "-framework CoreMedia" "-framework CoreVideo")
endif()

# Application Executable / Bundle
set(APP_ICON_MACOS "${CMAKE_CURRENT_SOURCE_DIR}/resources/app-icon.icns")
set(APP_SOURCES src/main.cpp)

if(APPLE)
    set(MACOSX_BUNDLE_ICON_FILE "app-icon.icns")
    set(MACOSX_BUNDLE_GUI_IDENTIFIER "local.harmonycast.desktop")
    set(MACOSX_BUNDLE_BUNDLE_NAME "HarmonyCast")
    set(MACOSX_BUNDLE_DISPLAY_NAME "鸿蒙投屏")
    set(MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION}")
    set(MACOSX_BUNDLE_BUNDLE_VERSION "${PROJECT_VERSION}")

    file(GLOB APP_RESOURCES "${CMAKE_CURRENT_SOURCE_DIR}/resources/*")

    add_executable(HarmonyCast MACOSX_BUNDLE
        ${APP_SOURCES}
        ${APP_RESOURCES}
    )
    set_source_files_properties(${APP_RESOURCES} PROPERTIES MACOSX_PACKAGE_LOCATION "Resources")
    set_target_properties(HarmonyCast PROPERTIES
        BUILD_WITH_INSTALL_RPATH TRUE
        INSTALL_RPATH "@executable_path/../Frameworks;@loader_path/../Frameworks;/opt/homebrew/opt/qt/lib;/opt/homebrew/opt/ffmpeg/lib;/opt/homebrew/lib"
    )
    add_custom_command(TARGET HarmonyCast POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_CURRENT_SOURCE_DIR}/resources" "$<TARGET_BUNDLE_DIR:HarmonyCast>/Contents/Resources"
        COMMAND codesign --force --deep --sign - "$<TARGET_BUNDLE_DIR:HarmonyCast>"
        COMMENT "Copying resources and applying ad-hoc code signature to HarmonyCast.app"
    )
else()
    if(WIN32)
        list(APPEND APP_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/resources/windows/HarmonyCast.rc")
        add_library(d3d12_stub SHARED
            src/windows/d3d12_stub.cpp
            src/windows/d3d12.def
        )
        set_target_properties(d3d12_stub PROPERTIES
            OUTPUT_NAME "d3d12"
            PREFIX ""
        )
        if(MSVC)
            target_link_options(d3d12_stub PRIVATE /SUBSYSTEM:WINDOWS,6.02)
        endif()
    endif()
    add_executable(HarmonyCast WIN32 ${APP_SOURCES})
    if(WIN32)
        add_dependencies(HarmonyCast d3d12_stub)
        add_custom_command(TARGET HarmonyCast POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                $<TARGET_FILE:d3d12_stub>
                $<TARGET_FILE_DIR:HarmonyCast>/d3d12.dll
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${CMAKE_CURRENT_SOURCE_DIR}/scripts/windows/HarmonyCast-SoftwareRendering.cmd"
                "$<TARGET_FILE_DIR:HarmonyCast>/HarmonyCast-SoftwareRendering.cmd"
            COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_CURRENT_SOURCE_DIR}/resources" "$<TARGET_FILE_DIR:HarmonyCast>/resources"
            COMMENT "Copying resources, software rendering script, and d3d12 stub to target directory"
        )
    else()
        add_custom_command(TARGET HarmonyCast POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_CURRENT_SOURCE_DIR}/resources" "$<TARGET_FILE_DIR:HarmonyCast>/resources"
            COMMENT "Copying resources to target directory"
        )
    endif()
endif()

target_include_directories(HarmonyCast PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(HarmonyCast PRIVATE harmonycast_core)
if(HARMONYCAST_SERVER2012_COMPAT AND MSVC)
    target_link_options(HarmonyCast PRIVATE /SUBSYSTEM:WINDOWS,6.02)
endif()

# Test Suite
enable_testing()
add_executable(test_harmonycast_core tests/test_core.cpp)
target_link_libraries(test_harmonycast_core PRIVATE harmonycast_core)
target_compile_options(test_harmonycast_core PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/UNDEBUG> $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-UNDEBUG>)
add_test(NAME CoreTests COMMAND test_harmonycast_core)

add_executable(test_harmonycast_update_assets tests/test_update_assets.cpp)
target_link_libraries(test_harmonycast_update_assets PRIVATE harmonycast_core)
add_test(NAME UpdateAssetTests COMMAND test_harmonycast_update_assets)

add_executable(test_harmonycast_performance tests/test_performance.cpp)
target_link_libraries(test_harmonycast_performance PRIVATE harmonycast_core)
target_compile_definitions(test_harmonycast_performance PRIVATE HARMONYCAST_TEST_FIXTURES="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures")
add_test(NAME PerformanceRegressionTests COMMAND test_harmonycast_performance)

add_executable(benchmark_harmonycast tests/benchmark_core.cpp)
target_link_libraries(benchmark_harmonycast PRIVATE harmonycast_core)

# CPack Multi-Platform Packaging
set(CPACK_PACKAGE_NAME "HarmonyCast")
set(CPACK_PACKAGE_VENDOR "HarmonyCast")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "High-Performance OpenHarmony Screen Mirroring & HiLog Workbench")
set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")

if(APPLE)
    set(CPACK_GENERATOR "DragNDrop")
    set(CPACK_DMG_VOLUME_NAME "HarmonyCast")
    set(CPACK_SYSTEM_NAME "macOS")
elseif(WIN32)
    set(CPACK_GENERATOR "NSIS;ZIP")
    set(CPACK_NSIS_DISPLAY_NAME "HarmonyCast")
    set(CPACK_SYSTEM_NAME "Windows-x64")
else()
    set(CPACK_GENERATOR "TGZ;DEB")
endif()

include(CPack)
