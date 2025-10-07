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

#include <GL/gl.h>
#include <gst/gl/gl.h>

#include "gstglbaseaudiovisualizer.h"
#include "gstpmaudiovisualizer.h"
#include "renderbuffer.h"

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
 * for initializing and cleaning up OpenGL resources. The `render`
 * virtual method of the GstPMAudioVisualizer is implemented to perform OpenGL
 * rendering. The implementer provides an implementation for fill_gl_memory to
 * render directly to gl memory.
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
 * Allow 0.75 * fps frame duration as wait time for frame render queuing.
 */
#ifndef MAX_RENDER_QUEUE_WAIT_TIME_IN_FRAME_DURATRIONS_N
#define MAX_RENDER_QUEUE_WAIT_TIME_IN_FRAME_DURATRIONS_N 3
#define MAX_RENDER_QUEUE_WAIT_TIME_IN_FRAME_DURATRIONS_D 4
#endif

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

static void gst_gl_base_audio_visualizer_finalize(GObject *object);
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
static GstFlowReturn gst_gl_base_audio_visualizer_parent_render(
    GstPMAudioVisualizer *bscope, GstBuffer *audio, GstClockTime pts,
    GstClockTime running_time, guint64 frame_duration);

/**
 * Internal utility for resetting state on start \
 */
static void gst_gl_base_audio_visualizer_start(GstGLBaseAudioVisualizer *glav);

/**
 * Internal utility for cleaning up gl context on stop
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

  gobject_class->finalize = gst_gl_base_audio_visualizer_finalize;
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
  glav->priv->is_realtime = FALSE;
  glav->context = NULL;

  glav->min_fps_n = DEFAULT_MIN_FPS_N;
  glav->min_fps_d = DEFAULT_MIN_FPS_D;

  g_rec_mutex_init(&glav->priv->context_lock);

  gst_gl_base_audio_visualizer_start(glav);
}

static void gst_gl_base_audio_visualizer_finalize(GObject *object) {
  GstGLBaseAudioVisualizer *glav = GST_GL_BASE_AUDIO_VISUALIZER(object);
  gst_gl_base_audio_visualizer_stop(glav);

  g_rec_mutex_clear(&glav->priv->context_lock);

  G_OBJECT_CLASS(parent_class)->finalize(object);
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

  rb_init_render_buffer(&glav->priv->render_buffer, GST_OBJECT(glav),
                        gst_gl_base_audio_visualizer_fill_gl,
                        adjust_fps_callback, max_frame_duration,
                        caps_frame_duration, glav->priv->is_realtime);

  // cascade gl start to implementor
  glav->priv->gl_started = glav_class->gl_start(glav);

  // get gl rendering going
  rb_start_render_thread(&glav->priv->render_buffer, glav->context,
                         pmav->srcpad);
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
  rb_stop_render_thread(&glav->priv->render_buffer);

  // clean up implementor
  if (glav->priv->gl_started) {
    glav_class->gl_stop(glav);
  }

  glav->priv->gl_started = FALSE;

  // clean up render buffer
  rb_dispose_render_buffer(&glav->priv->render_buffer);

  // clean up state
  if (glav->priv->fbo) {
    gst_object_unref(glav->priv->fbo);
  }
}

static gboolean gst_gl_base_audio_visualizer_default_fill_gl_memory(
    GstAVRenderParams *render_data) {
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

  GstBuffer *out_buf;
  GstVideoFrame out_video;

  // obtain output buffer from the (GL texture backed) pool
  gst_pm_audio_visualizer_util_prepare_output_buffer(pmav, &out_buf);

  // Check for GL sync meta
  GstGLSyncMeta *sync_meta = gst_buffer_get_gl_sync_meta(out_buf);

  if (sync_meta) {
    // wait until GPU is done using this buffer should not be needed
    // gst_gl_sync_meta_wait(sync_meta, glav->context);
  }

  // GstClockTime after_prepare = gst_util_get_timestamp();

  // map output video frame to buffer outbuf with gl flags
  gst_video_frame_map(&out_video, &pmav->vinfo, out_buf,
                      GST_MAP_WRITE | GST_MAP_GL |
                          GST_VIDEO_FRAME_MAP_FLAG_NO_REF);

  // GstClockTime after_map = gst_util_get_timestamp();
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
  out_buf = NULL;
}

static GstFlowReturn gst_gl_base_audio_visualizer_fill(
    GstPMAudioVisualizer *bscope, GstGLBaseAudioVisualizer *glav,
    GstBuffer *audio, GstClockTime pts, GstClockTime running_time,
    guint64 frame_duration) {

  g_rec_mutex_lock(&glav->priv->context_lock);
  if (G_UNLIKELY(!glav->context))
    goto not_negotiated;

  /* 0 framerate and we are at the second frame, eos */
  if (G_UNLIKELY(GST_VIDEO_INFO_FPS_N(&bscope->vinfo) == 0 &&
                 glav->priv->n_frames == 1))
    goto eos;

  // prepare args for queuing frame rendering
  RBQueueArgs args;
  args.render_buffer = &glav->priv->render_buffer;
  args.in_audio = audio;
  args.pts = pts;
  args.frame_duration = frame_duration;
  args.latency = bscope->latency;
  args.running_time = running_time;

  if (glav->priv->is_realtime == FALSE) {
    // wait for each frame to complete
    args.sync_rendering = TRUE;

    // unlimited for offline rendering, frames will never be dropped by QoS.
    args.max_wait = GST_CLOCK_TIME_NONE;
  } else {
    // fire and forget, mapping n samples per frame from upstream keeps us in
    // sync
    args.sync_rendering = FALSE;

    // limit wait based on fps factor, make sure we never wait too long in order
    // to keep in sync
    args.max_wait = (GstClockTimeDiff)gst_util_uint64_scale_int(
        frame_duration, MAX_RENDER_QUEUE_WAIT_TIME_IN_FRAME_DURATRIONS_N,
        MAX_RENDER_QUEUE_WAIT_TIME_IN_FRAME_DURATRIONS_D);
  }

  // dispatch gst_gl_base_audio_visualizer_fill_gl to the gl render buffer,
  // rendering is deferred. This may block for a while though.
  rb_queue_render_job_warn(&args);

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

