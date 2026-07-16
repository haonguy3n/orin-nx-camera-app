cmake_minimum_required(VERSION 3.16)
project(camera-viewer LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

find_package(Qt6 REQUIRED COMPONENTS Widgets Multimedia MultimediaWidgets Network)

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
    videopane.cpp
    videopane.h
    whitebalancecalibrator.cpp
    whitebalancecalibrator.h
)

# Shared host<->device protocol constants (single source of truth).
target_include_directories(camera-viewer PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../common)

target_link_libraries(camera-viewer PRIVATE
    Qt6::Widgets
    Qt6::Multimedia
    Qt6::MultimediaWidgets
    Qt6::Network
)
