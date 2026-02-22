
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
#ifdef HAVE_DMABUF
  /* Advertise GLMemory RGBA and DMABuf caps.
  /*
   * Advertise three cap structures in priority order:
   *
   * DMABuf caps use memory:DMABuf feature with standard video formats
   * (RGBA, NV12). This is the correct way to advertise DMABuf output
   * in GStreamer - downstream elements (e.g. vaapih264enc, vah264enc)
   * negotiate the actual format via caps intersection.
   * 1. memory:GLMemory RGBA — for glimagesink and GL consumers.
   *
   * The GLMemory RGBA path is listed first as highest priority for
   * glimagesink and similar GL consumers.
   *    * 2. memory:DMABuf with format=DMA_DRM — the GStreamer >= 1.24
   *    DMABuf modifier negotiation protocol.  Required for linking
   *    with vah264enc and other new VA plugin encoders.
   *
   *    The drm-format field is intentionally OMITTED from the
   *    template caps.  An absent field means "accept any value",
   *    which allows the caps intersection with any downstream
   *    drm-format string (e.g. "NV12:0x0200000018601b04") to
   *    succeed.  The actual drm-format is determined at runtime
   *    during negotiation and allocation.
   *
   * 3. memory:DMABuf with standard video format names (RGBA, NV12).
   *    Backward compatibility for vaapih264enc (deprecated) and
   *    older DMABuf consumers that don't use the DMA_DRM protocol.
   */
  return
      GST_VIDEO_CAPS_MAKE_WITH_FEATURES("memory:GLMemory", "RGBA") ";"
      "video/x-raw(memory:DMABuf), "
          "format = (string) DMA_DRM, "
          "width = " GST_VIDEO_SIZE_RANGE ", "
          "height = " GST_VIDEO_SIZE_RANGE ", "
          "framerate = " GST_VIDEO_FPS_RANGE ";"
      GST_VIDEO_CAPS_MAKE_WITH_FEATURES("memory:DMABuf", "{ RGBA, NV12 }");
#else
  return GST_VIDEO_CAPS_MAKE_WITH_FEATURES("memory:GLMemory", "RGBA");
#endif
}
