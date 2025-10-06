
add_executable(dyn-pads-example
        dynpads.c
)

target_include_directories(dyn-pads-example
        PUBLIC
        ${GSTREAMER_INCLUDE_DIRS}
        ${GSTREAMER_BASE_INCLUDE_DIRS}
        ${GSTREAMER_AUDIO_INCLUDE_DIRS}
        ${GSTREAMER_GL_INCLUDE_DIRS}
        ${GLIB2_INCLUDE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(dyn-pads-example
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
        gstprojectm
)
