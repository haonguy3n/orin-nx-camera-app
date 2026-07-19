cmake_minimum_required(VERSION 3.16)
project(camera-viewer LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

find_package(Qt6 REQUIRED COMPONENTS Widgets Multimedia MultimediaWidgets Network)
find_package(PkgConfig REQUIRED)
find_package(OpenSSL REQUIRED)
pkg_check_modules(LIBUSB REQUIRED IMPORTED_TARGET libusb-1.0)
# The secure USB transport decodes the tunnelled H.265 in-process (appsrc ->
# avdec_h265 -> appsink) and pushes frames into the panes directly.
pkg_check_modules(GSTRTSP REQUIRED IMPORTED_TARGET
    gstreamer-1.0 gstreamer-app-1.0)

qt_add_executable(camera-viewer
    main.cpp
    mainwindow.cpp
    mainwindow.h
    cameracontrols.cpp
    cameracontrols.h
    collapsiblesection.cpp
    collapsiblesection.h
    controlclient.cpp
    controlclient.h
    controlpanel.cpp
    controlpanel.h
    discoveryclient.cpp
    discoveryclient.h
    theme.cpp
    theme.h
    updateclient.cpp
    updateclient.h
    updatewidget.cpp
    updatewidget.h
    frameview.cpp
    frameview.h
    videopane.cpp
    videopane.h
    whitebalancecalibrator.cpp
    whitebalancecalibrator.h
    secureusbbridge.cpp
    secureusbbridge.h
)

# Shared host<->device protocol constants (single source of truth).
target_include_directories(camera-viewer PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../common)

target_link_libraries(camera-viewer PRIVATE
    Qt6::Widgets
    Qt6::Multimedia
    Qt6::MultimediaWidgets
    Qt6::Network
    PkgConfig::LIBUSB
    PkgConfig::GSTRTSP
    OpenSSL::Crypto
)
include(${CMAKE_CURRENT_SOURCE_DIR}/../cmake/GitVersion.cmake)
derive_git_version("0.4.0" CAMERA_VIEWER_VERSION)
message(STATUS "camera-viewer version: ${CAMERA_VIEWER_VERSION}")
target_compile_definitions(camera-viewer PRIVATE
    CAMERA_VIEWER_VERSION="${CAMERA_VIEWER_VERSION}")

target_include_directories(camera-viewer PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../embedded/src)
target_sources(camera-viewer PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../common/secure/SecureHandshake.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../common/secure/SecureWire.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../embedded/src/camera/secure/KeySchedule.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../embedded/src/camera/secure/SecureRecord.cpp)
