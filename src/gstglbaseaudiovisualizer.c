/*
 * GStreamer
 * Copyright (C) <1999> Erik Walthinsen <omega@cse.ogi.edu>
 * Copyright (C) 2002,2007 David A. Schleef <ds@schleef.org>
 * Copyright (C) 2008 Julien Isorce <julien.isorce@gmail.com>
 * Copyright (C) 2015 Matthew Waters <matthew@centricular.com>
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
 * GStreamer / gst-plugins-base / 1.19.2: gst-libs/gst/gl/gstglbasesrc.c
 * Git Repository:
 * https://github.com/GStreamer/gst-plugins-base/blob/master/gst-libs/gst/gl/gstglbasesrc.c
 * Original copyright notice has been retained at the top of this file.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "gstglbaseaudiovisualizer.h"

#include "gstpmaudiovisualizer.h"
#include "renderbuffer.h"

#include <gst/gl/gl.h>
#include <gst/gst.h>

#ifdef HAVE_DMABUF
#include "gstdmabufpool.h"
#include "gstdmabufegl.h"
#include "gstnv12shader.h"

#include <gst/allocators/gstdmabuf.h>
#include <gbm.h>
#include <xf86drm.h>
#include <fcntl.h>
#include <unistd.h>
#include <drm_fourcc.h>
#include <string.h>
#endif

#ifdef _WIN32
#define strcasecmp _stricmp
#endif

/**
 * SECTION:GstGLBaseAudioVisualizer
 * @short_description: #GstPMAudioVisualizer subclass for injecting OpenGL
 * resources in a pipeline
 * @title: GstGLBaseAudioVisualizer
 * @see_also: #GstPMAudioVisualizer
 *
 * Wrapper for GstPMAudioVisualizer for handling OpenGL contexts.
 *
 * #GstGLBaseAudioVisualizer handles the nitty gritty details of retrieving an
 * OpenGL context. It also provides `gl_start()` and `gl_stop()` virtual methods
 * that ensure an OpenGL context is available and current in the calling thread
 * for initializing and cleaning up OpenGL resources.
 *
 * The `render` virtual method of the GstPMAudioVisualizer is pre-implemented to perform OpenGL
 * rendering. The implementer provides an implementation for fill_gl_memory to
 * render directly to gl memory.
 *
 * Rendering is performed blocking for offline rendering and asynchronously for real-time rendering.
 * The plugin detects if the pipeline clock is a real-time clock, if possible.
 *
 * Typical plug-in call order for implementer-provided functions:
 * - gl_start (once)
 * - setup (every time caps change, typically once)
 * - fill_gl_memory (once for each frame)
 * - gl_stop (once)
 */

#define GST_CAT_DEFAULT gst_gl_base_audio_visualizer_debug
GST_DEBUG_CATEGORY_STATIC(gst_gl_base_audio_visualizer_debug);

#define DEFAULT_TIMESTAMP_OFFSET 0

/**
 * Wait for up to 0.625 * fps frame duration for a free slot to queue input
 * audio for a frame. If the previous frame does not start rendering within this
 * time, it is dropped.
 */
#ifndef MAX_QUEUE_WAIT_TIME_IN_FRAME_DURATIONS_N
#define MAX_QUEUE_WAIT_TIME_IN_FRAME_DURATIONS_N 5
#endif

#ifndef MAX_QUEUE_WAIT_TIME_IN_FRAME_DURATIONS_D
#define MAX_QUEUE_WAIT_TIME_IN_FRAME_DURATIONS_D 8
#endif

/*
 * GST element property default values.
 */
#define DEFAULT_MIN_FPS_N 1
#define DEFAULT_MIN_FPS_D 1

struct _GstGLBaseAudioVisualizerPrivate {
  GstGLContext *other_context;

  gint64 n_frames; /* total frames sent */

  gboolean gl_started;

  GRecMutex context_lock;
  GstGLFramebuffer *fbo;
  RBRenderBuffer render_buffer;
  gboolean is_realtime;

#ifdef HAVE_DMABUF
  /* DMABuf output mode state */
  GstDmaBufOutputMode dmabuf_mode;

  /* GBM device for DMABuf allocation */
  struct gbm_device *gbm_device;
  gint drm_fd;

  /* DMABuf buffer pool */
  GstDmaBufPool *dmabuf_pool;
  gboolean use_downstream_pool;

  /* NV12 shader converter (for NV12 mode) */
  GstNV12ShaderConverter nv12_converter;
  gboolean nv12_converter_initialized;

  /* DRM modifier from GBM allocation */
  guint64 drm_modifier;
  gboolean drm_modifier_valid;
#endif
};

/* Properties */
enum { PROP_0, PROP_MIN_FPS_N, PROP_MIN_FPS_D };

#define gst_gl_base_audio_visualizer_parent_class parent_class
G_DEFINE_ABSTRACT_TYPE_WITH_CODE(
    GstGLBaseAudioVisualizer, gst_gl_base_audio_visualizer,
    GST_TYPE_PM_AUDIO_VISUALIZER,
    G_ADD_PRIVATE(GstGLBaseAudioVisualizer)
        GST_DEBUG_CATEGORY_INIT(gst_gl_base_audio_visualizer_debug,
                                "glbaseaudiovisualizer", 0,
                                "glbaseaudiovisualizer element"););

static void gst_gl_base_audio_visualizer_dispose(GObject *object);
static void gst_gl_base_audio_visualizer_set_property(GObject *object,
                                                      guint prop_id,
                                                      const GValue *value,
                                                      GParamSpec *pspec);
static void gst_gl_base_audio_visualizer_get_property(GObject *object,
                                                      guint prop_id,
                                                      GValue *value,
                                                      GParamSpec *pspec);

/**
 * Discover gl context / display from gst.
 */
static void gst_gl_base_audio_visualizer_set_context(GstElement *element,
                                                     GstContext *context);
/**
 * Handle pipeline state changes.
 */
static GstStateChangeReturn
gst_gl_base_audio_visualizer_change_state(GstElement *element,
                                          GstStateChange transition);

/**
 * Renders a video frame using gl, impl for parent class
 * GstPMAudioVisualizerClass.
 */
static GstFlowReturn
gst_gl_base_audio_visualizer_parent_render(GstPMAudioVisualizer *bscope,
                                           GstBuffer *audio, GstClockTime pts,
                                           guint64 frame_duration);

/**
 * Internal utility for resetting state on start.
 */
static void gst_gl_base_audio_visualizer_start(GstGLBaseAudioVisualizer *glav);

/**
 * Internal utility for cleaning up gl context on stop.
 */
static void gst_gl_base_audio_visualizer_stop(GstGLBaseAudioVisualizer *glav);

/**
 * GL memory pool allocation impl for parent class GstPMAudioVisualizerClass.
 */
static gboolean gst_gl_base_audio_visualizer_parent_decide_allocation(
    GstPMAudioVisualizer *pmav, GstQuery *query);

/**
 * called when format changes, default empty v-impl for this class. can be
 * overwritten by implementer.
 */
static gboolean
gst_gl_base_audio_visualizer_default_setup(GstGLBaseAudioVisualizer *glav);

/**
 * gl context is started and usable. called from gl thread. default empty v-impl
 * for this class, can be overwritten by implementer.
 */
static gboolean
gst_gl_base_audio_visualizer_default_gl_start(GstGLBaseAudioVisualizer *glav);

/**
 * GL context is shutting down. called from gl thread. default empty v-impl for
 * this class. can be overwritten by implementer.
 */
static void
gst_gl_base_audio_visualizer_default_gl_stop(GstGLBaseAudioVisualizer *glav);

/**
 * Default empty v-impl for rendering a frame. called from gl thread. can be
 * overwritten by implementer.
 */
static gboolean gst_gl_base_audio_visualizer_default_fill_gl_memory(
    GstAVRenderParams *render_data);

/**
 * Find a valid gl context. lock must have already been acquired.
 */
static gboolean gst_gl_base_audio_visualizer_find_gl_context_unlocked(
    GstGLBaseAudioVisualizer *glav);

/**
 * Called whenever the caps change, src and sink caps are both set. Impl for
 * parent class GstPMAudioVisualizerClass.
 */
static gboolean
gst_gl_base_audio_visualizer_parent_setup(GstPMAudioVisualizer *pmav);

/**
 * Called from gl thread: fbo rtt rending function.
 */
static void gst_gl_base_audio_visualizer_fill_gl(GstGLContext *context,
                                                 gpointer render_slot_ptr);

