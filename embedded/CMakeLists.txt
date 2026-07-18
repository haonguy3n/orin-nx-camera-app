cmake_minimum_required(VERSION 3.16)
project(camera-streamer VERSION 0.4.0 LANGUAGES CXX)
option(ENABLE_SECURE_USB "Build the authenticated FunctionFS USB transport" ON)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(GNUInstallDirs)

find_package(PkgConfig REQUIRED)
if(ENABLE_SECURE_USB)
    find_package(OpenSSL REQUIRED)
endif()
pkg_check_modules(GST REQUIRED IMPORTED_TARGET
    gstreamer-1.0
    gstreamer-app-1.0
    gstreamer-rtsp-1.0
    gstreamer-rtsp-server-1.0
    gio-2.0
    json-glib-1.0
)

# Tree layout follows folly/fboss: src/camera/ is the project dir
# (fboss/fboss), src/camera/lib/ holds low-level device/system access
# (fboss/lib), src/camera/folly/ holds vendored folly mimics with tests in
# src/camera/folly/test/ (folly layout).
add_executable(camera-streamer
    # core
    src/camera/Main.cpp
    src/camera/core/Application.cpp
    src/camera/core/Watchdog.cpp
    # config
    src/camera/config/ConfigLoader.cpp
    # lib (low-level device/system access)
    src/camera/lib/net/NetworkResolver.cpp
    src/camera/lib/v4l2/V4l2Device.cpp
    # folly mimics with .cpp implementations (GLib-backed)
    src/camera/folly/io/async/AsyncServerSocket.cpp
    src/camera/folly/io/async/SSLContext.cpp
    # pipeline
    src/camera/pipeline/PipelineBuilder.cpp
    src/camera/pipeline/ArgusSource.cpp
    src/camera/pipeline/V4l2Source.cpp
    src/camera/pipeline/TestSource.cpp
    src/camera/pipeline/SourceFactory.cpp
    # rtsp
    src/camera/rtsp/MountController.cpp
    src/camera/rtsp/RtspServer.cpp
    # control
    src/camera/control/JsonUtil.cpp
    src/camera/control/ControlRegistry.cpp
    src/camera/control/ControlServer.cpp
    src/camera/control/handlers/RegisterHandlers.cpp
    src/camera/control/handlers/SystemHandlers.cpp
    src/camera/control/handlers/StatusHandler.cpp
    src/camera/control/handlers/ExposureHandler.cpp
    src/camera/control/handlers/TriggerHandler.cpp
    src/camera/control/handlers/IspHandler.cpp
    src/camera/control/handlers/ZoomHandler.cpp
    src/camera/control/handlers/V4l2ControlHandler.cpp
    src/camera/control/handlers/UpdateHandler.cpp
    # discovery
    src/camera/discovery/DiscoveryServer.cpp
    # update (OTA via swupdate)
    src/camera/update/SwupdateClient.cpp
    src/camera/update/UpdateServer.cpp
)
# ../common: protocol constants shared with host-ui (single source of truth).
target_include_directories(camera-streamer PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/../common)
target_compile_options(camera-streamer PRIVATE -Wall -Wextra)
target_compile_definitions(camera-streamer PRIVATE
    DEFAULT_CONF_PATH="${CMAKE_INSTALL_FULL_SYSCONFDIR}/camera-streamer.conf"
    APP_VERSION="${PROJECT_VERSION}"
)
target_link_libraries(camera-streamer PRIVATE PkgConfig::GST)

if(ENABLE_SECURE_USB)
    target_sources(camera-streamer PRIVATE
        src/camera/secure/SecureRecord.cpp
        src/camera/secure/KeySchedule.cpp
        src/camera/secure/SecureUsbServer.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../common/secure/SecureHandshake.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../common/secure/SecureWire.cpp)
    target_link_libraries(camera-streamer PRIVATE OpenSSL::Crypto)
    target_compile_definitions(camera-streamer PRIVATE ENABLE_SECURE_USB=1)
endif()

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
              systemd/camera-streamer-gencert.service
        DESTINATION ${SYSTEMD_SYSTEM_UNITDIR})
