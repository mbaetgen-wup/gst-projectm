
#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include "gstprojectmcaps.h"

#include "gstprojectm.h"

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
  return GST_VIDEO_CAPS_MAKE_WITH_FEATURES("memory:GLMemory", "RGBA");
}
