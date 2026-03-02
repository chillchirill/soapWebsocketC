cmake_minimum_required(VERSION 3.15.3)
project(soapWebsocketC C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

add_executable(sender
    src/sender.c
    #src/reciever.c
)

# vcpkg will provide pkgconf + .pc files for gstreamer/libsoup/json-glib
find_package(PkgConfig REQUIRED)

# ---- GStreamer + modules ----
pkg_search_module(GST REQUIRED IMPORTED_TARGET gstreamer-1.0>=1.18)
pkg_search_module(GST_SDP REQUIRED IMPORTED_TARGET gstreamer-sdp-1.0>=1.18)
pkg_search_module(GST_APP REQUIRED IMPORTED_TARGET gstreamer-app-1.0>=1.18)
pkg_search_module(GST_VIDEO REQUIRED IMPORTED_TARGET gstreamer-video-1.0>=1.18)
pkg_search_module(GST_WEBRTC REQUIRED IMPORTED_TARGET gstreamer-webrtc-1.0>=1.18)

# ---- libsoup + json-glib ----
# Depending on how vcpkg resolves, the pkg-config name can be libsoup-3.0 or libsoup-2.4
# We'll try 3.0 first, then 2.4.
pkg_search_module(SOUP3 QUIET IMPORTED_TARGET libsoup-3.0)
if (SOUP3_FOUND)
    set(SOUP_TARGET PkgConfig::SOUP3)
else()
    pkg_search_module(SOUP2 REQUIRED IMPORTED_TARGET libsoup-2.4)
    set(SOUP_TARGET PkgConfig::SOUP2)
endif()

pkg_search_module(JSON_GLIB REQUIRED IMPORTED_TARGET json-glib-1.0)

# Helpful core deps (often required by these libs)
pkg_search_module(GLIB REQUIRED IMPORTED_TARGET glib-2.0)
pkg_search_module(GOBJECT REQUIRED IMPORTED_TARGET gobject-2.0)

target_link_libraries(sender PRIVATE
    PkgConfig::GST
    PkgConfig::GST_SDP
    PkgConfig::GST_APP
    PkgConfig::GST_VIDEO
    PkgConfig::GST_WEBRTC
    ${SOUP_TARGET}
    PkgConfig::JSON_GLIB
    PkgConfig::GLIB
    PkgConfig::GOBJECT
)

# Optional: warnings
if (MSVC)
    target_compile_options(sender PRIVATE /W3)
else()
    target_compile_options(sender PRIVATE -Wall -Wextra -Wpedantic)
endif()