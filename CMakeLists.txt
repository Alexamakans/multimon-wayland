cmake_minimum_required(VERSION 3.16)
project(viture_ar_desktop_wayland_dmabuf VERSION 1.0)

add_compile_definitions(EGL_EGLEXT_PROTOTYPES GL_GLEXT_PROTOTYPES)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS 1)

# Your public headers live here
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/include)

find_package(OpenGL REQUIRED)
find_package(PkgConfig REQUIRED)
find_package(ZLIB REQUIRED)

# GLFW (Wayland-enabled build)
pkg_check_modules(GLFW3 REQUIRED glfw3)

# Wayland client, scanner, protocols
pkg_check_modules(WAYLAND REQUIRED wayland-client)
find_program(WAYLAND_SCANNER wayland-scanner REQUIRED)

# DRM/GBM for DMA-BUF allocation
pkg_check_modules(DRM REQUIRED libdrm)
pkg_check_modules(GBM REQUIRED gbm)

# EGL
pkg_check_modules(EGL REQUIRED egl)

# ---- Generate protocol headers + C stubs ----
set(PROTO_DIR ${CMAKE_CURRENT_SOURCE_DIR}/protocols)
set(GEN_DIR   ${CMAKE_CURRENT_BINARY_DIR}/protocols-generated)
file(MAKE_DIRECTORY ${GEN_DIR})

# Generate client-header
macro(WL_CLIENT_HEADER out xml)
  add_custom_command(
    OUTPUT ${GEN_DIR}/${out}
    COMMAND ${WAYLAND_SCANNER} client-header
            ${PROTO_DIR}/${xml}
            ${GEN_DIR}/${out}
    DEPENDS ${PROTO_DIR}/${xml}
    VERBATIM
  )
endmacro()

# Generate private-code (C file providing the interface objects)
macro(WL_PRIVATE_CODE outc xml)
  add_custom_command(
    OUTPUT ${GEN_DIR}/${outc}
    COMMAND ${WAYLAND_SCANNER} private-code
            ${PROTO_DIR}/${xml}
            ${GEN_DIR}/${outc}
    DEPENDS ${PROTO_DIR}/${xml}
    VERBATIM
  )
endmacro()

WL_CLIENT_HEADER(wlr-screencopy-unstable-v1-client-protocol.h wlr-screencopy-unstable-v1.xml)
WL_PRIVATE_CODE (wlr-screencopy-unstable-v1-protocol.c       wlr-screencopy-unstable-v1.xml)

WL_CLIENT_HEADER(linux-dmabuf-unstable-v1-client-protocol.h  linux-dmabuf-unstable-v1.xml)
WL_PRIVATE_CODE (linux-dmabuf-unstable-v1-protocol.c         linux-dmabuf-unstable-v1.xml)

WL_CLIENT_HEADER(xdg-output-unstable-v1-client-protocol.h     xdg-output-unstable-v1.xml)
WL_PRIVATE_CODE (xdg-output-unstable-v1-protocol.c            xdg-output-unstable-v1.xml)

add_custom_target(protocol_headers ALL
  DEPENDS
    ${GEN_DIR}/wlr-screencopy-unstable-v1-client-protocol.h
    ${GEN_DIR}/linux-dmabuf-unstable-v1-client-protocol.h
    ${GEN_DIR}/xdg-output-unstable-v1-client-protocol.h
    ${GEN_DIR}/wlr-screencopy-unstable-v1-protocol.c
    ${GEN_DIR}/linux-dmabuf-unstable-v1-protocol.c
    ${GEN_DIR}/xdg-output-unstable-v1-protocol.c
)

include_directories(${GEN_DIR})

include_directories(${GLFW3_INCLUDE_DIRS})
include_directories(${WAYLAND_INCLUDE_DIRS})
include_directories(${DRM_INCLUDE_DIRS})
include_directories(${GBM_INCLUDE_DIRS})
include_directories(${EGL_INCLUDE_DIRS})

# ---- VITURE SDK .so (prebuilt, same as your original layout) ----
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
  ${GEN_DIR}/wlr-screencopy-unstable-v1-protocol.c
  ${GEN_DIR}/linux-dmabuf-unstable-v1-protocol.c
  ${GEN_DIR}/xdg-output-unstable-v1-protocol.c
)

add_executable(${PROJECT_NAME} ${SRC})
add_dependencies(${PROJECT_NAME} protocol_headers)

target_link_libraries(${PROJECT_NAME}
  viture_sdk
  OpenGL::GL
  GLU                    # if you still use gluPerspective
  ZLIB::ZLIB
  ${EGL_LIBRARIES}       # libEGL (for eglCreateImage + eglGetProcAddress)
  ${GLFW3_LIBRARIES}     # libglfw
  ${WAYLAND_LIBRARIES}   # libwayland-client
  ${DRM_LIBRARIES}       # libdrm
  ${GBM_LIBRARIES}       # libgbm
  m
  rt
)

# Helpful rpath for local run (adjust as needed)
set_target_properties(${PROJECT_NAME} PROPERTIES
  BUILD_RPATH "${CMAKE_INSTALL_PREFIX}/lib"
)

