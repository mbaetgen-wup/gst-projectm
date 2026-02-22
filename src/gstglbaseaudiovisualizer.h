/*
 * GStreamer
 * Copyright (C) <1999> Erik Walthinsen <omega@cse.ogi.edu>
 * Copyright (C) 2002,2007 David A. Schleef <ds@schleef.org>
 * Copyright (C) 2008 Julien Isorce <julien.isorce@gmail.com>
 * Copyright (C) 2019 Philippe Normand <philn@igalia.com>
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
 * The code in this file is based on code from
 * GStreamer / gst-plugins-base / 1.19.2: gst-libs/gst/gl/gstglbasesrc.h
 * Git Repository:
 * https://github.com/GStreamer/gst-plugins-base/blob/master/gst-libs/gst/gl/gstglbasesrc.h
 * Original copyright notice has been retained at the top of this file.
 */

#ifndef __GST_GL_BASE_AUDIO_VISUALIZER_H__
#define __GST_GL_BASE_AUDIO_VISUALIZER_H__

#include "gstpmaudiovisualizer.h"

#include <gst/gl/gstgl_fwd.h>

#ifdef HAVE_DMABUF
#include "gstdmabufpool.h"
#include "gstnv12shader.h"
#include "gstdmabufegl.h"
#endif

typedef struct _GstGLBaseAudioVisualizer GstGLBaseAudioVisualizer;
typedef struct _GstGLBaseAudioVisualizerClass GstGLBaseAudioVisualizerClass;
typedef struct _GstGLBaseAudioVisualizerPrivate GstGLBaseAudioVisualizerPrivate;
typedef struct _GstAVRenderParams GstAVRenderParams;

G_DEFINE_AUTOPTR_CLEANUP_FUNC(GstGLBaseAudioVisualizer, gst_object_unref)

G_BEGIN_DECLS

GType gst_gl_base_audio_visualizer_get_type(void);

#define GST_TYPE_GL_BASE_AUDIO_VISUALIZER                                      \
  (gst_gl_base_audio_visualizer_get_type())
#define GST_GL_BASE_AUDIO_VISUALIZER(obj)                                      \
  (G_TYPE_CHECK_INSTANCE_CAST((obj), GST_TYPE_GL_BASE_AUDIO_VISUALIZER,        \
                              GstGLBaseAudioVisualizer))
#define GST_GL_BASE_AUDIO_VISUALIZER_CLASS(klass)                              \
  (G_TYPE_CHECK_CLASS_CAST((klass), GST_TYPE_GL_BASE_AUDIO_VISUALIZER,         \
                           GstGLBaseAudioVisualizerClass))
#define GST_IS_GL_BASE_AUDIO_VISUALIZER(obj)                                   \
  (G_TYPE_CHECK_INSTANCE_TYPE((obj), GST_TYPE_GL_BASE_AUDIO_VISUALIZER))
#define GST_IS_GL_BASE_AUDIO_VISUALIZER_CLASS(klass)                           \
  (G_TYPE_CHECK_CLASS_TYPE((klass), GST_TYPE_GL_BASE_AUDIO_VISUALIZER))
#define GST_GL_BASE_AUDIO_VISUALIZER_GET_CLASS(obj)                            \
  (G_TYPE_INSTANCE_GET_CLASS((obj), GST_TYPE_GL_BASE_AUDIO_VISUALIZER,         \
                             GstGLBaseAudioVisualizerClass))

/**
 * Plugin mode of operation type.
 */
typedef enum {
  /**
   * Real-time / live rendering.
   */
  GST_GL_BASE_AUDIO_VISUALIZER_REALTIME,

  /**
   * Faster-than-real-time rendering.
   */
  GST_GL_BASE_AUDIO_VISUALIZER_OFFLINE,

  /**
   * Auto-detect if pipeline is live.
   */
  GST_GL_BASE_AUDIO_VISUALIZER_AUTO
} GstGLBaseAudioVisualizerMode;

/**
 * GstGLBaseAudioVisualizer:
 * @display: the currently configured #GstGLDisplay
 * @context: the currently configured #GstGLContext
 *
 * The parent instance type of base GL Audio Visualizer.
 */
struct _GstGLBaseAudioVisualizer {
  GstPMAudioVisualizer parent;

  /*< public >*/
  GstGLDisplay *display;
  GstGLContext *context;

  /**
   * Minimum FPS numerator setting for EMA.
   */
  gint min_fps_n;

  /**
   * Minimum FPS denominator setting for EMA.
   */
  gint min_fps_d;

  /**
   * Operation mode property.
   */
  GstGLBaseAudioVisualizerMode is_live;

  GstGLBaseAudioVisualizerPrivate *priv;

  /*< private >*/
  gpointer _padding[GST_PADDING];
};

/**
 * GstGLBaseAudioVisualizerClass:
 * @supported_gl_api: the logical-OR of #GstGLAPI's supported by this element
 * @gl_start: called in the GL thread to set up the element GL state.
 * @gl_stop: called in the GL thread to clean up the element GL state.
 * @gl_render: called in the GL thread to fill the current video texture.
 * @setup: called when the format changes (delegate from
 * GstPMAudioVisualizer.setup)
 *
 * The base class for OpenGL based audio visualizers.
 * Extends GstPMAudioVisualizer to add GL rendering callbacks.
 * Handles GL context and render buffers.
 */
struct _GstGLBaseAudioVisualizerClass {
  GstPMAudioVisualizerClass parent_class;

  /*< public >*/
  /**
   * Supported OpenGL API flags.
   */
  GstGLAPI supported_gl_api;

  /**
   * Virtual function called from gl thread once the gl context can be used for
   * initializing gl resources.
   */
  gboolean (*gl_start)(GstGLBaseAudioVisualizer *glav);

  /**
   * Virtual function called from gl thread when gl context is being closed for
   * gl resource clean up.
   */
  void (*gl_stop)(GstGLBaseAudioVisualizer *glav);

  /**
   * Virtual function called when caps have been set for the pipeline.
   */
  gboolean (*setup)(GstGLBaseAudioVisualizer *glav);

  /* Virtual function called to render each frame, in_audio is optional. */
  gboolean (*fill_gl_memory)(GstAVRenderParams *render_data);

  /*< private >*/
  gpointer _padding[GST_PADDING];
};

/**
 * Parameter struct for rendering calls.
 */
struct _GstAVRenderParams {

  /**
   * Context plugin.
   */
  GstGLBaseAudioVisualizer *plugin;

  /**
   * Framebuffer to use for rendering.
   */
  GstGLFramebuffer *fbo;

  /**
   * Rendering target GL memory.
   */
  GstGLMemory *mem;

  /**
   * Audio data for frame.
   */
  GstBuffer *in_audio;

  /**
   * Current buffer presentation timestamp.
   */
  GstClockTime pts;

  /*< private >*/
  gpointer _padding[GST_PADDING];
};

G_END_DECLS

/**
 * Convert a string ("true", "false", "auto") to a GstGLBaseAudioVisualizerMode.
 */
GstGLBaseAudioVisualizerMode
gst_gl_base_audio_visualizer_mode_from_string(const gchar *str);

/**
 * Convert a GstGLBaseAudioVisualizerMode to a string ("true", "false", "auto").
 * Returns a static string; do not free.
 */
const gchar *
gst_gl_base_audio_visualizer_mode_to_string(GstGLBaseAudioVisualizerMode mode);

#endif /* __GST_GL_BASE_AUDIO_VISUALIZER_H__ */
