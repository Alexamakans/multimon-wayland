cmake_minimum_required(VERSION 3.16)
project(viture_ar_desktop_wayland_dmabuf VERSION 1.0 LANGUAGES C CXX)

add_compile_definitions(EGL_EGLEXT_PROTOTYPES GL_GLEXT_PROTOTYPES)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Public headers
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/include)

# --- Core deps ---
find_package(OpenGL REQUIRED)   # OpenGL::GL
find_package(ZLIB REQUIRED)
find_package(PkgConfig REQUIRED)

pkg_check_modules(GLFW3   REQUIRED glfw3)
pkg_check_modules(WAYLAND REQUIRED wayland-client)
pkg_check_modules(WP      REQUIRED wayland-protocols)
pkg_check_modules(DRM     REQUIRED libdrm)
pkg_check_modules(GBM     REQUIRED gbm)
pkg_check_modules(EGL     REQUIRED egl)
pkg_check_modules(LIBSYSTEMD     REQUIRED libsystemd)
if (LIBSYSTEMD_FOUND)
  add_compile_definitions(HAVE_LIBSYSTEMD=1)
  include_directories(${LIBSYSTEMD_INCLUDE_DIRS})
endif()

find_program(WAYLAND_SCANNER wayland-scanner REQUIRED)

include_directories(
  ${GLFW3_INCLUDE_DIRS}
  ${WAYLAND_INCLUDE_DIRS}
  ${DRM_INCLUDE_DIRS}
  ${GBM_INCLUDE_DIRS}
  ${EGL_INCLUDE_DIRS}
)

# ---- Locate protocol roots ----
# 1) linux-dmabuf is in wayland-protocols
execute_process(
  COMMAND ${PKG_CONFIG_EXECUTABLE} --variable=pkgdatadir wayland-protocols
  OUTPUT_VARIABLE WAYLAND_PROTOCOLS_DIR
  OUTPUT_STRIP_TRAILING_WHITESPACE
)
if(NOT EXISTS "${WAYLAND_PROTOCOLS_DIR}")
  message(FATAL_ERROR "Could not locate wayland-protocols pkgdatadir")
endif()

# 2) wlr-screencopy is in wlroots' wlr-protocols (no .pc on Nix).
#    Provide an override from the command line (recommended on Nix):
#      -DWLR_PROTOCOLS_DIR=/nix/store/...-wlroots-*/share/wlr-protocols
set(WLR_PROTOCOLS_DIR "${WLR_PROTOCOLS_DIR}" CACHE PATH "Path to wlr-protocols (share/wlr-protocols)")

if(NOT WLR_PROTOCOLS_DIR)
  # Try environment hint
  if(DEFINED ENV{WLR_PROTOCOLS_DIR} AND EXISTS "$ENV{WLR_PROTOCOLS_DIR}")
    set(WLR_PROTOCOLS_DIR "$ENV{WLR_PROTOCOLS_DIR}")
  endif()
endif()

if(NOT WLR_PROTOCOLS_DIR)
  # Last-ditch common prefixes (works on some non-Nix distros)
  find_path(
    WLR_PROTOCOLS_DIR
    NAMES unstable/wlr-screencopy/wlr-screencopy-unstable-v1.xml
    PATHS
      /usr/share/wlr-protocols
      /usr/local/share/wlr-protocols
      ${CMAKE_INSTALL_PREFIX}/share/wlr-protocols
    NO_DEFAULT_PATH
  )
endif()

if(NOT WLR_PROTOCOLS_DIR)
  message(FATAL_ERROR
    "Could not locate wlroots wlr-protocols.\n"
    "Pass -DWLR_PROTOCOLS_DIR=/path/to/share/wlr-protocols (on Nix: ${wlroots}/share/wlr-protocols).")
endif()

# ---- Concrete XML files (from system) ----
# linux-dmabuf (wayland-protocols) is stable across versions:
set(SYS_LINUX_DMABUF_XML
    "${WAYLAND_PROTOCOLS_DIR}/unstable/linux-dmabuf/linux-dmabuf-unstable-v1.xml")

# wlr-screencopy (wlr-protocols) has two common layouts:
#   NEW: ${WLR_PROTOCOLS_DIR}/unstable/wlr-screencopy/wlr-screencopy-unstable-v1.xml
#   OLD: ${WLR_PROTOCOLS_DIR}/unstable/wlr-screencopy-unstable-v1.xml
set(_WLR_XML_NEW "${WLR_PROTOCOLS_DIR}/unstable/wlr-screencopy/wlr-screencopy-unstable-v1.xml")
set(_WLR_XML_OLD "${WLR_PROTOCOLS_DIR}/unstable/wlr-screencopy-unstable-v1.xml")

if (EXISTS "${_WLR_XML_NEW}")
  set(SYS_WLR_SCREENCOPY_XML "${_WLR_XML_NEW}")
