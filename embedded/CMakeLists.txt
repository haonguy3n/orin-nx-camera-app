cmake_minimum_required(VERSION 3.16)
project(camera-streamer VERSION 0.4.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(GNUInstallDirs)

find_package(PkgConfig REQUIRED)
pkg_check_modules(GST REQUIRED IMPORTED_TARGET
    gstreamer-1.0
    gstreamer-rtsp-1.0
    gstreamer-rtsp-server-1.0
    gio-2.0
    json-glib-1.0
)

add_executable(camera-streamer
    # core
    src/main.cpp
    src/core/application.cpp
    # config
    src/config/config_loader.cpp
    # net
    src/net/network_resolver.cpp
    # v4l2
    src/v4l2/v4l2_device.cpp
    # pipeline
    src/pipeline/pipeline_builder.cpp
    src/pipeline/argus_source.cpp
    src/pipeline/v4l2_source.cpp
    src/pipeline/test_source.cpp
    src/pipeline/source_factory.cpp
    # rtsp
    src/rtsp/mount_controller.cpp
    src/rtsp/rtsp_server.cpp
    # control
    src/control/json_util.cpp
    src/control/control_registry.cpp
    src/control/control_server.cpp
    src/control/handlers/register_handlers.cpp
    src/control/handlers/system_handlers.cpp
    src/control/handlers/status_handler.cpp
    src/control/handlers/exposure_handler.cpp
    src/control/handlers/trigger_handler.cpp
    src/control/handlers/isp_handler.cpp
    src/control/handlers/zoom_handler.cpp
    src/control/handlers/v4l2_control_handler.cpp
    src/control/handlers/update_handler.cpp
    # discovery
    src/discovery/discovery_server.cpp
    # update (OTA via swupdate)
    src/update/swupdate_client.cpp
    src/update/update_server.cpp
)
target_include_directories(camera-streamer PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_options(camera-streamer PRIVATE -Wall -Wextra)
target_compile_definitions(camera-streamer PRIVATE
    DEFAULT_CONF_PATH="${CMAKE_INSTALL_FULL_SYSCONFDIR}/camera-streamer.conf"
    APP_VERSION="${PROJECT_VERSION}"
)
target_link_libraries(camera-streamer PRIVATE PkgConfig::GST)

# --- install ---------------------------------------------------------------

install(TARGETS camera-streamer RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

install(FILES config/camera-streamer.conf
        DESTINATION ${CMAKE_INSTALL_SYSCONFDIR})

# systemd unit dir: -DSYSTEMD_SYSTEM_UNITDIR (Yocto passes ${systemd_system_unitdir})
# wins; else ask systemd.pc; else fall back to the conventional path.
if(NOT SYSTEMD_SYSTEM_UNITDIR)
    pkg_get_variable(SYSTEMD_SYSTEM_UNITDIR systemd systemdsystemunitdir)
endif()
if(NOT SYSTEMD_SYSTEM_UNITDIR)
    set(SYSTEMD_SYSTEM_UNITDIR "${CMAKE_INSTALL_PREFIX}/lib/systemd/system")
endif()
install(FILES systemd/camera-streamer.service
        DESTINATION ${SYSTEMD_SYSTEM_UNITDIR})