static void
gst_gl_base_audio_visualizer_class_init(GstGLBaseAudioVisualizerClass *klass) {
  GObjectClass *gobject_class = G_OBJECT_CLASS(klass);
  GstPMAudioVisualizerClass *pmav_class = GST_PM_AUDIO_VISUALIZER_CLASS(klass);
  GstElementClass *element_class = GST_ELEMENT_CLASS(klass);

  gobject_class->dispose = gst_gl_base_audio_visualizer_dispose;
  gobject_class->set_property = gst_gl_base_audio_visualizer_set_property;
  gobject_class->get_property = gst_gl_base_audio_visualizer_get_property;

  element_class->set_context =
      GST_DEBUG_FUNCPTR(gst_gl_base_audio_visualizer_set_context);

  pmav_class->change_state =
      GST_DEBUG_FUNCPTR(gst_gl_base_audio_visualizer_change_state);

  pmav_class->decide_allocation =
      GST_DEBUG_FUNCPTR(gst_gl_base_audio_visualizer_parent_decide_allocation);

  pmav_class->setup =
      GST_DEBUG_FUNCPTR(gst_gl_base_audio_visualizer_parent_setup);

  pmav_class->render =
      GST_DEBUG_FUNCPTR(gst_gl_base_audio_visualizer_parent_render);

  klass->supported_gl_api = GST_GL_API_ANY;

  klass->gl_start =
      GST_DEBUG_FUNCPTR(gst_gl_base_audio_visualizer_default_gl_start);

  klass->gl_stop =
      GST_DEBUG_FUNCPTR(gst_gl_base_audio_visualizer_default_gl_stop);

  klass->setup = GST_DEBUG_FUNCPTR(gst_gl_base_audio_visualizer_default_setup);

  klass->fill_gl_memory =
      GST_DEBUG_FUNCPTR(gst_gl_base_audio_visualizer_default_fill_gl_memory);

  g_object_class_install_property(
      gobject_class, PROP_MIN_FPS_N,
      g_param_spec_int("min-fps-n", "Min FPS numerator",
                       "Specifies the numerator for the min fps (EMA)", 1, 1000,
                       DEFAULT_MIN_FPS_N,
                       G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property(
      gobject_class, PROP_MIN_FPS_D,
      g_param_spec_int("min-fps-d", "Min FPS denominator",
                       "Specifies the denominator for the min fps (EMA)", 1,
                       1000, DEFAULT_MIN_FPS_D,
                       G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
}

/**
 * Callback function to receive fps changes from render buffer.
 *
 * @param user_data Render buffer to use.
 * @param frame_duration New fps frame duration.
 */
static void adjust_fps_callback(gpointer user_data, guint64 frame_duration) {

  if (frame_duration == 0) {
    return;
  }

  RBRenderBuffer *render_state = (RBRenderBuffer *)user_data;

  GstPMAudioVisualizer *scope = GST_PM_AUDIO_VISUALIZER(render_state->plugin);

  gst_pm_audio_visualizer_adjust_fps(scope, frame_duration);
}

static void gst_gl_base_audio_visualizer_init(GstGLBaseAudioVisualizer *glav) {
  glav->priv = gst_gl_base_audio_visualizer_get_instance_private(glav);
  glav->priv->gl_started = FALSE;
  glav->priv->fbo = NULL;
  glav->is_live = GST_GL_BASE_AUDIO_VISUALIZER_AUTO;
  glav->priv->is_realtime = FALSE;
  glav->context = NULL;

  glav->min_fps_n = DEFAULT_MIN_FPS_N;
  glav->min_fps_d = DEFAULT_MIN_FPS_D;

  g_rec_mutex_init(&glav->priv->context_lock);

#ifdef HAVE_DMABUF
  glav->priv->dmabuf_mode = GST_DMABUF_MODE_GLMEMORY_RGBA;
  glav->priv->gbm_device = NULL;
  glav->priv->drm_fd = -1;
  glav->priv->dmabuf_pool = NULL;
  glav->priv->nv12_converter_initialized = FALSE;
  glav->priv->drm_modifier = 0;
  glav->priv->drm_modifier_valid = FALSE;
#endif

  gst_gl_base_audio_visualizer_start(glav);
}

static void gst_gl_base_audio_visualizer_dispose(GObject *object) {
  GstGLBaseAudioVisualizer *glav = GST_GL_BASE_AUDIO_VISUALIZER(object);
  gst_gl_base_audio_visualizer_stop(glav);

  g_rec_mutex_clear(&glav->priv->context_lock);

  G_OBJECT_CLASS(parent_class)->dispose(object);
}

static void gst_gl_base_audio_visualizer_set_property(GObject *object,
                                                      guint prop_id,
                                                      const GValue *value,
                                                      GParamSpec *pspec) {
  GstGLBaseAudioVisualizer *glav = GST_GL_BASE_AUDIO_VISUALIZER(object);

  switch (prop_id) {

  case PROP_MIN_FPS_N:
    glav->min_fps_n = g_value_get_int(value);
    break;

  case PROP_MIN_FPS_D:
    glav->min_fps_d = g_value_get_int(value);
    break;

  default:
    G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
    break;
  }
}

static void gst_gl_base_audio_visualizer_get_property(GObject *object,
                                                      guint prop_id,
                                                      GValue *value,
                                                      GParamSpec *pspec) {
  GstGLBaseAudioVisualizer *glav = GST_GL_BASE_AUDIO_VISUALIZER(object);

  switch (prop_id) {

  case PROP_MIN_FPS_N:
    g_value_set_int(value, glav->min_fps_n);
    break;

  case PROP_MIN_FPS_D:
    g_value_set_int(value, glav->min_fps_d);
    break;

  default:
    G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
    break;
  }
}

static void gst_gl_base_audio_visualizer_set_context(GstElement *element,
                                                     GstContext *context) {
  GstGLBaseAudioVisualizer *glav = GST_GL_BASE_AUDIO_VISUALIZER(element);
  GstGLBaseAudioVisualizerClass *klass =
      GST_GL_BASE_AUDIO_VISUALIZER_GET_CLASS(glav);
  GstGLDisplay *old_display, *new_display;

  g_rec_mutex_lock(&glav->priv->context_lock);
  old_display = glav->display ? gst_object_ref(glav->display) : NULL;
  gst_gl_handle_set_context(element, context, &glav->display,
                            &glav->priv->other_context);
  if (glav->display)
    gst_gl_display_filter_gl_api(glav->display, klass->supported_gl_api);
  new_display = glav->display ? gst_object_ref(glav->display) : NULL;

  if (old_display && new_display) {
    if (old_display != new_display) {
      gst_clear_object(&glav->context);
      if (gst_gl_base_audio_visualizer_find_gl_context_unlocked(glav)) {
        gst_pad_mark_reconfigure(GST_BASE_SRC_PAD(glav));
      }
    }
  }
  gst_clear_object(&old_display);
  gst_clear_object(&new_display);
  g_rec_mutex_unlock(&glav->priv->context_lock);

  GST_ELEMENT_CLASS(parent_class)->set_context(element, context);
}

#if !defined(_WIN32) && GST_CHECK_VERSION(1, 24, 0)

/**
 * Find the pipeline and determine if it is live.
 * Not supported on Windows, gst < 1.24.
 *
 * @param element Plugin element.
 *
 * @return TRUE if the pipeline is live.
 */
static gboolean is_pipeline_live(GstElement *element) {
  GstPipeline *pipeline = NULL;
  gboolean is_live = FALSE;

  GstObject *parent = GST_OBJECT(element);
  while (parent && !GST_IS_PIPELINE(parent)) {
    GstObject *next = gst_object_get_parent(parent);
    if (parent != GST_OBJECT(element))
      gst_object_unref(parent);
    parent = next;
  }

  if (parent && GST_IS_PIPELINE(parent)) {
    pipeline = GST_PIPELINE(parent);
    is_live = gst_pipeline_is_live(pipeline);
    gst_object_unref(parent);
  }

  return is_live;
}

#endif

static gboolean
gst_gl_base_audio_visualizer_default_gl_start(GstGLBaseAudioVisualizer *glav) {
  return TRUE;
}

static gboolean
gst_gl_base_audio_visualizer_default_setup(GstGLBaseAudioVisualizer *glav) {
  return TRUE;
}

static void gst_gl_base_audio_visualizer_gl_start(GstGLContext *context,
                                                  gpointer data) {
  GstGLBaseAudioVisualizer *glav = GST_GL_BASE_AUDIO_VISUALIZER(data);
  GstPMAudioVisualizer *pmav = GST_PM_AUDIO_VISUALIZER(data);
  GstGLBaseAudioVisualizerClass *glav_class =
      GST_GL_BASE_AUDIO_VISUALIZER_GET_CLASS(glav);

  GST_INFO_OBJECT(glav, "starting");
  gst_gl_insert_debug_marker(glav->context, "starting element %s",
                             GST_OBJECT_NAME(glav));

  // init fbo for rtt
  glav->priv->fbo = gst_gl_framebuffer_new_with_default_depth(
      context, GST_VIDEO_INFO_WIDTH(&pmav->vinfo),
      GST_VIDEO_INFO_HEIGHT(&pmav->vinfo));

  // initialize render buffer
  GstClockTime max_frame_duration =
      gst_util_uint64_scale_int(GST_SECOND, glav->min_fps_d, glav->min_fps_n);

  GstClockTime caps_frame_duration =
      gst_util_uint64_scale_int(GST_SECOND, GST_VIDEO_INFO_FPS_D(&pmav->vinfo),
                                GST_VIDEO_INFO_FPS_N(&pmav->vinfo));

  // determine if we're using a real-time pipeline
  if (glav->is_live == GST_GL_BASE_AUDIO_VISUALIZER_OFFLINE) {
    glav->priv->is_realtime = FALSE;
  } else if (glav->is_live == GST_GL_BASE_AUDIO_VISUALIZER_REALTIME) {
    glav->priv->is_realtime = TRUE;
  } else {
    // auto-detect, unless we're on windows
#if defined(_WIN32) || !GST_CHECK_VERSION(1, 24, 0)
    glav->priv->is_realtime = FALSE;
#else
    glav->priv->is_realtime = is_pipeline_live(GST_ELEMENT(data));
#endif
  }

  // render loop QoS is disabled for offline rendering
  rb_init_render_buffer(
      &glav->priv->render_buffer, GST_OBJECT(glav), glav->context, pmav->srcpad,
      gst_gl_base_audio_visualizer_fill_gl, adjust_fps_callback,
      max_frame_duration, caps_frame_duration, glav->priv->is_realtime,
      glav->priv->is_realtime);

  // cascade gl start to implementor
  glav->priv->gl_started = glav_class->gl_start(glav);

  // get gl rendering going
  rb_start(&glav->priv->render_buffer);
}

static void
gst_gl_base_audio_visualizer_default_gl_stop(GstGLBaseAudioVisualizer *glav) {}

static void gst_gl_base_audio_visualizer_gl_stop(GstGLContext *context,
                                                 gpointer data) {
  GstGLBaseAudioVisualizer *glav = GST_GL_BASE_AUDIO_VISUALIZER(data);
  GstGLBaseAudioVisualizerClass *glav_class =
      GST_GL_BASE_AUDIO_VISUALIZER_GET_CLASS(glav);

  GST_INFO_OBJECT(glav, "gl stopping");
  gst_gl_insert_debug_marker(glav->context, "stopping element %s",
                             GST_OBJECT_NAME(glav));

  // stop gl rendering first
  rb_stop(&glav->priv->render_buffer);
  rb_clear(&glav->priv->render_buffer);

  // clean up implementor
  if (glav->priv->gl_started) {
    glav_class->gl_stop(glav);
  }

  glav->priv->gl_started = FALSE;

  // clean up render buffer
  rb_dispose_render_buffer(&glav->priv->render_buffer);

#ifdef HAVE_DMABUF
  // clean up DMABuf resources (NV12 converter must be freed on GL thread)
  if (glav->priv->nv12_converter_initialized) {
    gst_nv12_shader_dispose(&glav->priv->nv12_converter);
    glav->priv->nv12_converter_initialized = FALSE;
  }
#endif

  // clean up state
  if (glav->priv->fbo) {
    gst_object_unref(glav->priv->fbo);
  }

  gst_pm_audio_visualizer_dispose_buffer_pool(GST_PM_AUDIO_VISUALIZER(data));
}


#ifdef HAVE_DMABUF

/**
 * Open a DRM render node and create a GBM device.
 * Called once during allocation setup.
 *
 * @param glav The plugin instance.
 * @return TRUE on success.
 */
static gboolean
gst_gl_base_audio_visualizer_init_gbm(GstGLBaseAudioVisualizer *glav) {
  if (glav->priv->gbm_device)
    return TRUE;

  /* Try common render nodes */
  const gchar *render_nodes[] = {
    "/dev/dri/renderD128",
    "/dev/dri/renderD129",
    NULL
  };

  for (gint i = 0; render_nodes[i]; i++) {
    glav->priv->drm_fd = open(render_nodes[i], O_RDWR | O_CLOEXEC);
    if (glav->priv->drm_fd >= 0) {
      glav->priv->gbm_device = gbm_create_device(glav->priv->drm_fd);
      if (glav->priv->gbm_device) {
        GST_INFO_OBJECT(glav, "GBM device created from %s", render_nodes[i]);
        return TRUE;
      }
      close(glav->priv->drm_fd);
      glav->priv->drm_fd = -1;
    }
  }

  GST_WARNING_OBJECT(glav, "Failed to create GBM device from any render node");
  return FALSE;
}

/**
 * Release GBM device and DRM fd.
 */
static void
gst_gl_base_audio_visualizer_cleanup_gbm(GstGLBaseAudioVisualizer *glav) {
  if (glav->priv->dmabuf_pool) {
    gst_buffer_pool_set_active(GST_BUFFER_POOL(glav->priv->dmabuf_pool), FALSE);
    gst_object_unref(glav->priv->dmabuf_pool);
    glav->priv->dmabuf_pool = NULL;
  }

  if (glav->priv->nv12_converter_initialized) {
    gst_nv12_shader_dispose(&glav->priv->nv12_converter);
    glav->priv->nv12_converter_initialized = FALSE;
  }

  if (glav->priv->gbm_device) {
    gbm_device_destroy(glav->priv->gbm_device);
    glav->priv->gbm_device = NULL;
  }

  if (glav->priv->drm_fd >= 0) {
    close(glav->priv->drm_fd);
    glav->priv->drm_fd = -1;
  }
}


static gboolean gst_gl_base_audio_visualizer_default_fill_gl_memory(
    GstAVRenderParams *render_data) {
  (void)render_data;
  return TRUE;
}

static void gst_gl_base_audio_visualizer_fill_gl(GstGLContext *context,
                                                 gpointer render_slot_ptr) {

  // we're inside the gl thread!

  RBSlot *render_slot = (RBSlot *)render_slot_ptr;

  GstGLBaseAudioVisualizer *glav =
      GST_GL_BASE_AUDIO_VISUALIZER(render_slot->plugin);

  GstGLBaseAudioVisualizerClass *klass =
      GST_GL_BASE_AUDIO_VISUALIZER_GET_CLASS(render_slot->plugin);

  GstPMAudioVisualizer *pmav = GST_PM_AUDIO_VISUALIZER(render_slot->plugin);

  GstBuffer *out_buf = NULL;
  GstVideoFrame out_video;

#ifdef HAVE_DMABUF
  if (glav->priv->dmabuf_mode == GST_DMABUF_MODE_DMABUF_RGBA ||
      glav->priv->dmabuf_mode == GST_DMABUF_MODE_DMABUF_NV12) {

    /* DMABuf rendering path - acquire from DMABuf pool */
    {
      GstFlowReturn flow_ret =
          gst_pm_audio_visualizer_util_prepare_output_buffer(pmav, &out_buf);
      if (flow_ret != GST_FLOW_OK || out_buf == NULL) {
        GST_ERROR_OBJECT(glav,
            "DMABuf pool buffer acquisition failed (flow=%d, buf=%p)",
            flow_ret, out_buf);
        render_slot->gl_result = FALSE;
        render_slot->out_buf = NULL;
        return;
      }
    }

    if (glav->priv->dmabuf_mode == GST_DMABUF_MODE_DMABUF_RGBA) {
      /*
       * MODE_DMABUF_RGBA:
       * Import DMABuf → EGLImage → GL texture, bind to FBO, render.
       */
      if (gst_buffer_n_memory(out_buf) < 1) {
        GST_ERROR_OBJECT(glav, "RGBA DMABuf buffer has no memory planes");
        render_slot->gl_result = FALSE;
        render_slot->out_buf = out_buf;
        return;
      }
      GstMemory *mem = gst_buffer_peek_memory(out_buf, 0);
      gint fd = gst_dmabuf_memory_get_fd(mem);
      GstVideoMeta *vmeta = gst_buffer_get_video_meta(out_buf);
      guint width = vmeta ? vmeta->width : GST_VIDEO_INFO_WIDTH(&pmav->vinfo);
      guint height = vmeta ? vmeta->height : GST_VIDEO_INFO_HEIGHT(&pmav->vinfo);
      guint stride = vmeta ? vmeta->stride[0] : width * 4;

      GstDmaBufEGLImage egl_img;
      if (!gst_dmabuf_egl_import(&egl_img, glav->context, fd,
                                  width, height, stride, 0,
                                  DRM_FORMAT_ARGB8888,
                                  glav->priv->drm_modifier)) {
        GST_ERROR_OBJECT(glav, "Failed to import RGBA DMABuf as EGLImage");
        render_slot->gl_result = FALSE;
        render_slot->out_buf = out_buf;
        return;
      }

      /* Bind EGLImage texture to FBO and render into it */
      const GstGLFuncs *gl = glav->context->gl_vtable;
      guint fbo_id;
      gl->GenFramebuffers(1, &fbo_id);
      gl->BindFramebuffer(GL_FRAMEBUFFER, fbo_id);
      gl->FramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                                GL_TEXTURE_2D, egl_img.gl_tex, 0);

      gl->Viewport(0, 0, width, height);

      GstAVRenderParams ds_rd;
      ds_rd.in_audio = render_slot->in_audio;
      ds_rd.mem = NULL; /* no GLMemory in DMABuf mode */
      ds_rd.fbo = glav->priv->fbo;
      ds_rd.pts = render_slot->pts;
      ds_rd.plugin = glav;

      render_slot->gl_result = klass->fill_gl_memory(&ds_rd);

      gl->BindFramebuffer(GL_FRAMEBUFFER, 0);
      gl->DeleteFramebuffers(1, &fbo_id);

      gst_dmabuf_egl_release(&egl_img, glav->context);

    } else { /* GST_DMABUF_MODE_DMABUF_NV12 */
      /*
       * MODE_DMABUF_NV12 — zero-copy VA-API path:
       *
       *   VA allocates tiled NV12 DMABuf
       *           ↓
       *   Import planes as R8 + GR88 EGLImages
       *           ↓
       *   Render visualizer → intermediate RGBA FBO
       *           ↓
       *   Shader convert RGBA → Y plane (R8, full res)
       *           ↓
       *   Shader convert RGBA → UV plane (GR88, half res)
       *           ↓
       *   Add GL sync meta
       *           ↓
       *   Push buffer → vah264enc uses same memory
       *
       * NV12 is NOT color-renderable, so we cannot FBO-attach it directly.
       * Instead we import each plane separately:
       *   Plane 0 (Y)  → DRM_FORMAT_R8  → color-renderable ✔
       *   Plane 1 (UV) → DRM_FORMAT_GR88 → color-renderable ✔
       */
      guint width = GST_VIDEO_INFO_WIDTH(&pmav->vinfo);
      guint height = GST_VIDEO_INFO_HEIGHT(&pmav->vinfo);

      /* Initialize NV12 converter on first use (GL thread) */
      if (!glav->priv->nv12_converter_initialized) {
        if (!gst_nv12_shader_init(&glav->priv->nv12_converter,
                                   glav->context, width, height)) {
          GST_ERROR_OBJECT(glav, "Failed to init NV12 shader converter");
          render_slot->gl_result = FALSE;
          render_slot->out_buf = out_buf;
          return;
        }
        glav->priv->nv12_converter_initialized = TRUE;
      }

      /* Step 1: Render scene to intermediate RGBA FBO */
      GstAVRenderParams ds_rd;
      ds_rd.in_audio = render_slot->in_audio;
      ds_rd.mem = NULL;
      ds_rd.fbo = gst_nv12_shader_get_rgba_fbo(&glav->priv->nv12_converter);
      ds_rd.pts = render_slot->pts;
      ds_rd.plugin = glav;

      render_slot->gl_result = klass->fill_gl_memory(&ds_rd);

      if (!render_slot->gl_result) {
        GST_WARNING_OBJECT(glav, "Scene render to intermediate RGBA failed");
        render_slot->out_buf = out_buf;
        return;
      }

      /* Step 2: Extract DMABuf fd(s) and plane geometry.
       *
       * Buffer layouts:
       *   VA-API (single-fd):  n_mem == 1, planes at different offsets
       *   GBM    (multi-fd):   n_mem >= 2, separate fd per plane
       */
      guint n_mem = gst_buffer_n_memory(out_buf);
      GstVideoMeta *vmeta = gst_buffer_get_video_meta(out_buf);

      gint y_fd, uv_fd;
      guint y_stride, uv_stride, y_offset, uv_offset;

      if (n_mem >= 2) {
        /* GBM-style: separate fds per plane */
        GstMemory *y_mem_obj = gst_buffer_peek_memory(out_buf, 0);
        GstMemory *uv_mem_obj = gst_buffer_peek_memory(out_buf, 1);
        y_fd = gst_dmabuf_memory_get_fd(y_mem_obj);
        uv_fd = gst_dmabuf_memory_get_fd(uv_mem_obj);
        y_stride = vmeta ? vmeta->stride[0] : width;
        uv_stride = vmeta ? vmeta->stride[1] : width;
        y_offset = vmeta ? vmeta->offset[0] : 0;
        uv_offset = vmeta ? vmeta->offset[1] : 0;
      } else if (n_mem == 1) {
        /* VA-API style: single fd, planes at different offsets */
        GstMemory *mem = gst_buffer_peek_memory(out_buf, 0);
        if (!gst_is_dmabuf_memory(mem)) {
          GST_ERROR_OBJECT(glav, "NV12 buffer memory is not DMABuf");
          render_slot->gl_result = FALSE;
          render_slot->out_buf = out_buf;
          return;
        }
        y_fd = gst_dmabuf_memory_get_fd(mem);
        uv_fd = y_fd;
        if (vmeta && vmeta->n_planes >= 2) {
          y_stride = vmeta->stride[0];
          uv_stride = vmeta->stride[1];
          y_offset = vmeta->offset[0];
          uv_offset = vmeta->offset[1];
        } else {
          y_stride = width;
          uv_stride = width;
          y_offset = 0;
          uv_offset = (guint)((gsize)width * height);
          GST_WARNING_OBJECT(glav,
              "No VideoMeta; guessing NV12 layout");
        }
      } else {
        GST_ERROR_OBJECT(glav,
            "NV12 buffer has %u memories, expected 1 or 2", n_mem);
        render_slot->gl_result = FALSE;
        render_slot->out_buf = out_buf;
        return;
      }

      GST_INFO_OBJECT(glav,
          "NV12: n_mem=%u y_fd=%d uv_fd=%d "
          "Y(off=%u stride=%u) UV(off=%u stride=%u) "
          "mod=0x%" G_GINT64_MODIFIER "x",
          n_mem, y_fd, uv_fd,
          y_offset, y_stride, uv_offset, uv_stride,
          glav->priv->drm_modifier);

      /* Step 3: Import planes separately as R8 and GR88 EGLImages.
       *
       * These sub-formats ARE color-renderable, so the shader can
       * write into them via FBO attachment.  The modifier is passed
       * so the EGL driver handles the tiled memory layout. */
      guint uv_width = (width + 1) / 2;
      guint uv_height = (height + 1) / 2;

      GstDmaBufEGLImage y_egl;
      if (!gst_dmabuf_egl_import(&y_egl, glav->context, y_fd,
                                  width, height, y_stride, y_offset,
                                  DRM_FORMAT_R8,
                                  glav->priv->drm_modifier)) {
        GST_ERROR_OBJECT(glav, "Y plane import failed (R8, mod=0x%"
                         G_GINT64_MODIFIER "x)", glav->priv->drm_modifier);
        render_slot->gl_result = FALSE;
        render_slot->out_buf = out_buf;
        return;
      }

      GstDmaBufEGLImage uv_egl;
      if (!gst_dmabuf_egl_import(&uv_egl, glav->context, uv_fd,
                                  uv_width, uv_height, uv_stride, uv_offset,
                                  DRM_FORMAT_GR88,
                                  glav->priv->drm_modifier)) {
        GST_ERROR_OBJECT(glav, "UV plane import failed (GR88, mod=0x%"
                         G_GINT64_MODIFIER "x)", glav->priv->drm_modifier);
        gst_dmabuf_egl_release(&y_egl, glav->context);
        render_slot->gl_result = FALSE;
        render_slot->out_buf = out_buf;
        return;
      }

      /* Step 4: Convert RGBA → Y + UV via shader passes.
       * Each pass attaches the plane texture as FBO color target. */
      guint rgba_tex = gst_nv12_shader_get_rgba_tex(
          &glav->priv->nv12_converter);

      GST_INFO_OBJECT(glav,
          "NV12 convert: rgba_tex=%u y_tex=%u(%ux%u) uv_tex=%u(%ux%u)",
          rgba_tex, y_egl.gl_tex, width, height,
          uv_egl.gl_tex, uv_width, uv_height);

      render_slot->gl_result = gst_nv12_shader_convert(
          &glav->priv->nv12_converter, rgba_tex,
          y_egl.gl_tex, uv_egl.gl_tex);

      if (!render_slot->gl_result)
        GST_ERROR_OBJECT(glav, "NV12 shader conversion failed");

      gst_dmabuf_egl_release(&y_egl, glav->context);
      gst_dmabuf_egl_release(&uv_egl, glav->context);

      /* Step 5: GL sync meta — ensures VA encoder waits for GL rendering
       * to complete before reading the surface. */
      {
        GstGLSyncMeta *sync_meta = gst_buffer_get_gl_sync_meta(out_buf);
        if (!sync_meta) {
          gst_buffer_add_gl_sync_meta(glav->context, out_buf);
          sync_meta = gst_buffer_get_gl_sync_meta(out_buf);
        }
        if (sync_meta)
          gst_gl_sync_meta_set_sync_point(sync_meta, glav->context);
      }
    }

    render_slot->out_buf = out_buf;
    /* ownership transferred */
    return;
  }
#endif /* HAVE_DMABUF */

  /* =========================================================
   * Standard GLMemory RGBA path (MODE_GLMEMORY_RGBA, unchanged)
   * ========================================================= */

  // obtain output buffer from the (GL texture backed) pool
  gst_pm_audio_visualizer_util_prepare_output_buffer(pmav, &out_buf);

  /* Validate that the buffer actually contains GLMemory before proceeding. */
  if (gst_buffer_n_memory(out_buf) == 0) {
    GST_ERROR_OBJECT(glav, "Output buffer has no memory attached");
    render_slot->gl_result = FALSE;
    render_slot->out_buf = out_buf;
    return;
  }

  {
    GstMemory *mem_check = gst_buffer_peek_memory(out_buf, 0);
    if (!gst_is_gl_memory(mem_check)) {
      GST_ERROR_OBJECT(glav,
          "Output buffer memory is not GLMemory (type=%s). "
          "DMABuf was likely negotiated but allocation failed.",
          g_type_name(G_OBJECT_TYPE(mem_check)));
      render_slot->gl_result = FALSE;
      render_slot->out_buf = out_buf;
      return;
    }
  }

  // Check for GL sync meta
  GstGLSyncMeta *sync_meta = gst_buffer_get_gl_sync_meta(out_buf);

  // if (sync_meta) {
  //    wait until GPU is done using this buffer should not be needed
  //    gst_gl_sync_meta_wait(sync_meta, glav->context);
  // }

  // map output video frame to buffer outbuf with gl flags
  if (!gst_video_frame_map(&out_video, &pmav->vinfo, out_buf,
                      GST_MAP_WRITE | GST_MAP_GL |
                          GST_VIDEO_FRAME_MAP_FLAG_NO_REF)) {
    GST_ERROR_OBJECT(glav,
        "Failed to map output buffer as GL video frame (format=%s)",
        gst_video_format_to_string(GST_VIDEO_INFO_FORMAT(&pmav->vinfo)));
    render_slot->gl_result = FALSE;
    render_slot->out_buf = out_buf;
    return;
  }

  GstAVRenderParams ds_rd;
  ds_rd.in_audio = render_slot->in_audio;
  ds_rd.mem = GST_GL_MEMORY_CAST(gst_buffer_peek_memory(out_buf, 0));
  ds_rd.fbo = glav->priv->fbo;
  ds_rd.pts = render_slot->pts;
  ds_rd.plugin = glav;

  GST_TRACE_OBJECT(render_slot->plugin, "filling gl memory %p", ds_rd.mem);

  // call virtual render function with audio and video
  render_slot->gl_result = klass->fill_gl_memory(&ds_rd);

  gst_video_frame_unmap(&out_video);

  if (sync_meta)
    gst_gl_sync_meta_set_sync_point(sync_meta, glav->context);

  render_slot->out_buf = out_buf;
  // ownership transferred
  out_buf = NULL;
}

static GstFlowReturn gst_gl_base_audio_visualizer_fill(
    GstPMAudioVisualizer *bscope, GstGLBaseAudioVisualizer *glav,
    GstBuffer *audio, GstClockTime pts, guint64 frame_duration) {

  g_rec_mutex_lock(&glav->priv->context_lock);
  if (G_UNLIKELY(!glav->context))
    goto not_negotiated;

  /* 0 framerate and we are at the second frame, eos */
  if (G_UNLIKELY(GST_VIDEO_INFO_FPS_N(&bscope->vinfo) == 0 &&
                 glav->priv->n_frames == 1))
    goto eos;

  if (glav->priv->is_realtime == FALSE) {
    g_rec_mutex_unlock(&glav->priv->context_lock);

    // offline rendering can be done synchronously, avoid queuing overhead
    rb_render_blocking(&glav->priv->render_buffer, audio, pts, frame_duration);

    g_rec_mutex_lock(&glav->priv->context_lock);
  } else {
    // prepare args for queuing frame rendering
    RBQueueArgs args;
    args.render_buffer = &glav->priv->render_buffer;
    args.in_audio = audio;
    args.pts = pts;
    args.frame_duration = frame_duration;

    // limit wait based on fps factor, make sure we never wait too long in order
    // to keep in sync
    args.max_wait = (GstClockTimeDiff)gst_util_uint64_scale_int(
        frame_duration, MAX_QUEUE_WAIT_TIME_IN_FRAME_DURATIONS_N,
        MAX_QUEUE_WAIT_TIME_IN_FRAME_DURATIONS_D);

    g_rec_mutex_unlock(&glav->priv->context_lock);

    // dispatch gst_gl_base_audio_visualizer_fill_gl to the gl render buffer,
    // rendering is deferred. This may block for a while though.
    rb_queue_render_task_log(&args);

    g_rec_mutex_lock(&glav->priv->context_lock);
  }

  glav->priv->n_frames++;

  g_rec_mutex_unlock(&glav->priv->context_lock);

  return GST_FLOW_OK;

not_negotiated: {
  g_rec_mutex_unlock(&glav->priv->context_lock);
  GST_ELEMENT_ERROR(glav, CORE, NEGOTIATION, (NULL),
                    (("format wasn't negotiated before get function")));
  return GST_FLOW_NOT_NEGOTIATED;
}
eos: {
  g_rec_mutex_unlock(&glav->priv->context_lock);
  GST_DEBUG_OBJECT(glav, "eos: 0 framerate, frame %d",
                   (gint)glav->priv->n_frames);
  return GST_FLOW_EOS;
}
}

static gboolean
gst_gl_base_audio_visualizer_parent_setup(GstPMAudioVisualizer *pmav) {
  GstGLBaseAudioVisualizer *glav = GST_GL_BASE_AUDIO_VISUALIZER(pmav);
  GstGLBaseAudioVisualizerClass *glav_class =
      GST_GL_BASE_AUDIO_VISUALIZER_GET_CLASS(pmav);

  GstClockTime caps_frame_duration =
      gst_util_uint64_scale_int(GST_SECOND, GST_VIDEO_INFO_FPS_D(&pmav->vinfo),
                                GST_VIDEO_INFO_FPS_N(&pmav->vinfo));

  rb_set_caps_frame_duration(&glav->priv->render_buffer, caps_frame_duration);

  GST_INFO_OBJECT(glav,
                  "GL setup - render config: is-live: %s, caps-frame-duration: "
                  "%" GST_TIME_FORMAT
                  ", min-fps: %d/%d, min-fps-duration: %" GST_TIME_FORMAT,
                  glav->priv->is_realtime ? "true" : "false",
                  GST_TIME_ARGS(caps_frame_duration), glav->min_fps_n,
                  glav->min_fps_d,
                  GST_TIME_ARGS(glav->priv->render_buffer.max_frame_duration));

  // cascade setup to the derived plugin after gl initialization has been
  // completed
  return glav_class->setup(glav);
}

static GstFlowReturn
gst_gl_base_audio_visualizer_parent_render(GstPMAudioVisualizer *bscope,
                                           GstBuffer *audio, GstClockTime pts,
                                           guint64 frame_duration) {
  GstGLBaseAudioVisualizer *glav = GST_GL_BASE_AUDIO_VISUALIZER(bscope);

  return gst_gl_base_audio_visualizer_fill(bscope, glav, audio, pts,
                                           frame_duration);
}

static void gst_gl_base_audio_visualizer_start(GstGLBaseAudioVisualizer *glav) {
  glav->priv->n_frames = 0;
}

static void gst_gl_base_audio_visualizer_stop(GstGLBaseAudioVisualizer *glav) {
  g_rec_mutex_lock(&glav->priv->context_lock);

  if (glav->context) {
    if (glav->priv->gl_started)
      gst_gl_context_thread_add(glav->context,
                                gst_gl_base_audio_visualizer_gl_stop, glav);

    gst_object_unref(glav->context);
  }

  glav->context = NULL;

#ifdef HAVE_DMABUF
  gst_gl_base_audio_visualizer_cleanup_gbm(glav);
#endif

  g_rec_mutex_unlock(&glav->priv->context_lock);
}

static gboolean
_find_local_gl_context_unlocked(GstGLBaseAudioVisualizer *glav) {
  GstGLContext *context, *prev_context;
  gboolean ret;

  if (glav->context && glav->context->display == glav->display)
    return TRUE;

  context = prev_context = glav->context;
  g_rec_mutex_unlock(&glav->priv->context_lock);
  /* we need to drop the lock to query as another element may also be
   * performing a context query on us which would also attempt to take the
   * context_lock. Our query could block on the same lock in the other element.
   */
  ret = gst_gl_query_local_gl_context(GST_ELEMENT(glav), GST_PAD_SRC, &context);
  g_rec_mutex_lock(&glav->priv->context_lock);
  if (ret) {
    if (glav->context != prev_context) {
      /* we need to recheck everything since we dropped the lock and the
       * context has changed */
      if (glav->context && glav->context->display == glav->display) {
        if (context != glav->context)
          gst_clear_object(&context);
        return TRUE;
      }
    }

    if (context->display == glav->display) {
      glav->context = context;
      return TRUE;
    }
    if (context != glav->context)
      gst_clear_object(&context);
  }
  return FALSE;
}

static gboolean gst_gl_base_audio_visualizer_find_gl_context_unlocked(
    GstGLBaseAudioVisualizer *glav) {
  GstGLBaseAudioVisualizerClass *klass =
      GST_GL_BASE_AUDIO_VISUALIZER_GET_CLASS(glav);
  GError *error = NULL;
  gboolean new_context = FALSE;

  GST_DEBUG_OBJECT(
      glav, "attempting to find an OpenGL context, existing %" GST_PTR_FORMAT,
      glav->context);

  if (!glav->context)
    new_context = TRUE;

  if (!gst_gl_ensure_element_data(glav, &glav->display,
                                  &glav->priv->other_context))
    return FALSE;

  gst_gl_display_filter_gl_api(glav->display, klass->supported_gl_api);

  _find_local_gl_context_unlocked(glav);

  if (!glav->context) {
    GST_OBJECT_LOCK(glav->display);
    do {
      if (glav->context) {
        gst_object_unref(glav->context);
        glav->context = NULL;
      }
      /* just get a GL context.  we don't care */
      glav->context =
          gst_gl_display_get_gl_context_for_thread(glav->display, NULL);
      if (!glav->context) {
        if (!gst_gl_display_create_context(glav->display,
                                           glav->priv->other_context,
                                           &glav->context, &error)) {
          GST_OBJECT_UNLOCK(glav->display);
          goto context_error;
        }
      }
    } while (!gst_gl_display_add_context(glav->display, glav->context));
    GST_OBJECT_UNLOCK(glav->display);
  }
  GST_INFO_OBJECT(glav, "found OpenGL context %" GST_PTR_FORMAT, glav->context);

  if (new_context || !glav->priv->gl_started) {
    if (glav->priv->gl_started)
      gst_gl_context_thread_add(glav->context,
                                gst_gl_base_audio_visualizer_gl_stop, glav);

    {
      if ((gst_gl_context_get_gl_api(glav->context) &
           klass->supported_gl_api) == 0)
        goto unsupported_gl_api;
    }

    gst_gl_context_thread_add(glav->context,
                              gst_gl_base_audio_visualizer_gl_start, glav);

    if (!glav->priv->gl_started)
      goto error;
  }

  return TRUE;

unsupported_gl_api: {
  GstGLAPI gl_api = gst_gl_context_get_gl_api(glav->context);
  gchar *gl_api_str = gst_gl_api_to_string(gl_api);
  gchar *supported_gl_api_str = gst_gl_api_to_string(klass->supported_gl_api);
  GST_ELEMENT_ERROR(glav, RESOURCE, BUSY,
                    ("GL API's not compatible context: %s supported: %s",
                     gl_api_str, supported_gl_api_str),
                    (NULL));

  g_free(supported_gl_api_str);
  g_free(gl_api_str);
  return FALSE;
}
context_error: {
  if (error) {
    GST_ELEMENT_ERROR(glav, RESOURCE, NOT_FOUND, ("%s", error->message),
                      (NULL));
    g_clear_error(&error);
  } else {
    GST_ELEMENT_ERROR(glav, RESOURCE, NOT_FOUND, (NULL), (NULL));
  }
  if (glav->context)
    gst_object_unref(glav->context);
  glav->context = NULL;
  return FALSE;
}
error: {
  GST_ELEMENT_ERROR(glav, LIBRARY, INIT, ("Subclass failed to initialize."),
                    (NULL));
  return FALSE;
}
}

/**
 * Determine output mode from downstream caps in the allocation query.
 *
 * Handles both GStreamer >= 1.24 DMA_DRM caps (format=DMA_DRM with
 * drm-format field) and legacy caps (format=NV12/RGBA with
 * memory:DMABuf feature).
 *
 * @param glav  The plugin instance.
 * @param query The allocation query from downstream.
 * @return The selected output mode.
 */
static GstDmaBufOutputMode
gst_gl_base_audio_visualizer_detect_mode(GstGLBaseAudioVisualizer *glav,
                                          GstQuery *query) {
  GstCaps *caps = NULL;
  GstStructure *s;
  const gchar *format_str;
  GstCapsFeatures *features;

  gst_query_parse_allocation(query, &caps, NULL);
  if (!caps) {
    GST_DEBUG_OBJECT(glav, "No caps in allocation query, using GLMemory");
    return GST_DMABUF_MODE_GLMEMORY_RGBA;
  }

  s = gst_caps_get_structure(caps, 0);
  features = gst_caps_get_features(caps, 0);

  /* Check if caps have DMABuf memory feature */
  if (features && gst_caps_features_contains(features, "memory:DMABuf")) {
    format_str = gst_structure_get_string(s, "format");

    /*
     * GStreamer >= 1.24 DMABuf protocol: format=DMA_DRM with drm-format
     * field containing "NV12", "NV12:0x01...", "AR24", etc.
     */
    if (format_str && g_strcmp0(format_str, "DMA_DRM") == 0) {
      const gchar *drm_fmt = gst_structure_get_string(s, "drm-format");
      if (drm_fmt && g_str_has_prefix(drm_fmt, "NV12")) {
        GST_INFO_OBJECT(glav,
            "Downstream requests DMABuf DMA_DRM NV12 (drm-format=%s)",
            drm_fmt);
        return GST_DMABUF_MODE_DMABUF_NV12;
      }
      GST_INFO_OBJECT(glav,
          "Downstream requests DMABuf DMA_DRM RGBA (drm-format=%s)",
          drm_fmt ? drm_fmt : "(null)");
      return GST_DMABUF_MODE_DMABUF_RGBA;
    }

    /* Legacy DMABuf caps with standard video format names */
    if (format_str && g_strcmp0(format_str, "NV12") == 0) {
      GST_INFO_OBJECT(glav, "Downstream requests DMABuf NV12 (legacy)");
      return GST_DMABUF_MODE_DMABUF_NV12;
    }

    GST_INFO_OBJECT(glav, "Downstream requests DMABuf RGBA (format=%s)",
                    format_str ? format_str : "(null)");
    return GST_DMABUF_MODE_DMABUF_RGBA;
  }

  /* Check for GLMemory feature — this is the standard GL path */
  if (features && gst_caps_features_contains(features, "memory:GLMemory")) {
    GST_DEBUG_OBJECT(glav, "Downstream requests GLMemory, using GLMemory path");
    return GST_DMABUF_MODE_GLMEMORY_RGBA;
  }

  GST_DEBUG_OBJECT(glav, "Downstream does not request DMABuf, using GLMemory");
  return GST_DMABUF_MODE_GLMEMORY_RGBA;
}

/**
 * Build DMABuf caps using the GStreamer >= 1.24 DMA_DRM protocol.
 *
 * Produces caps of the form:
 *   video/x-raw(memory:DMABuf), format=DMA_DRM, drm-format=NV12, ...
 *
 * Per the GStreamer DMABuf design document:
 * - Linear modifier (0x0): drm-format is just the fourcc, e.g. "NV12"
 * - Non-linear modifier:   drm-format includes suffix, e.g. "NV12:0x01..."
 *
 * @param mode     The DMABuf mode (NV12 or RGBA).
 * @param modifier The DRM modifier from GBM allocation.
 * @param width    Video width.
 * @param height   Video height.
 * @param fps_n    FPS numerator.
 * @param fps_d    FPS denominator.
 *
 * @return New caps, or NULL on error.  Caller owns the reference.
 */
static GstCaps *
gst_gl_base_audio_visualizer_build_dmabuf_caps(GstDmaBufOutputMode mode,
                                                guint64 modifier,
                                                gint width, gint height,
                                                gint fps_n, gint fps_d) {
  GstCaps *caps;
  const gchar *drm_fourcc;
  gchar *drm_format_str;

  if (mode == GST_DMABUF_MODE_DMABUF_NV12) {
    drm_fourcc = "NV12";
  } else {
    drm_fourcc = "AR24"; /* DRM_FORMAT_ARGB8888 */
  }

  /* Linear modifier (0) omits the suffix per the DMABuf spec */
  if (modifier != 0) {
    drm_format_str = g_strdup_printf(
        "%s:0x%016" G_GINT64_MODIFIER "x", drm_fourcc, modifier);
  } else {
    drm_format_str = g_strdup(drm_fourcc);
  }

  caps = gst_caps_new_simple("video/x-raw",
      "format", G_TYPE_STRING, "DMA_DRM",
      "drm-format", G_TYPE_STRING, drm_format_str,
      "width", G_TYPE_INT, width,
      "height", G_TYPE_INT, height,
      "framerate", GST_TYPE_FRACTION, fps_n, fps_d,
      NULL);

  gst_caps_set_features(caps, 0,
      gst_caps_features_new("memory:DMABuf", NULL));

  GST_DEBUG("Built DMABuf caps: %" GST_PTR_FORMAT, caps);

  g_free(drm_format_str);
  return caps;
}

/**
 * DMABuf decide_allocation implementation.
 *
 * For NV12: Accept the downstream pool (e.g. GstVaPool from vah264enc).
 *   The encoder allocates tiled NV12 surfaces.  We import the per-plane
 *   DMABufs as R8/GR88 EGLImages, render RGBA → NV12 via shader, and
 *   push the same buffer back.  True zero-copy.
 *
 * For RGBA: Use our own GBM pool (existing path).
 *
 * @return TRUE on success, FALSE to fall back to GLMemory.
 */
static gboolean
gst_gl_base_audio_visualizer_decide_allocation_dmabuf(
    GstGLBaseAudioVisualizer *glav,
    GstQuery *query,
    GstGLContext *context,
    GstPMAudioVisualizer *pmav) {
  GstDmaBufOutputMode mode;
  guint width, height;

  mode = gst_gl_base_audio_visualizer_detect_mode(glav, query);

  if (mode == GST_DMABUF_MODE_GLMEMORY_RGBA) {
    glav->priv->dmabuf_mode = GST_DMABUF_MODE_GLMEMORY_RGBA;
    return FALSE;
  }

  /*
   * DMABuf import requires EGL (eglCreateImageKHR).
   * On GLX-only contexts (e.g. NVIDIA + X11), EGL functions are not
   * available and DMABuf import will crash.  Fall back to GLMemory.
   */
  {
    GstGLPlatform platform = gst_gl_context_get_gl_platform(context);
    if (!(platform & GST_GL_PLATFORM_EGL)) {
      GST_WARNING_OBJECT(glav,
          "DMABuf import requires EGL but GL context is %s (platform=0x%x). "
          "Falling back to GLMemory. "
          "Try: GST_GL_PLATFORM=egl or use gldownload ! videoconvert",
          platform == GST_GL_PLATFORM_GLX ? "GLX" : "non-EGL",
          platform);
      glav->priv->dmabuf_mode = GST_DMABUF_MODE_GLMEMORY_RGBA;
      return FALSE;
    }
  }

  width = GST_VIDEO_INFO_WIDTH(&pmav->vinfo);
  height = GST_VIDEO_INFO_HEIGHT(&pmav->vinfo);

  if (width == 0 || height == 0) {
    GST_ELEMENT_ERROR(glav, STREAM, FORMAT,
        ("Invalid video dimensions %ux%u", width, height), (NULL));
    return FALSE;
  }

  /* Parse DRM modifier from downstream caps drm-format field.
   * e.g. "NV12:0x0200000018601b04" → modifier 0x0200000018601b04
   * Plain "NV12" → modifier 0 (LINEAR). */
  {
    GstCaps *alloc_caps = NULL;
    guint64 caps_modifier = 0;
    gst_query_parse_allocation(query, &alloc_caps, NULL);
    if (alloc_caps) {
      GstStructure *s = gst_caps_get_structure(alloc_caps, 0);
      const gchar *drm_fmt = gst_structure_get_string(s, "drm-format");
      if (drm_fmt) {
        const gchar *colon = strchr(drm_fmt, ':');
        if (colon) {
          caps_modifier = g_ascii_strtoull(colon + 1, NULL, 16);
          GST_INFO_OBJECT(glav, "Parsed DRM modifier 0x%" G_GINT64_MODIFIER
                          "x from drm-format=%s", caps_modifier, drm_fmt);
        }
      }
    }
    glav->priv->drm_modifier = caps_modifier;
    glav->priv->drm_modifier_valid = TRUE;
  }

  if (mode == GST_DMABUF_MODE_DMABUF_NV12) {
    /*
     * NV12: Accept downstream pool as-is.
     *
     * The downstream encoder put its pool in the allocation query.
     * We do NOT create our own pool.  We leave the query untouched
     * so prepare_output_buffer() acquires from the downstream pool.
     */
    if (gst_query_get_n_allocation_pools(query) < 1) {
      GST_ELEMENT_ERROR(glav, STREAM, FORMAT,
          ("NV12 DMABuf requires a downstream pool, but none provided."),
          ("Try: projectm ! gldownload ! videoconvert ! vah264enc"));
      return FALSE;
    }

    if (glav->priv->dmabuf_pool) {
      gst_buffer_pool_set_active(
          GST_BUFFER_POOL(glav->priv->dmabuf_pool), FALSE);
      gst_object_unref(glav->priv->dmabuf_pool);
      glav->priv->dmabuf_pool = NULL;
    }

    glav->priv->dmabuf_mode = mode;
    glav->priv->use_downstream_pool = TRUE;

    GST_INFO_OBJECT(glav,
        "NV12 DMABuf: accepting downstream pool, %ux%u, "
        "modifier=0x%" G_GINT64_MODIFIER "x",
        width, height, glav->priv->drm_modifier);
    return TRUE;
  }

  /* ── RGBA: Use our own GBM pool (unchanged from baseline) ── */

  if (!gst_gl_base_audio_visualizer_init_gbm(glav)) {
    GST_ELEMENT_ERROR(glav, RESOURCE, NOT_FOUND,
        ("GBM device init failed (no /dev/dri/renderD* accessible?)"),
        ("Cannot create DMABuf buffers without GBM device"));
    return FALSE;
  }

  if (glav->priv->dmabuf_pool) {
    gst_buffer_pool_set_active(
        GST_BUFFER_POOL(glav->priv->dmabuf_pool), FALSE);
    gst_object_unref(glav->priv->dmabuf_pool);
    glav->priv->dmabuf_pool = NULL;
  }

  glav->priv->dmabuf_pool = gst_dmabuf_pool_new(
      glav->priv->gbm_device, mode, width, height,
      glav->priv->drm_modifier);

  if (!glav->priv->dmabuf_pool) {
    GST_ELEMENT_ERROR(glav, RESOURCE, NO_SPACE_LEFT,
        ("Failed to create RGBA DMABuf pool (%ux%u)", width, height), (NULL));
    return FALSE;
  }

  {
    GstCaps *pool_caps;
    GstBufferPool *bpool = GST_BUFFER_POOL(glav->priv->dmabuf_pool);
    GstStructure *config;
    GstVideoInfo pool_vinfo;

    gst_video_info_set_format(&pool_vinfo, GST_VIDEO_FORMAT_BGRA, width, height);
    GST_VIDEO_INFO_FPS_N(&pool_vinfo) = GST_VIDEO_INFO_FPS_N(&pmav->vinfo);
    GST_VIDEO_INFO_FPS_D(&pool_vinfo) = GST_VIDEO_INFO_FPS_D(&pmav->vinfo);
    pool_caps = gst_video_info_to_caps(&pool_vinfo);
    gst_caps_set_features(pool_caps, 0,
        gst_caps_features_new("memory:DMABuf", NULL));

    config = gst_buffer_pool_get_config(bpool);
    gst_buffer_pool_config_set_params(config, pool_caps, 0, 0, 0);
    gst_buffer_pool_config_add_option(config, GST_BUFFER_POOL_OPTION_VIDEO_META);

    if (!gst_buffer_pool_set_config(bpool, config)) {
      gst_caps_unref(pool_caps);
      gst_object_unref(glav->priv->dmabuf_pool);
      glav->priv->dmabuf_pool = NULL;
      return FALSE;
    }
    gst_caps_unref(pool_caps);

    /* Probe allocation */
    GstBuffer *probe = NULL;
    if (!gst_buffer_pool_set_active(bpool, TRUE) ||
        gst_buffer_pool_acquire_buffer(bpool, &probe, NULL) != GST_FLOW_OK ||
        probe == NULL) {
      GST_WARNING_OBJECT(glav, "RGBA GBM probe failed");
      gst_buffer_pool_set_active(bpool, FALSE);
      gst_object_unref(glav->priv->dmabuf_pool);
      glav->priv->dmabuf_pool = NULL;
      return FALSE;
    }
    glav->priv->drm_modifier =
        gst_dmabuf_pool_get_modifier(glav->priv->dmabuf_pool);
    glav->priv->drm_modifier_valid = TRUE;
    gst_buffer_unref(probe);
    gst_buffer_pool_set_active(bpool, FALSE);
  }

  glav->priv->dmabuf_mode = mode;
  glav->priv->use_downstream_pool = FALSE;

  if (gst_query_get_n_allocation_pools(query) > 0)
    gst_query_set_nth_allocation_pool(query, 0,
        GST_BUFFER_POOL(glav->priv->dmabuf_pool), 0, 0, 0);
  else
    gst_query_add_allocation_pool(query,
        GST_BUFFER_POOL(glav->priv->dmabuf_pool), 0, 0, 0);

  GST_INFO_OBJECT(glav, "RGBA DMABuf: GBM pool, %ux%u, "
                  "modifier=0x%" G_GINT64_MODIFIER "x",
                  width, height, glav->priv->drm_modifier);
  return TRUE;
}

#endif /* HAVE_DMABUF */

static gboolean gst_gl_base_audio_visualizer_parent_decide_allocation(
    GstPMAudioVisualizer *pmav, GstQuery *query) {
  GstGLBaseAudioVisualizer *glav = GST_GL_BASE_AUDIO_VISUALIZER(pmav);
  GstGLContext *context;
  GstBufferPool *pool = NULL;
  GstStructure *config;
  GstCaps *caps;
  guint min, max, size;
  gboolean update_pool;

  g_rec_mutex_lock(&glav->priv->context_lock);
  if (!gst_gl_base_audio_visualizer_find_gl_context_unlocked(glav)) {
    g_rec_mutex_unlock(&glav->priv->context_lock);
    return FALSE;
  }
  context = gst_object_ref(glav->context);
  g_rec_mutex_unlock(&glav->priv->context_lock);

#ifdef HAVE_DMABUF
  /* Try DMABuf allocation first; falls back to GLMemory if not applicable */
  if (gst_gl_base_audio_visualizer_decide_allocation_dmabuf(
          glav, query, context, pmav)) {
    gst_object_unref(context);
    GST_INFO_OBJECT(glav, "Using DMABuf output mode %d",
                    glav->priv->dmabuf_mode);
    return TRUE;
  }
  /* If we get here, use the standard GLMemory path below */
  GST_DEBUG_OBJECT(glav, "DMABuf not selected or failed, using GLMemory");
#endif

  gst_query_parse_allocation(query, &caps, NULL);

  if (gst_query_get_n_allocation_pools(query) > 0) {
    gst_query_parse_nth_allocation_pool(query, 0, &pool, &size, &min, &max);

    update_pool = TRUE;
  } else {
    GstVideoInfo vinfo;

    gst_video_info_init(&vinfo);
    /* gst_video_info_from_caps cannot parse DMA_DRM caps.  Use the
       already-parsed vinfo from src_setcaps instead. */
    if (!gst_video_info_from_caps(&vinfo, caps)) {
      vinfo = pmav->vinfo;
    }
    size = vinfo.size;
    min = 0;
    max = 0;
    update_pool = FALSE;
  }

  if (!pool || !GST_IS_GL_BUFFER_POOL(pool)) {
    /* can't use this pool */
    if (pool)
      gst_object_unref(pool);
    pool = gst_gl_buffer_pool_new(context);
  }
  config = gst_buffer_pool_get_config(pool);

  // there should be at least 2 textures, so that one is rendered while the
  // other one is pushed downstream
  // todo: pool size config properties needed ?
  if (min < 2) {
    min = 2;
  }
  gst_buffer_pool_config_set_params(config, caps, size, min, max);
  gst_buffer_pool_config_add_option(config, GST_BUFFER_POOL_OPTION_VIDEO_META);
  if (gst_query_find_allocation_meta(query, GST_GL_SYNC_META_API_TYPE, NULL))
    gst_buffer_pool_config_add_option(config,
                                      GST_BUFFER_POOL_OPTION_GL_SYNC_META);
  gst_buffer_pool_config_add_option(
      config, GST_BUFFER_POOL_OPTION_VIDEO_GL_TEXTURE_UPLOAD_META);

  gst_buffer_pool_config_add_option(
      config, GST_BUFFER_POOL_OPTION_GL_TEXTURE_TARGET_2D);

  gst_buffer_pool_set_config(pool, config);

  if (update_pool)
    gst_query_set_nth_allocation_pool(query, 0, pool, size, min, max);
  else
    gst_query_add_allocation_pool(query, pool, size, min, max);

  gst_object_unref(pool);
  gst_object_unref(context);

  return TRUE;
}

static GstPipeline *get_pipeline(GstElement *element) {
  GstObject *parent = GST_OBJECT(element);

  while (parent) {
    if (GST_IS_PIPELINE(parent))
      return GST_PIPELINE(parent);

    GstObject *next = gst_object_get_parent(parent);

    // we increase ref with get_parent, so unref previous level
    if (parent != GST_OBJECT(element))
      gst_object_unref(parent);

    parent = next;
  }

  return NULL; // no pipeline found
}

static GstStateChangeReturn
gst_gl_base_audio_visualizer_change_state(GstElement *element,
                                          GstStateChange transition) {
  GstGLBaseAudioVisualizer *glav = GST_GL_BASE_AUDIO_VISUALIZER(element);
  GstStateChangeReturn ret = GST_STATE_CHANGE_SUCCESS;

  GST_DEBUG_OBJECT(
      glav, "changing state: %s => %s",
      gst_element_state_get_name(GST_STATE_TRANSITION_CURRENT(transition)),
      gst_element_state_get_name(GST_STATE_TRANSITION_NEXT(transition)));

  switch (transition) {
  case GST_STATE_CHANGE_READY_TO_NULL:
    g_rec_mutex_lock(&glav->priv->context_lock);
    gst_clear_object(&glav->priv->other_context);
    gst_clear_object(&glav->display);
    g_rec_mutex_unlock(&glav->priv->context_lock);
    break;

  default:
    break;
  }

  return ret;
}

GstGLBaseAudioVisualizerMode
gst_gl_base_audio_visualizer_mode_from_string(const gchar *str) {
  if (str != NULL) {
    if (strcasecmp("true", str) == 0) {
      return GST_GL_BASE_AUDIO_VISUALIZER_REALTIME;
    } else if (strcasecmp("false", str) == 0) {
      return GST_GL_BASE_AUDIO_VISUALIZER_OFFLINE;
    }
  }
  return GST_GL_BASE_AUDIO_VISUALIZER_AUTO;
}

const gchar *
gst_gl_base_audio_visualizer_mode_to_string(GstGLBaseAudioVisualizerMode mode) {
  switch (mode) {
  case GST_GL_BASE_AUDIO_VISUALIZER_REALTIME:
    return "true";
  case GST_GL_BASE_AUDIO_VISUALIZER_OFFLINE:
    return "false";
  default:
    return "auto";
  }
}
