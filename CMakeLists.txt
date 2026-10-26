cmake_minimum_required(VERSION 3.22)
project(HarmonyCast VERSION 2.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

if(APPLE)
    list(APPEND CMAKE_PREFIX_PATH "/opt/homebrew/opt/qt" "/opt/homebrew/opt/ffmpeg")
endif()

# Find Qt 6
find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets Network Svg SvgWidgets WebSockets)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

# Find FFmpeg
find_path(FFMPEG_INCLUDE_DIR libavcodec/avcodec.h
    PATHS /opt/homebrew/opt/ffmpeg/include /usr/local/include /usr/include)
find_library(AVCODEC_LIBRARY avcodec
    PATHS /opt/homebrew/opt/ffmpeg/lib /usr/local/lib /usr/lib)
find_library(AVUTIL_LIBRARY avutil
    PATHS /opt/homebrew/opt/ffmpeg/lib /usr/local/lib /usr/lib)
find_library(SWSCALE_LIBRARY swscale
    PATHS /opt/homebrew/opt/ffmpeg/lib /usr/local/lib /usr/lib)

if(NOT FFMPEG_INCLUDE_DIR OR NOT AVCODEC_LIBRARY)
    message(FATAL_ERROR "FFmpeg (libavcodec, libavutil, libswscale) not found!")
endif()

message(STATUS "Found Qt6: ${Qt6_VERSION}")
message(STATUS "Found FFmpeg include: ${FFMPEG_INCLUDE_DIR}")
message(STATUS "Found libavcodec: ${AVCODEC_LIBRARY}")

# Core Library Sources
set(CORE_HEADERS
    include/harmonycast/Types.h
    include/harmonycast/HdcClient.h
    include/harmonycast/DeviceManager.h
    include/harmonycast/PacketDemuxer.h
    include/harmonycast/VideoDecoder.h
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
    include/harmonycast/MainWindow.h
)

set(CORE_SOURCES
    src/HdcClient.cpp
    src/DeviceManager.cpp
    src/PacketDemuxer.cpp
    src/VideoDecoder.cpp
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
        Qt6::Core
        Qt6::Gui
        Qt6::Widgets
        Qt6::Network
        Qt6::Svg
        Qt6::SvgWidgets
        Qt6::WebSockets
    PRIVATE
        ${AVCODEC_LIBRARY}
        ${AVUTIL_LIBRARY}
        ${SWSCALE_LIBRARY}
)

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
    add_executable(HarmonyCast WIN32 ${APP_SOURCES})
endif()

target_include_directories(HarmonyCast PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(HarmonyCast PRIVATE harmonycast_core)

# Test Suite
enable_testing()
add_executable(test_harmonycast_core tests/test_core.cpp)
target_link_libraries(test_harmonycast_core PRIVATE harmonycast_core)
add_test(NAME CoreTests COMMAND test_harmonycast_core)

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
