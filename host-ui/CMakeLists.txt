cmake_minimum_required(VERSION 3.16)
project(camera-viewer LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

find_package(Qt6 REQUIRED COMPONENTS Widgets Multimedia MultimediaWidgets)

qt_add_executable(camera-viewer
    main.cpp
    mainwindow.cpp
    mainwindow.h
)

target_link_libraries(camera-viewer PRIVATE
    Qt6::Widgets
    Qt6::Multimedia
    Qt6::MultimediaWidgets
)
