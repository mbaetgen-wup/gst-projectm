/* GStreamer
 * Copyright (C) <2011> Stefan Kost <ensonic@users.sf.net>
 * Copyright (C) <2015> Luis de Bethencourt <luis@debethencourt.com>
 *
 * gstaudiovisualizer.c: base class for audio visualisation elements
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 51 Franklin St, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 */

/*
 * The code in this file is based on
 * GStreamer / gst-plugins-base, latest version as of 2025/05/29.
 * gst-libs/gst/pbutils/gstaudiovisualizer.h Git Repository:
 * https://gitlab.freedesktop.org/gstreamer/gstreamer/-/blob/main/subprojects/gst-plugins-base/gst-libs/gst/pbutils/gstaudiovisualizer.h
 *
 * Original copyright notice has been retained at the top of this file.
 * The code has been modified to improve compatibility with projectM and OpenGL.
 * See impl for details.
 */

#ifndef __GST_PM_AUDIO_VISUALIZER_H__
#define __GST_PM_AUDIO_VISUALIZER_H__

#include <gst/gst.h>

#include <gst/audio/audio.h>
#include <gst/video/video.h>

G_BEGIN_DECLS
#define GST_TYPE_PM_AUDIO_VISUALIZER (gst_pm_audio_visualizer_get_type())
#define GST_PM_AUDIO_VISUALIZER(obj)                                           \
  (G_TYPE_CHECK_INSTANCE_CAST((obj), GST_TYPE_PM_AUDIO_VISUALIZER,             \
                              GstPMAudioVisualizer))
#define GST_PM_AUDIO_VISUALIZER_CLASS(klass)                                   \
  (G_TYPE_CHECK_CLASS_CAST((klass), GST_TYPE_PM_AUDIO_VISUALIZER,              \
                           GstPMAudioVisualizerClass))
#define GST_PM_AUDIO_VISUALIZER_GET_CLASS(obj)                                 \
  (G_TYPE_INSTANCE_GET_CLASS((obj), GST_TYPE_PM_AUDIO_VISUALIZER,              \
                             GstPMAudioVisualizerClass))
#define GST_PM_IS_SYNAESTHESIA(obj)                                            \
  (G_TYPE_CHECK_INSTANCE_TYPE((obj), GST_TYPE_PM_AUDIO_VISUALIZER))
#define GST_PM_IS_SYNAESTHESIA_CLASS(klass)                                    \
  (G_TYPE_CHECK_CLASS_TYPE((klass), GST_TYPE_PM_AUDIO_VISUALIZER))
typedef struct _GstPMAudioVisualizer GstPMAudioVisualizer;
typedef struct _GstPMAudioVisualizerClass GstPMAudioVisualizerClass;
typedef struct _GstPMAudioVisualizerPrivate GstPMAudioVisualizerPrivate;

struct _GstPMAudioVisualizer {
  GstElement parent;

  /**
   * current min samples per frame wanted by the subclass (one channel), may
   * vary depending on actual fps.
   */
  guint req_spf;

  /**
   * Current fps frame duration, may be different from caps fps.
   */
  guint64 req_frame_duration;

  /**
   * Caps video state.
   */
  GstVideoInfo vinfo;

  /**
   * Input audio state.
   */
  GstAudioInfo ainfo;

  /*< private >*/
  GstPMAudioVisualizerPrivate *priv;

  /**
   * Source pad to push video buffers downstream.
   */
  GstPad *srcpad;

  /**
   * Current pipeline latency.
   */
  GstClockTime latency;

  /*< private >*/
  gpointer _padding[GST_PADDING];
};

/**
 * GstPMAudioVisualizerClass:
 * @decide_allocation: buffer pool allocation
 * @render: render a frame from an audio buffer.
 * @setup: Called whenever the format changes, and sink and src caps are
 * configured.
 * @change_state: Cascades gst change state to the implementor. Parent is
 * processed first.
 * @segment_change: Cascades gst segment events to the implementor. Parent is
 * processed first.
 *
 * Base class for audio visualizers, derived from gstreamer
 * GstAudioVisualizerClass. This plugin consumes n audio input samples for each
 * output video frame to keep audio and video in-sync.
 */
struct _GstPMAudioVisualizerClass {
  /*< private >*/
  GstElementClass parent_class;

  /*< public >*/
  /**
   * Virtual function, called whenever the caps change, sink and src pad caps
   * are both configured.
   */
  gboolean (*setup)(GstPMAudioVisualizer *scope);

  /**
   * Virtual function for rendering a frame.
   */
  GstFlowReturn (*render)(GstPMAudioVisualizer *scope, GstBuffer *audio,
                          GstClockTime pts, guint64 frame_duration);

  /**
   * Virtual function for buffer pool allocation.
   */
  gboolean (*decide_allocation)(GstPMAudioVisualizer *scope, GstQuery *query);

  /**
   * Virtual function to allow implementor to receive change_state events.
   */
  GstStateChangeReturn (*change_state)(GstElement *element,
                                       GstStateChange transition);

  /**
   * Virtual function allow implementor to receive segment change events.
   */
  void (*segment_change)(GstPMAudioVisualizer *scope, GstSegment *segment);

  /*< private >*/
  gpointer _padding[GST_PADDING];
};

GType gst_pm_audio_visualizer_get_type(void);

/**
 * Obtain buffer from buffer pool for rendering.
 *
 * @param scope Plugin data.
 * @param outbuf Pointer for receiving output buffer.
 *
 * @return GST_FLOW_ERROR in case of pool errors, or the result of
 * gst_buffer_pool_acquire_buffer(...)
 */
GstFlowReturn
gst_pm_audio_visualizer_util_prepare_output_buffer(GstPMAudioVisualizer *scope,
                                                   GstBuffer **outbuf);

void gst_pm_audio_visualizer_adjust_fps(GstPMAudioVisualizer *scope,
                                        guint64 frame_duration);

void
gst_pm_audio_visualizer_dispose_buffer_pool(GstPMAudioVisualizer *scope);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(GstPMAudioVisualizer, gst_object_unref)

G_END_DECLS
#endif /* __GST_PM_AUDIO_VISUALIZER_H__ */