/**
 * Find out if the pipeline is using a real-time clock.
 *
 * @param element GST element
 * @return TRUE in case the element uses a system clock.
 */
static gboolean is_pipeline_realtime(GstElement *element) {
  GstClock *clock = gst_element_get_clock(element);
  gboolean is_realtime = FALSE;

  if (clock) {
    // Compare to the system clock (used for real-time playback)
    is_realtime = GST_IS_SYSTEM_CLOCK(clock);
    gst_object_unref(clock);
  }

  return is_realtime;
}

static gboolean
gst_gl_base_audio_visualizer_parent_setup(GstPMAudioVisualizer *pmav) {
  GstGLBaseAudioVisualizer *glav = GST_GL_BASE_AUDIO_VISUALIZER(pmav);
  GstGLBaseAudioVisualizerClass *glav_class =
      GST_GL_BASE_AUDIO_VISUALIZER_GET_CLASS(pmav);

  // configure QoS for the pipeline, disabled for offline rendering
  g_rec_mutex_lock(&glav->priv->context_lock);

  gboolean is_realtime = is_pipeline_realtime(GST_ELEMENT(pmav));
  glav->priv->is_realtime = is_realtime;

  g_rec_mutex_unlock(&glav->priv->context_lock);

  // update render buffer config
  rb_set_qos_enabled(&glav->priv->render_buffer, is_realtime);

  GstClockTime caps_frame_duration =
      gst_util_uint64_scale_int(GST_SECOND, GST_VIDEO_INFO_FPS_D(&pmav->vinfo),
                                GST_VIDEO_INFO_FPS_N(&pmav->vinfo));
  rb_set_caps_frame_duration(&glav->priv->render_buffer, caps_frame_duration);

  GST_INFO_OBJECT(
      glav,
      "GL setup - render config: real-time: %s, caps-frame-duration: "
      "%" GST_TIME_FORMAT
      ", min-fps: %d/%d, min-fps-duration: %" GST_TIME_FORMAT,
      is_realtime ? "true" : "false", GST_TIME_ARGS(caps_frame_duration),
      glav->min_fps_n, glav->min_fps_d,
      GST_TIME_ARGS(glav->priv->render_buffer.max_frame_duration));

  // cascade setup to the derived plugin after gl initialization has been
  // completed
  return glav_class->setup(glav);
}

static GstFlowReturn gst_gl_base_audio_visualizer_parent_render(
    GstPMAudioVisualizer *bscope, GstBuffer *audio, GstClockTime pts,
    GstClockTime running_time, guint64 frame_duration) {
  GstGLBaseAudioVisualizer *glav = GST_GL_BASE_AUDIO_VISUALIZER(bscope);

  return gst_gl_base_audio_visualizer_fill(bscope, glav, audio, pts,
                                           running_time, frame_duration);
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

  gst_query_parse_allocation(query, &caps, NULL);

  if (gst_query_get_n_allocation_pools(query) > 0) {
    gst_query_parse_nth_allocation_pool(query, 0, &pool, &size, &min, &max);

    update_pool = TRUE;
  } else {
    GstVideoInfo vinfo;

    gst_video_info_init(&vinfo);
    gst_video_info_from_caps(&vinfo, caps);
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
