cmake_minimum_required(VERSION 3.21)
project(soapWebsocketC C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

find_package(PkgConfig REQUIRED)

# GStreamer core + modules
pkg_check_modules(GST REQUIRED IMPORTED_TARGET
    gstreamer-1.0
    gstreamer-sdp-1.0
    gstreamer-rtp-1.0
    gstreamer-webrtc-1.0
)

# libsoup + json-glib
pkg_check_modules(SOUP REQUIRED IMPORTED_TARGET libsoup-3.0)
pkg_check_modules(JSONGLIB REQUIRED IMPORTED_TARGET json-glib-1.0)

# Receiver
add_executable(receiver
    src/reciever.c
)

target_link_libraries(receiver PRIVATE
    PkgConfig::GST
    PkgConfig::SOUP
    PkgConfig::JSONGLIB
)

# Sender
add_executable(sender
    src/sender.c
)

target_link_libraries(sender PRIVATE
    PkgConfig::GST
    PkgConfig::SOUP
    PkgConfig::JSONGLIB
)

# PATH for debugger (apply to both)
set(_DBG_PATH "PATH=C:/Program Files/gstreamer/1.0/msvc_x86_64/bin;C:/vcpkg/installed/x64-windows/bin;%PATH%")

set_property(TARGET receiver PROPERTY VS_DEBUGGER_ENVIRONMENT "${_DBG_PATH}")
set_property(TARGET sender   PROPERTY VS_DEBUGGER_ENVIRONMENT "${_DBG_PATH}")