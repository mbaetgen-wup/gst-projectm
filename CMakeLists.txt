cmake_minimum_required(VERSION 3.21 FATAL_ERROR)

include(CMakeDependentOption)
include(CheckSymbolExists)

set(OpenGL_GL_PREFERENCE "GLVND")

project(gstprojectm VERSION 0.0.1)

# ── Plugin version string ──────────────────────────────────────────
# Override via: cmake -DGSTPROJECTM_VERSION=1.2.3
# CI sets this to the tag version or commit hash for PR builds.
set(GSTPROJECTM_VERSION "unspecified" CACHE STRING
        "Plugin version string embedded in the GStreamer plugin metadata")

# ── Vendor metadata for static builds ─────────────────────────────
# When projectM is statically linked, these record which projectM
# source was used.  CI passes these; local builds default to empty.
set(PROJECTM_VENDOR_REF "" CACHE STRING
        "projectM git ref (tag/branch) used for static builds")
set(PROJECTM_VENDOR_COMMIT "" CACHE STRING
        "projectM short commit hash used for static builds")
set(PROJECTM_VENDOR_TIMESTAMP "" CACHE STRING
        "Build timestamp of the vendored projectM (ISO 8601)")
set(PROJECTM_VENDOR_GL_VARIANT "" CACHE STRING
        "GL API variant of the vendored projectM (e.g. gl or gles)")

list(APPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake")

find_package(projectM4 4.1.0 REQUIRED Playlist)

find_package(OpenGL REQUIRED)
find_package(GStreamer REQUIRED COMPONENTS gstreamer-audio gstreamer-gl gstreamer-pbutils gstreamer-video)
find_package(GLIB2 REQUIRED)

# On Windows, GStreamer GL headers #include <GL/glext.h> which comes from
# the Khronos OpenGL Registry, not the platform SDK. Locate it explicitly.
if(WIN32)
    find_path(GL_EXTENSION_INCLUDE_DIR GL/glext.h)
    if(GL_EXTENSION_INCLUDE_DIR)
        message(STATUS "Found GL/glext.h in: ${GL_EXTENSION_INCLUDE_DIR}")
    else()
        message(WARNING "GL/glext.h not found — GStreamer GL headers may fail to compile")
    endif()
endif()

add_library(gstprojectm SHARED
        src/caps.h
        src/caps.c
        src/debug.h
        src/debug.c
        src/config.h
        src/enums.h
        src/plugin.h
        src/plugin.c
        src/projectm.h
        src/projectm.c
        src/gstglbaseaudiovisualizer.h
        src/gstglbaseaudiovisualizer.c
)

target_include_directories(gstprojectm
        PUBLIC
        ${GSTREAMER_INCLUDE_DIRS}
        ${GSTREAMER_BASE_INCLUDE_DIRS}
        ${GSTREAMER_AUDIO_INCLUDE_DIRS}
        ${GSTREAMER_GL_INCLUDE_DIRS}
        ${GLIB2_INCLUDE_DIR}
        ${OPENGL_INCLUDE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}
)

if(WIN32 AND GL_EXTENSION_INCLUDE_DIR)
    target_include_directories(gstprojectm PUBLIC ${GL_EXTENSION_INCLUDE_DIR})
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

target_compile_definitions(gstprojectm
        PRIVATE
        PACKAGE_VERSION="${GSTPROJECTM_VERSION}"
)

# Pass vendor metadata as compile definitions (static builds only).
if(NOT PROJECTM_VENDOR_REF STREQUAL "")
    target_compile_definitions(gstprojectm PRIVATE
            HAVE_PROJECTM_VENDOR_INFO
            PROJECTM_VENDOR_REF="${PROJECTM_VENDOR_REF}"
            PROJECTM_VENDOR_COMMIT="${PROJECTM_VENDOR_COMMIT}"
            PROJECTM_VENDOR_TIMESTAMP="${PROJECTM_VENDOR_TIMESTAMP}"
            PROJECTM_VENDOR_GL_VARIANT="${PROJECTM_VENDOR_GL_VARIANT}"
    )
    message(STATUS "projectM vendor: ref=${PROJECTM_VENDOR_REF} commit=${PROJECTM_VENDOR_COMMIT} timestamp=${PROJECTM_VENDOR_TIMESTAMP} gl=${PROJECTM_VENDOR_GL_VARIANT}")
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
