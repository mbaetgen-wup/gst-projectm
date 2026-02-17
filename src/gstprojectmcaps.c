
#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include "gstprojectmcaps.h"

#include "gstprojectm.h"

#include <gst/gl/gl.h>

#if GST_GL_HAVE_PLATFORM_EGL && GST_GL_HAVE_DMABUF
#include <gst/allocators/gstdmabuf.h>
#endif

#include <gst/audio/audio-format.h>
#include <gst/video/video-format.h>

GST_DEBUG_CATEGORY_STATIC(gst_projectm_caps_debug);
#define GST_CAT_DEFAULT gst_projectm_caps_debug

const gchar *get_audio_sink_cap() {
  return GST_AUDIO_CAPS_MAKE("audio/x-raw, "
                             "format = (string) " GST_AUDIO_NE(
                                 S16) ", "
                                      "layout = (string) interleaved, "
                                      "channels = (int) { 2 }, "
                                      "rate = (int) { 44100 }, "
                                      "channel-mask = (bitmask) { 0x0003 }");
}

const gchar *get_video_src_cap() {
#if GST_GL_HAVE_PLATFORM_EGL && GST_GL_HAVE_DMABUF
  return GST_VIDEO_CAPS_MAKE_WITH_FEATURES(
      "memory:GLMemory",
      "RGBA") "; " GST_VIDEO_CAPS_MAKE_WITH_FEATURES(GST_CAPS_FEATURE_MEMORY_DMABUF,
                                                     "RGBA");
#else
  return GST_VIDEO_CAPS_MAKE_WITH_FEATURES("memory:GLMemory", "RGBA");
#endif
}