elseif (EXISTS "${_WLR_XML_OLD}")
  set(SYS_WLR_SCREENCOPY_XML "${_WLR_XML_OLD}")
else()
  message(FATAL_ERROR
    "Wayland protocol XML not found in either layout:\n"
    "  ${_WLR_XML_NEW}\n"
    "  ${_WLR_XML_OLD}\n"
    "WLR_PROTOCOLS_DIR='${WLR_PROTOCOLS_DIR}'"
  )
endif()

# sanity-check linux-dmabuf as well
if(NOT EXISTS "${SYS_LINUX_DMABUF_XML}")
  message(FATAL_ERROR "Wayland protocol XML not found: ${SYS_LINUX_DMABUF_XML}")
endif()

# ---- Work dir & local copies ----
set(GEN_DIR ${CMAKE_CURRENT_BINARY_DIR}/protocols-generated)
file(MAKE_DIRECTORY ${GEN_DIR})

set(LOCAL_DMABUF_XML "${GEN_DIR}/linux-dmabuf.xml")
set(LOCAL_WLR_XML    "${GEN_DIR}/wlr-screencopy.xml")
configure_file("${SYS_LINUX_DMABUF_XML}"   "${LOCAL_DMABUF_XML}" COPYONLY)
configure_file("${SYS_WLR_SCREENCOPY_XML}" "${LOCAL_WLR_XML}"    COPYONLY)

# ---- Generate client headers ----
add_custom_command(
  OUTPUT ${GEN_DIR}/linux-dmabuf-unstable-v1-client-protocol.h
  COMMAND ${WAYLAND_SCANNER} client-header ${LOCAL_DMABUF_XML} ${GEN_DIR}/linux-dmabuf-unstable-v1-client-protocol.h
  DEPENDS ${LOCAL_DMABUF_XML}
  VERBATIM
)
add_custom_command(
  OUTPUT ${GEN_DIR}/wlr-screencopy-unstable-v1-client-protocol.h
  COMMAND ${WAYLAND_SCANNER} client-header ${LOCAL_WLR_XML} ${GEN_DIR}/wlr-screencopy-unstable-v1-client-protocol.h
  DEPENDS ${LOCAL_WLR_XML}
  VERBATIM
)

# ---- Generate private-code C stubs ----
add_custom_command(
  OUTPUT ${GEN_DIR}/linux-dmabuf-unstable-v1-protocol.c
  COMMAND ${WAYLAND_SCANNER} private-code ${LOCAL_DMABUF_XML} ${GEN_DIR}/linux-dmabuf-unstable-v1-protocol.c
  DEPENDS ${LOCAL_DMABUF_XML}
  VERBATIM
)
add_custom_command(
  OUTPUT ${GEN_DIR}/wlr-screencopy-unstable-v1-protocol.c
  COMMAND ${WAYLAND_SCANNER} private-code ${LOCAL_WLR_XML} ${GEN_DIR}/wlr-screencopy-unstable-v1-protocol.c
  DEPENDS ${LOCAL_WLR_XML}
  VERBATIM
)

add_custom_target(protocol_headers ALL
  DEPENDS
    ${GEN_DIR}/linux-dmabuf-unstable-v1-client-protocol.h
    ${GEN_DIR}/wlr-screencopy-unstable-v1-client-protocol.h
    ${GEN_DIR}/linux-dmabuf-unstable-v1-protocol.c
    ${GEN_DIR}/wlr-screencopy-unstable-v1-protocol.c
)

include_directories(${GEN_DIR})

# ---- VITURE SDK .so (prebuilt) ----
add_library(viture_sdk SHARED IMPORTED)
set_target_properties(viture_sdk PROPERTIES
  IMPORTED_LOCATION ${CMAKE_CURRENT_SOURCE_DIR}/libs/libviture_one_sdk.so
)

# ---- Sources ----
file(GLOB SRC
  src/*.cpp
  src/*.c
)
list(APPEND SRC
  ${GEN_DIR}/linux-dmabuf-unstable-v1-protocol.c
  ${GEN_DIR}/wlr-screencopy-unstable-v1-protocol.c
)

add_executable(${PROJECT_NAME} ${SRC})
add_dependencies(${PROJECT_NAME} protocol_headers)

# ---- Linking ----
target_link_libraries(${PROJECT_NAME}
  viture_sdk
  OpenGL::GL
  GLU                    # remove if you no longer use GLU
  ZLIB::ZLIB
  ${EGL_LIBRARIES}
  ${GLFW3_LIBRARIES}
  ${WAYLAND_LIBRARIES}
  ${DRM_LIBRARIES}
  ${GBM_LIBRARIES}
  ${LIBSYSTEMD_LIBRARIES}
  m
  rt
)

set_target_properties(${PROJECT_NAME} PROPERTIES
  BUILD_RPATH "${CMAKE_INSTALL_PREFIX}/lib"
)

install(TARGETS ${PROJECT_NAME} RUNTIME DESTINATION bin)

