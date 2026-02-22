cmake_minimum_required(VERSION 3.21 FATAL_ERROR)

include(CMakeDependentOption)
include(CheckSymbolExists)

set(OpenGL_GL_PREFERENCE "GLVND")

project(gstprojectm VERSION 0.0.1)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake")

find_package(projectM4 4.1.0 REQUIRED Playlist)

find_package(GStreamer REQUIRED COMPONENTS gstreamer-audio gstreamer-gl gstreamer-pbutils gstreamer-video)
find_package(GLIB2 REQUIRED)

# ---- Conditional DMABuf support detection ----
# DMABuf requires: gbm, egl, drm, gstallocators, gstgl (with EGL)
include(FindPkgConfig)

set(HAVE_DMABUF FALSE)
if(UNIX AND NOT APPLE)
    pkg_check_modules(GBM gbm)
    pkg_check_modules(EGL egl)
    pkg_check_modules(DRM libdrm)
    pkg_check_modules(GST_ALLOCATORS gstreamer-allocators-1.0)
    pkg_check_modules(GST_GL_EGL gstreamer-gl-egl-1.0)

    if(GBM_FOUND AND EGL_FOUND AND DRM_FOUND AND GST_ALLOCATORS_FOUND AND GST_GL_EGL_FOUND)
        set(HAVE_DMABUF TRUE)
        message(STATUS "DMABuf support: ENABLED")
        message(STATUS "  GBM: ${GBM_LIBRARIES}")
        message(STATUS "  EGL: ${EGL_LIBRARIES}")
        message(STATUS "  DRM: ${DRM_LIBRARIES}")
        message(STATUS "  GST Allocators: ${GST_ALLOCATORS_LIBRARIES}")
        message(STATUS "  GST GL EGL: ${GST_GL_EGL_LIBRARIES}")
    else()
        message(STATUS "DMABuf support: DISABLED (missing dependencies)")
        if(NOT GBM_FOUND)
            message(STATUS "  Missing: gbm")
        endif()
        if(NOT EGL_FOUND)
            message(STATUS "  Missing: egl")
        endif()
        if(NOT DRM_FOUND)
            message(STATUS "  Missing: libdrm")
        endif()
        if(NOT GST_ALLOCATORS_FOUND)
            message(STATUS "  Missing: gstreamer-allocators-1.0")
        endif()
        if(NOT GST_GL_EGL_FOUND)
            message(STATUS "  Missing: gstreamer-gl-egl-1.0")
        endif()
    endif()
else()
    message(STATUS "DMABuf support: DISABLED (Linux only)")
endif()

# ---- Core source files ----
set(GSTPROJECTM_SOURCES
    src/bufferdisposal.h
    src/bufferdisposal.c
    src/debug.h
    src/debug.c
    src/gstglbaseaudiovisualizer.h
    src/gstglbaseaudiovisualizer.c
    src/gstpmaudiovisualizer.h
    src/gstpmaudiovisualizer.c
    src/gstprojectm.h
    src/gstprojectm.c
    src/gstprojectmbase.h
    src/gstprojectmbase.c
    src/gstprojectmcaps.h
    src/gstprojectmcaps.c
    src/gstprojectmconfig.h
    src/pushbuffer.h
    src/pushbuffer.c
    src/register.c
    src/renderbuffer.h
    src/renderbuffer.c
)

# ---- Add DMABuf source files conditionally ----
if(HAVE_DMABUF)
    list(APPEND GSTPROJECTM_SOURCES
        src/gstdmabufpool.h
        src/gstdmabufpool.c
        src/gstdmabufegl.h
        src/gstdmabufegl.c
        src/gstnv12shader.h
        src/gstnv12shader.c
    )
endif()

add_library(gstprojectm MODULE ${GSTPROJECTM_SOURCES})

target_include_directories(gstprojectm
    PUBLIC
        ${GSTREAMER_INCLUDE_DIRS}
        ${GSTREAMER_BASE_INCLUDE_DIRS}
        ${GSTREAMER_AUDIO_INCLUDE_DIRS}
        ${GSTREAMER_GL_INCLUDE_DIRS}
        ${GLIB2_INCLUDE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}
)

# ---- DMABuf compile definitions and include dirs ----
if(HAVE_DMABUF)
    target_compile_definitions(gstprojectm PRIVATE HAVE_DMABUF=1)
    target_include_directories(gstprojectm
        PRIVATE
            ${GBM_INCLUDE_DIRS}
            ${EGL_INCLUDE_DIRS}
            ${DRM_INCLUDE_DIRS}
            ${GST_ALLOCATORS_INCLUDE_DIRS}
            ${GST_GL_EGL_INCLUDE_DIRS}
    )
endif()

message(STATUS "GSTREAMER_INCLUDE_DIRS: ${GSTREAMER_INCLUDE_DIRS}")
message(STATUS "GSTREAMER_LIBRARIES: ${GSTREAMER_LIBRARIES}")
message(STATUS "GSTREAMER_BASE_INCLUDE_DIRS: ${GSTREAMER_BASE_INCLUDE_DIRS}")
message(STATUS "GSTREAMER_BASE_LIBRARIES: ${GSTREAMER_BASE_LIBRARIES}")
message(STATUS "GSTREAMER_AUDIO_INCLUDE_DIRS: ${GSTREAMER_AUDIO_INCLUDE_DIRS}")
message(STATUS "GSTREAMER_AUDIO_LIBRARIES: ${GSTREAMER_AUDIO_LIBRARIES}")
message(STATUS "GSTREAMER_GL_INCLUDE_DIRS: ${GSTREAMER_GL_INCLUDE_DIRS}")
message(STATUS "GSTREAMER_GL_LIBRARIES: ${GSTREAMER_GL_LIBRARIES}")
message(STATUS "GLIB2_INCLUDE_DIR: ${GLIB2_INCLUDE_DIR}")
message(STATUS "GLIB2_LIBRARIES: ${GLIB2_LIBRARIES}")
message(STATUS "CMAKE_CURRENT_SOURCE_DIR: ${CMAKE_CURRENT_SOURCE_DIR}")

# GLEW needs to be initialized if libprojectM depends on it.
if(TARGET GLEW::glew OR TARGET GLEW::glew_s)
    target_compile_definitions(gstprojectm
            PRIVATE
            USE_GLEW
    )
endif()

target_link_libraries(gstprojectm
    PRIVATE
        libprojectM::projectM
        libprojectM::playlist
    PUBLIC
        ${GSTREAMER_LIBRARIES}
        ${GSTREAMER_BASE_LIBRARIES}
        ${GSTREAMER_AUDIO_LIBRARIES}
        ${GSTREAMER_VIDEO_LIBRARIES}
        ${GSTREAMER_GL_LIBRARIES}
        ${GSTREAMER_PBUTILS_LIBRARIES}
        ${GLIB2_LIBRARIES}
        ${GLIB2_GOBJECT_LIBRARIES}
)

# ---- DMABuf link libraries ----
if(HAVE_DMABUF)
    target_link_libraries(gstprojectm
        PRIVATE
            ${GBM_LIBRARIES}
            ${EGL_LIBRARIES}
            ${DRM_LIBRARIES}
            ${GST_ALLOCATORS_LIBRARIES}
            ${GST_GL_EGL_LIBRARIES}
    )
endif()
