/* GStreamer
 * Copyright (C) <2011> Stefan Kost <ensonic@users.sf.net>
 * Copyright (C) <2015> Luis de Bethencourt <luis@debethencourt.com>
 *
 * gstaudiovisualizer.h: base class for audio visualisation elements
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
/**
 * SECTION:gstaudiovisualizer
 * @title: GstPMAudioVisualizer
 * @short_description: Base class for visualizers.
 *
 * A baseclass for scopes (visualizers). It takes care of re-fitting the
 * audio-rate to video-rate and handles renegotiation (downstream video size
 * changes).
 *
 * It also provides several background shading effects. These effects are
 * applied to a previous picture before the `render()` implementation can draw a
 * new frame.
 */

/*
 * The code in this file is based on
 * GStreamer / gst-plugins-base, latest version as of 2025/05/29.
 * gst-libs/gst/pbutils/gstaudiovisualizer.c Git Repository:
 * https://gitlab.freedesktop.org/gstreamer/gstreamer/-/blob/main/subprojects/gst-plugins-base/gst-libs/gst/pbutils/gstaudiovisualizer.c
 * Original copyright notice has been retained at the top of this file.
 *
 * The code has been modified to improve compatibility with projectM and OpenGL.
 *
 * - Main memory based video frame buffers have been removed.
 *
 * - Cpu based transition shaders have been removed.
 *
 * - Bugfix for the amount of bytes being flushed for a single video frame from
 * the audio input buffer.
 *
 * - Uses a sample count based approach for pts/dts timestamps instead
 * GstAdapter derived timestamps.
 *
 * - Consistent locking and fixes for some race conditions.
 *
 * - Allow dynamic fps adjustments while staying sample accurate.
 *
 * - Segment event propagation.
 *
 * - Memory management and rendering is implementer-provided.
 *
 *  Typical plug-in call order for implementer-provided functions:
 *  - decide_allocation (once)
 *  - setup (when caps change, typically once)
 *  - render (once for each frame)
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <string.h>

#include <gst/pbutils/pbutils-enumtypes.h>
#include <gst/video/video.h>

#include "gstpmaudiovisualizer.h"

GST_DEBUG_CATEGORY_STATIC(pm_audio_visualizer_debug);
#define GST_CAT_DEFAULT (pm_audio_visualizer_debug)

/**
 * Ignore QoS events during the first couple of frames that can cause a start
 * delay.
 */
#ifndef QOS_IGNORE_FIRST_N_FRAMES
#define QOS_IGNORE_FIRST_N_FRAMES 5
#endif

/**
 * Min latency change required to push a latency event
 * upstream. The latency is compared to last published latency.
 */
#ifndef LATENCY_EVENT_MIN_CHANGE
#define LATENCY_EVENT_MIN_CHANGE GST_MSECOND
#endif

enum { PROP_0 };

static GstBaseTransformClass *parent_class = NULL;
static gint private_offset = 0;

static void
gst_pm_audio_visualizer_class_init(GstPMAudioVisualizerClass *klass);
static void gst_pm_audio_visualizer_init(GstPMAudioVisualizer *scope,
                                         GstPMAudioVisualizerClass *g_class);
static void gst_pm_audio_visualizer_set_property(GObject *object, guint prop_id,
                                                 const GValue *value,
                                                 GParamSpec *pspec);
static void gst_pm_audio_visualizer_get_property(GObject *object, guint prop_id,
                                                 GValue *value,
                                                 GParamSpec *pspec);
static void gst_pm_audio_visualizer_dispose(GObject *object);

static gboolean
gst_pm_audio_visualizer_src_negotiate(GstPMAudioVisualizer *scope);
static gboolean gst_pm_audio_visualizer_src_setcaps(GstPMAudioVisualizer *scope,
                                                    GstCaps *caps);
static gboolean
gst_pm_audio_visualizer_sink_setcaps(GstPMAudioVisualizer *scope,
                                     GstCaps *caps);

static GstFlowReturn gst_pm_audio_visualizer_chain(GstPad *pad,
                                                   GstObject *parent,
                                                   GstBuffer *buffer);

static gboolean gst_pm_audio_visualizer_src_event(GstPad *pad,
                                                  GstObject *parent,
                                                  GstEvent *event);
static gboolean gst_pm_audio_visualizer_sink_event(GstPad *pad,
                                                   GstObject *parent,
                                                   GstEvent *event);

static gboolean gst_pm_audio_visualizer_src_query(GstPad *pad,
                                                  GstObject *parent,
                                                  GstQuery *query);

static GstStateChangeReturn
gst_pm_audio_visualizer_parent_change_state(GstElement *element,
                                            GstStateChange transition);

static GstStateChangeReturn
gst_pm_audio_visualizer_default_change_state(GstElement *element,
                                             GstStateChange transition);

static gboolean
gst_pm_audio_visualizer_do_bufferpool(GstPMAudioVisualizer *scope,
                                      GstCaps *outcaps);

static gboolean
gst_pm_audio_visualizer_default_decide_allocation(GstPMAudioVisualizer *scope,
                                                  GstQuery *query);

static void gst_pm_audio_visualizer_send_latency_if_needed_unlocked(
    GstPMAudioVisualizer *scope);

struct _GstPMAudioVisualizerPrivate {
  gboolean negotiated;

  GstBufferPool *pool;
  gboolean pool_active;
  GstAllocator *allocator;
  GstAllocationParams params;
  GstQuery *query;

  /* pads */
  GstPad *sinkpad;

  GstAdapter *adapter;

  GstBuffer *inbuf;

  guint spf; /* samples per video frame */

  /* QoS stuff */ /* with LOCK */
  gdouble proportion;
  /* qos: earliest time to render the next frame, the render loop will skip
   * frames until this time */
  GstClockTime earliest_time;

  guint dropped; /* frames dropped / not dropped */
  guint processed;

  /* samples consumed, relative to the current segment. Basis for timestamps. */
  guint64 samples_consumed;

  /* configuration mutex */
  GMutex config_lock;

  GstSegment segment;

  /* ready flag and condition triggered once the plugin is ready to process
   * buffers, triggers every time a caps event is processed */
  GCond ready_cond;
  gboolean ready;

  /* have src caps been setup */
  gboolean src_ready;

  /* have sink caps been setup */
  gboolean sink_ready;

  /* clock timestamp pts offset, either from first audio buffer pts or segment
   * event */
  gboolean pts_offset_initialized;
  GstClockTime pts_offset;
  GstClockTime caps_frame_duration;
  GstClockTime last_reported_latency;
  gboolean fps_changed;
};

/* base class */

GType gst_pm_audio_visualizer_get_type(void) {
  static gsize audio_visualizer_type = 0;

  if (g_once_init_enter(&audio_visualizer_type)) {
    static const GTypeInfo audio_visualizer_info = {
        sizeof(GstPMAudioVisualizerClass),
        NULL,
        NULL,
        (GClassInitFunc)gst_pm_audio_visualizer_class_init,
        NULL,
        NULL,
        sizeof(GstPMAudioVisualizer),
        0,
        (GInstanceInitFunc)gst_pm_audio_visualizer_init,
    };
    GType _type;

    /* TODO: rename when exporting it as a library */
    _type =
        g_type_register_static(GST_TYPE_ELEMENT, "GstPMAudioVisualizer",
                               &audio_visualizer_info, G_TYPE_FLAG_ABSTRACT);

    private_offset =
        g_type_add_instance_private(_type, sizeof(GstPMAudioVisualizerPrivate));

    g_once_init_leave(&audio_visualizer_type, _type);
  }
  return (GType)audio_visualizer_type;
}

static inline GstPMAudioVisualizerPrivate *
gst_pm_audio_visualizer_get_instance_private(GstPMAudioVisualizer *self) {
  return (G_STRUCT_MEMBER_P(self, private_offset));
}

static void
gst_pm_audio_visualizer_class_init(GstPMAudioVisualizerClass *klass) {
  GObjectClass *gobject_class = (GObjectClass *)klass;
  GstElementClass *element_class = (GstElementClass *)klass;

  if (private_offset != 0)
    g_type_class_adjust_private_offset(klass, &private_offset);

  parent_class = g_type_class_peek_parent(klass);

  GST_DEBUG_CATEGORY_INIT(pm_audio_visualizer_debug, "pmaudiovisualizer", 0,
                          "projectm audio visualisation base class");

  gobject_class->set_property = gst_pm_audio_visualizer_set_property;
  gobject_class->get_property = gst_pm_audio_visualizer_get_property;
  gobject_class->dispose = gst_pm_audio_visualizer_dispose;

  element_class->change_state =
      GST_DEBUG_FUNCPTR(gst_pm_audio_visualizer_parent_change_state);

  klass->change_state =
      GST_DEBUG_FUNCPTR(gst_pm_audio_visualizer_default_change_state);

  klass->decide_allocation =
      GST_DEBUG_FUNCPTR(gst_pm_audio_visualizer_default_decide_allocation);

  klass->segment_change = NULL;
}

static void gst_pm_audio_visualizer_init(GstPMAudioVisualizer *scope,
                                         GstPMAudioVisualizerClass *g_class) {
  GstPadTemplate *pad_template;

  scope->priv = gst_pm_audio_visualizer_get_instance_private(scope);

  /* create the sink pad */
  pad_template =
      gst_element_class_get_pad_template(GST_ELEMENT_CLASS(g_class), "sink");
  g_return_if_fail(pad_template != NULL);
  scope->priv->sinkpad = gst_pad_new_from_template(pad_template, "sink");
  gst_pad_set_chain_function(scope->priv->sinkpad,
                             GST_DEBUG_FUNCPTR(gst_pm_audio_visualizer_chain));
  gst_pad_set_event_function(
      scope->priv->sinkpad,
      GST_DEBUG_FUNCPTR(gst_pm_audio_visualizer_sink_event));
  gst_element_add_pad(GST_ELEMENT(scope), scope->priv->sinkpad);

  /* create the src pad */
  pad_template =
      gst_element_class_get_pad_template(GST_ELEMENT_CLASS(g_class), "src");
  g_return_if_fail(pad_template != NULL);
  scope->srcpad = gst_pad_new_from_template(pad_template, "src");
  gst_pad_set_event_function(
      scope->srcpad, GST_DEBUG_FUNCPTR(gst_pm_audio_visualizer_src_event));
  gst_pad_set_query_function(
      scope->srcpad, GST_DEBUG_FUNCPTR(gst_pm_audio_visualizer_src_query));
  gst_element_add_pad(GST_ELEMENT(scope), scope->srcpad);

  scope->priv->adapter = gst_adapter_new();
  scope->priv->inbuf = gst_buffer_new();
  g_cond_init(&scope->priv->ready_cond);

  scope->priv->dropped = 0;
  scope->priv->earliest_time = 0;
  scope->priv->processed = 0;
  scope->priv->samples_consumed = 0;
  scope->priv->src_ready = FALSE;
  scope->priv->sink_ready = FALSE;
  scope->priv->ready = FALSE;
  scope->priv->pts_offset_initialized = FALSE;
  scope->priv->pts_offset = GST_CLOCK_TIME_NONE;
  scope->priv->caps_frame_duration = 0;
  scope->priv->last_reported_latency = GST_CLOCK_TIME_NONE;
  scope->priv->fps_changed = FALSE;
  scope->latency = GST_CLOCK_TIME_NONE;

  /* properties */

  /* reset the initial video state */
  gst_video_info_init(&scope->vinfo);
  scope->req_frame_duration = GST_CLOCK_TIME_NONE;

  /* reset the initial state */
  gst_audio_info_init(&scope->ainfo);
  gst_video_info_init(&scope->vinfo);

  g_mutex_init(&scope->priv->config_lock);
}

static void gst_pm_audio_visualizer_set_property(GObject *object, guint prop_id,
                                                 const GValue *value,
                                                 GParamSpec *pspec) {
  GstPMAudioVisualizer *scope = GST_PM_AUDIO_VISUALIZER(object);

  switch (prop_id) {
  default:
    G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
    break;
  }
}

static void gst_pm_audio_visualizer_get_property(GObject *object, guint prop_id,
                                                 GValue *value,
                                                 GParamSpec *pspec) {
  GstPMAudioVisualizer *scope = GST_PM_AUDIO_VISUALIZER(object);

  switch (prop_id) {
  default:
    G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
    break;
  }
}

static void gst_pm_audio_visualizer_dispose(GObject *object) {
  GstPMAudioVisualizer *scope = GST_PM_AUDIO_VISUALIZER(object);

  if (scope->priv->adapter) {
    g_object_unref(scope->priv->adapter);
    scope->priv->adapter = NULL;
  }
  if (scope->priv->config_lock.p) {
    g_mutex_clear(&scope->priv->config_lock);
    scope->priv->config_lock.p = NULL;
  }
  if (scope->priv->ready_cond.p) {
    g_cond_clear(&scope->priv->ready_cond);
    scope->priv->ready_cond.p = NULL;
  }

  G_OBJECT_CLASS(parent_class)->dispose(object);
}

static void
gst_pm_audio_visualizer_reset_unlocked(GstPMAudioVisualizer *scope) {

  gst_adapter_clear(scope->priv->adapter);
  gst_segment_init(&scope->priv->segment, GST_FORMAT_UNDEFINED);

  scope->priv->proportion = 1.0;
  scope->priv->earliest_time = 0;
  scope->priv->dropped = 0;
  scope->priv->processed = 0;
  scope->priv->samples_consumed = 0;
  scope->priv->pts_offset_initialized = FALSE;
  scope->priv->pts_offset = GST_CLOCK_TIME_NONE;
  scope->latency = GST_CLOCK_TIME_NONE;
}

/* */
static gboolean gst_pm_audio_visualizer_do_setup(GstPMAudioVisualizer *scope) {

  GstPMAudioVisualizerClass *klass = GST_PM_AUDIO_VISUALIZER_GET_CLASS(scope);

  GST_OBJECT_LOCK(scope);
  scope->priv->earliest_time = 0;
  GST_OBJECT_UNLOCK(scope);

  g_mutex_lock(&scope->priv->config_lock);

  const guint spf = gst_util_uint64_scale_int(
      GST_AUDIO_INFO_RATE(&scope->ainfo), GST_VIDEO_INFO_FPS_D(&scope->vinfo),
      GST_VIDEO_INFO_FPS_N(&scope->vinfo));

  scope->req_spf = spf;
  scope->priv->spf = spf;

  g_mutex_unlock(&scope->priv->config_lock);

  if (klass->setup && !klass->setup(scope))
    return FALSE;

  GST_INFO_OBJECT(
      scope, "video: dimension %dx%d, framerate %d/%d",
      GST_VIDEO_INFO_WIDTH(&scope->vinfo), GST_VIDEO_INFO_HEIGHT(&scope->vinfo),
      GST_VIDEO_INFO_FPS_N(&scope->vinfo), GST_VIDEO_INFO_FPS_D(&scope->vinfo));

  GST_INFO_OBJECT(scope, "audio: rate %d, channels: %d, bpf: %d",
                  GST_AUDIO_INFO_RATE(&scope->ainfo),
                  GST_AUDIO_INFO_CHANNELS(&scope->ainfo),
                  GST_AUDIO_INFO_BPF(&scope->ainfo));

  GST_INFO_OBJECT(scope, "blocks: spf / req_spf %u", spf);

  g_mutex_lock(&scope->priv->config_lock);
  scope->priv->ready = TRUE;
  g_cond_broadcast(&scope->priv->ready_cond);
  g_mutex_unlock(&scope->priv->config_lock);

  return TRUE;
}

static gboolean
gst_pm_audio_visualizer_sink_setcaps(GstPMAudioVisualizer *scope,
                                     GstCaps *caps) {
  GstAudioInfo info;

  if (!gst_audio_info_from_caps(&info, caps))
    goto wrong_caps;

  g_mutex_lock(&scope->priv->config_lock);
  scope->ainfo = info;
  g_mutex_unlock(&scope->priv->config_lock);

  GST_DEBUG_OBJECT(scope, "audio: channels %d, rate %d",
                   GST_AUDIO_INFO_CHANNELS(&info), GST_AUDIO_INFO_RATE(&info));

  if (!gst_pm_audio_visualizer_src_negotiate(scope)) {
    goto not_negotiated;
  }

  g_mutex_lock(&scope->priv->config_lock);
  scope->priv->sink_ready = TRUE;
  g_mutex_unlock(&scope->priv->config_lock);

  if (scope->priv->src_ready) {
    gst_pm_audio_visualizer_do_setup(scope);
  }

  return TRUE;

  /* Errors */
wrong_caps: {
  GST_WARNING_OBJECT(scope, "could not parse caps");
  return FALSE;
}
not_negotiated: {
  GST_WARNING_OBJECT(scope, "failed to negotiate");
  return FALSE;
}
}

static gboolean gst_pm_audio_visualizer_src_setcaps(GstPMAudioVisualizer *scope,
                                                    GstCaps *caps) {
  GstVideoInfo info;
  gboolean res;

  if (!gst_video_info_from_caps(&info, caps))
    goto wrong_caps;

  g_mutex_lock(&scope->priv->config_lock);

  scope->vinfo = info;

  scope->priv->caps_frame_duration = gst_util_uint64_scale_int(
      GST_SECOND, GST_VIDEO_INFO_FPS_D(&info), GST_VIDEO_INFO_FPS_N(&info));

  scope->req_frame_duration = scope->priv->caps_frame_duration;
  g_mutex_unlock(&scope->priv->config_lock);

  gst_pad_set_caps(scope->srcpad, caps);

  /* find a pool for the negotiated caps now */
  res = gst_pm_audio_visualizer_do_bufferpool(scope, caps);
  gst_caps_unref(caps);

  g_mutex_lock(&scope->priv->config_lock);
  scope->priv->src_ready = TRUE;
  g_mutex_unlock(&scope->priv->config_lock);
  if (scope->priv->sink_ready) {
    if (!gst_pm_audio_visualizer_do_setup(scope)) {
      goto setup_failed;
    }
  }

  return res;

  /* ERRORS */
wrong_caps: {
  gst_caps_unref(caps);
  GST_DEBUG_OBJECT(scope, "error parsing caps");
  return FALSE;
}

setup_failed: {
  GST_WARNING_OBJECT(scope, "failed to set up");
  return FALSE;
}
}

static gboolean
gst_pm_audio_visualizer_src_negotiate(GstPMAudioVisualizer *scope) {
  GstCaps *othercaps, *target;
  GstStructure *structure;
  GstCaps *templ;
  gboolean ret;

  templ = gst_pad_get_pad_template_caps(scope->srcpad);

  GST_DEBUG_OBJECT(scope, "performing negotiation");

  /* see what the peer can do */
  othercaps = gst_pad_peer_query_caps(scope->srcpad, NULL);
  if (othercaps) {
    target = gst_caps_intersect(othercaps, templ);
    gst_caps_unref(othercaps);
    gst_caps_unref(templ);

    if (gst_caps_is_empty(target))
      goto no_format;

    target = gst_caps_truncate(target);
  } else {
    target = templ;
  }

  target = gst_caps_make_writable(target);
  structure = gst_caps_get_structure(target, 0);
  gst_structure_fixate_field_nearest_int(structure, "width", 320);
  gst_structure_fixate_field_nearest_int(structure, "height", 200);
  gst_structure_fixate_field_nearest_fraction(structure, "framerate", 25, 1);
  if (gst_structure_has_field(structure, "pixel-aspect-ratio"))
    gst_structure_fixate_field_nearest_fraction(structure, "pixel-aspect-ratio",
                                                1, 1);

  target = gst_caps_fixate(target);

  GST_DEBUG_OBJECT(scope, "final caps are %" GST_PTR_FORMAT, target);

  ret = gst_pm_audio_visualizer_src_setcaps(scope, target);

  return ret;

no_format: {
  gst_caps_unref(target);
  return FALSE;
}
}

/* takes ownership of the pool, allocator and query */
static gboolean gst_pm_audio_visualizer_set_allocation(
    GstPMAudioVisualizer *scope, GstBufferPool *pool, GstAllocator *allocator,
    const GstAllocationParams *params, GstQuery *query) {
  GstAllocator *oldalloc;
  GstBufferPool *oldpool;
  GstQuery *oldquery;
  GstPMAudioVisualizerPrivate *priv = scope->priv;

  GST_OBJECT_LOCK(scope);
  oldpool = priv->pool;
  priv->pool = pool;
  priv->pool_active = FALSE;

  oldalloc = priv->allocator;
  priv->allocator = allocator;

  oldquery = priv->query;
  priv->query = query;

  if (params)
    priv->params = *params;
  else
    gst_allocation_params_init(&priv->params);
  GST_OBJECT_UNLOCK(scope);

  if (oldpool) {
    GST_DEBUG_OBJECT(scope, "deactivating old pool %p", oldpool);
    gst_buffer_pool_set_active(oldpool, FALSE);
    gst_object_unref(oldpool);
  }
  if (oldalloc) {
    gst_object_unref(oldalloc);
  }
  if (oldquery) {
    gst_query_unref(oldquery);
  }
  return TRUE;
}

static gboolean
gst_pm_audio_visualizer_do_bufferpool(GstPMAudioVisualizer *scope,
                                      GstCaps *outcaps) {
  GstQuery *query;
  gboolean result = TRUE;
  GstBufferPool *pool = NULL;
  GstPMAudioVisualizerClass *klass;
  GstAllocator *allocator;
  GstAllocationParams params;

  /* not passthrough, we need to allocate */
  /* find a pool for the negotiated caps now */
  GST_DEBUG_OBJECT(scope, "doing allocation query");
  query = gst_query_new_allocation(outcaps, TRUE);

  if (!gst_pad_peer_query(scope->srcpad, query)) {
    /* not a problem, we use the query defaults */
    GST_DEBUG_OBJECT(scope, "allocation query failed");
  }

  klass = GST_PM_AUDIO_VISUALIZER_GET_CLASS(scope);

  GST_DEBUG_OBJECT(scope, "calling decide_allocation");
  g_assert(klass->decide_allocation != NULL);
  result = klass->decide_allocation(scope, query);

  GST_DEBUG_OBJECT(scope, "ALLOCATION (%d) params: %" GST_PTR_FORMAT, result,
                   query);

  if (!result)
    goto no_decide_allocation;

  /* we got configuration from our peer or the decide_allocation method,
   * parse them */
  if (gst_query_get_n_allocation_params(query) > 0) {
    gst_query_parse_nth_allocation_param(query, 0, &allocator, &params);
  } else {
    allocator = NULL;
    gst_allocation_params_init(&params);
  }

  if (gst_query_get_n_allocation_pools(query) > 0)
    gst_query_parse_nth_allocation_pool(query, 0, &pool, NULL, NULL, NULL);

  /* now store */
  result = gst_pm_audio_visualizer_set_allocation(scope, pool, allocator,
                                                  &params, query);

  return result;

  /* Errors */
no_decide_allocation: {
  GST_WARNING_OBJECT(scope, "Subclass failed to decide allocation");
  gst_query_unref(query);

  return result;
}
}

static gboolean
gst_pm_audio_visualizer_default_decide_allocation(GstPMAudioVisualizer *scope,
                                                  GstQuery *query) {
  /* removed main memory pool implementation. This vmethod is overridden for
   * using gl memory by gstglbaseaudiovisualizer. */
  g_error("vmethod gst_pm_audio_visualizer_default_decide_allocation is not "
          "implemented");
}

GstFlowReturn
gst_pm_audio_visualizer_util_prepare_output_buffer(GstPMAudioVisualizer *scope,
                                                   GstBuffer **outbuf) {
  GstPMAudioVisualizerPrivate *priv;

  priv = scope->priv;

  g_assert(priv->pool != NULL);

  /* we can't reuse the input buffer */
  if (!priv->pool_active) {
    GST_DEBUG_OBJECT(scope, "setting pool %p active", priv->pool);
    if (!gst_buffer_pool_set_active(priv->pool, TRUE))
      goto activate_failed;
    priv->pool_active = TRUE;
  }
  GST_TRACE_OBJECT(scope, "using pool alloc");

  return gst_buffer_pool_acquire_buffer(priv->pool, outbuf, NULL);

  /* ERRORS */
activate_failed: {
  GST_ELEMENT_ERROR(scope, RESOURCE, SETTINGS,
                    ("failed to activate bufferpool"),
                    ("failed to activate bufferpool"));
  return GST_FLOW_ERROR;
}
}

static GstFlowReturn gst_pm_audio_visualizer_chain(GstPad *pad,
                                                   GstObject *parent,
                                                   GstBuffer *buffer) {
  GstFlowReturn ret = GST_FLOW_OK;
  GstPMAudioVisualizer *scope = GST_PM_AUDIO_VISUALIZER(parent);
  GstPMAudioVisualizerClass *klass;
  GstClockTime ts, frame_duration;
  guint avail, sbpf;
  // databuf is a buffer holding one video frame worth of audio data used as
  // temp buffer for copying from the adapter only
  // inbuf is a plugin-scoped buffer holding a copy of the one video frame worth
  // of audio data from the adapter to process
  GstBuffer *databuf, *inbuf;
  gint bpf;

  klass = GST_PM_AUDIO_VISUALIZER_GET_CLASS(scope);

  // ensure caps have been setup for sink and src pads, and plugin init code is
  // done
  g_mutex_lock(&scope->priv->config_lock);
  while (!scope->priv->ready) {
    g_cond_wait(&scope->priv->ready_cond, &scope->priv->config_lock);
  }
  g_mutex_unlock(&scope->priv->config_lock);

  if (buffer == NULL) {
    return GST_FLOW_OK;
  }

  /* remember pts timestamp of the first audio buffer as stream clock offset
   * timestamp */
  g_mutex_lock(&scope->priv->config_lock);
  if (!scope->priv->pts_offset_initialized) {
    scope->priv->pts_offset_initialized = TRUE;
    scope->priv->pts_offset = GST_BUFFER_PTS(buffer);

    GstClock *clock = gst_element_get_clock(GST_ELEMENT(scope));
    GstClockTime running_time = gst_clock_get_time(clock) -
                                gst_element_get_base_time(GST_ELEMENT(scope));

    GST_DEBUG_OBJECT(
        scope,
        "Buffer ts: %" GST_TIME_FORMAT ", running_time: %" GST_TIME_FORMAT,
        GST_TIME_ARGS(scope->priv->pts_offset), GST_TIME_ARGS(running_time));
  }
  g_mutex_unlock(&scope->priv->config_lock);

  /* resync on DISCONT */
  if (GST_BUFFER_FLAG_IS_SET(buffer, GST_BUFFER_FLAG_DISCONT)) {
    gst_adapter_clear(scope->priv->adapter);
  }

  /* Make sure have an output format */
  if (gst_pad_check_reconfigure(scope->srcpad)) {
    if (!gst_pm_audio_visualizer_src_negotiate(scope)) {
      gst_pad_mark_reconfigure(scope->srcpad);
      goto not_negotiated;
    }
  }

  bpf = GST_AUDIO_INFO_BPF(&scope->ainfo);

  if (bpf == 0) {
    ret = GST_FLOW_NOT_NEGOTIATED;
    goto beach;
  }

  GST_TRACE_OBJECT(scope, "Chain func pushing %lu bytes to adapter",
                   gst_buffer_get_size(buffer));

  gst_adapter_push(scope->priv->adapter, buffer);

  g_mutex_lock(&scope->priv->config_lock);

  /* this is what we want */
  /* number of audio bytes to process for one video frame */
  /* samples per video frame * audio bytes per frame for both channels */
  sbpf = scope->req_spf * bpf;

  inbuf = scope->priv->inbuf;

  /* original code FIXME: the timestamp in the adapter would be different - this
   * should be fixed now by deriving timestamps from the number of samples
   * consumed. */
  gst_buffer_copy_into(inbuf, buffer, GST_BUFFER_COPY_METADATA, 0, -1);

  /* this is what we have */
  avail = gst_adapter_available(scope->priv->adapter);
  // GST_LOG_OBJECT(scope, "avail: %u, bpf: %u", avail, sbpf);
  while (avail >= sbpf) {

    gboolean fps_changed_since_last_frame = scope->priv->fps_changed;
    scope->priv->fps_changed = FALSE;

    // make sure frame duration does not change while processing one frame
    frame_duration = scope->req_frame_duration;

    /* calculate timestamp based on audio input samples already processed to
     * avoid clock drift */
    ts = scope->priv->pts_offset +
         gst_util_uint64_scale_int(scope->priv->samples_consumed, GST_SECOND,
                                   GST_AUDIO_INFO_RATE(&scope->ainfo));

    scope->priv->samples_consumed += scope->req_spf;

    /* check for QoS, don't compute buffers that are known to be late */
    if (GST_CLOCK_TIME_IS_VALID(ts)) {
      GstClockTime earliest_time;
      gdouble proportion;
      guint64 qostime;

      qostime = gst_segment_to_running_time(&scope->priv->segment,
                                            GST_FORMAT_TIME, ts) +
                frame_duration;

      earliest_time = scope->priv->earliest_time;
      proportion = scope->priv->proportion;

      if (scope->priv->segment.format != GST_FORMAT_TIME) {
        GST_WARNING_OBJECT(scope,
                           "Segment format not TIME, skipping QoS checks");
      } else if (GST_CLOCK_TIME_IS_VALID(earliest_time) &&
                 qostime <= earliest_time) {
        GstClockTime stream_time, jitter;
        GstMessage *qos_msg;

        GST_DEBUG_OBJECT(scope,
                         "QoS: skip ts: %" GST_TIME_FORMAT
                         ", earliest: %" GST_TIME_FORMAT,
                         GST_TIME_ARGS(qostime), GST_TIME_ARGS(earliest_time));

        ++scope->priv->dropped;
        stream_time = gst_segment_to_stream_time(&scope->priv->segment,
                                                 GST_FORMAT_TIME, ts);
        jitter = GST_CLOCK_DIFF(qostime, earliest_time);
        qos_msg =
            gst_message_new_qos(GST_OBJECT(scope), FALSE, qostime, stream_time,
                                ts, GST_BUFFER_DURATION(buffer));
        gst_message_set_qos_values(qos_msg, jitter, proportion, 1000000);
        gst_message_set_qos_stats(qos_msg, GST_FORMAT_BUFFERS,
                                  scope->priv->processed, scope->priv->dropped);
        gst_element_post_message(GST_ELEMENT(scope), qos_msg);

        goto skip;
      }
    }

    /* map pts ts via segment for general use */
    ts = gst_segment_to_stream_time(&scope->priv->segment, GST_FORMAT_TIME, ts);

    ++scope->priv->processed;

    /* sync controlled properties */
    if (GST_CLOCK_TIME_IS_VALID(ts))
      gst_object_sync_values(GST_OBJECT(scope), ts);

    /* this can fail as the data size we need could have changed */
    if (!(databuf = gst_adapter_get_buffer(scope->priv->adapter, sbpf)))
      break;

    /* place sbpf number of bytes of audio data into inbuf  */
    /* this is not a deep copy of the data at this point */
    gst_buffer_remove_all_memory(inbuf);
    gst_buffer_copy_into(inbuf, databuf, GST_BUFFER_COPY_MEMORY, 0, sbpf);
    gst_buffer_unref(databuf);

    /* call class->render() vmethod */
    g_mutex_unlock(&scope->priv->config_lock);

    ret = klass->render(scope, inbuf, ts, frame_duration);
    if (ret != GST_FLOW_OK) {
      goto beach;
    }

    g_mutex_lock(&scope->priv->config_lock);

  skip:
    // inform upstream of updated fps
    if (fps_changed_since_last_frame == TRUE) {
      gst_pm_audio_visualizer_send_latency_if_needed_unlocked(scope);
    }

    /* we want to take less or more, depending on spf : req_spf */
    if (avail - sbpf >= sbpf) {
      // enough audio data for more frames is available
      gst_adapter_unmap(scope->priv->adapter);
      gst_adapter_flush(scope->priv->adapter, sbpf);
    } else if (avail >= sbpf) {
      // was just enough audio data for one frame
      /* just flush a bit and stop */
      // rendering. seems like a bug in the original code
      // gst_adapter_flush(scope->priv->adapter, (avail - sbpf));

      // instead just flush one video frame worth of audio data from the buffer
      // and stop
      gst_adapter_unmap(scope->priv->adapter);
      gst_adapter_flush(scope->priv->adapter, sbpf);
      break;
    }
    avail = gst_adapter_available(scope->priv->adapter);

    // recalculate for the next frame
    sbpf = scope->req_spf * bpf;
  }

  g_mutex_unlock(&scope->priv->config_lock);

beach:
  return ret;

  /* ERRORS */
not_negotiated: {
  GST_DEBUG_OBJECT(scope, "Failed to renegotiate");
  return GST_FLOW_NOT_NEGOTIATED;
}
}

static gboolean gst_pm_audio_visualizer_src_event(GstPad *pad,
                                                  GstObject *parent,
                                                  GstEvent *event) {
  gboolean res;
  GstPMAudioVisualizer *scope;

  scope = GST_PM_AUDIO_VISUALIZER(parent);

  switch (GST_EVENT_TYPE(event)) {
  case GST_EVENT_QOS: {
    gdouble proportion;
    GstClockTimeDiff diff;
    GstClockTime timestamp;

    gst_event_parse_qos(event, NULL, &proportion, &diff, &timestamp);

    /* save stuff for the _chain() function */
    g_mutex_lock(&scope->priv->config_lock);
    // ignore QoS events for first few frames, sinks seem to send erratic QoS at
    // the beginning
    if (scope->priv->processed > QOS_IGNORE_FIRST_N_FRAMES) {
      scope->priv->proportion = proportion;
      if (diff > 0) {
        /* we're late, this is a good estimate for next displayable
         * frame (see part-qos.txt) */
        scope->priv->earliest_time = timestamp + MIN(diff * 2, GST_SECOND * 3) +
                                     scope->req_frame_duration;
      } else {
        scope->priv->earliest_time = timestamp + diff;
      }
    } else {
      GST_DEBUG_OBJECT(scope, "Ignoring early QoS event, processed frames: %d",
                       scope->priv->processed);
    }
    g_mutex_unlock(&scope->priv->config_lock);

    res = gst_pad_push_event(scope->priv->sinkpad, event);
    break;
  }
  case GST_EVENT_LATENCY:
    g_mutex_lock(&scope->priv->config_lock);
    gst_event_parse_latency(event, &scope->latency);
    g_mutex_unlock(&scope->priv->config_lock);
    res = gst_pad_event_default(pad, parent, event);
    GST_DEBUG_OBJECT(scope, "Received latency event: %" GST_TIME_FORMAT,
                     GST_TIME_ARGS(scope->latency));
    break;
  case GST_EVENT_RECONFIGURE:
    /* don't forward */
    gst_event_unref(event);
    res = TRUE;
    break;
  default:
    res = gst_pad_event_default(pad, parent, event);
    break;
  }

  return res;
}

static gboolean gst_pm_audio_visualizer_sink_event(GstPad *pad,
                                                   GstObject *parent,
                                                   GstEvent *event) {
  gboolean res;

  GstPMAudioVisualizer *scope = GST_PM_AUDIO_VISUALIZER(parent);
  GstPMAudioVisualizerClass *klass = GST_PM_AUDIO_VISUALIZER_GET_CLASS(scope);

  switch (GST_EVENT_TYPE(event)) {
  case GST_EVENT_CAPS: {
    GstCaps *caps;

    gst_event_parse_caps(event, &caps);
    res = gst_pm_audio_visualizer_sink_setcaps(scope, caps);
    gst_event_unref(event);
    break;
  }
  case GST_EVENT_FLUSH_STOP:
    g_mutex_lock(&scope->priv->config_lock);
    gst_pm_audio_visualizer_reset_unlocked(scope);
    g_mutex_unlock(&scope->priv->config_lock);
    res = gst_pad_push_event(scope->srcpad, event);
    break;
  case GST_EVENT_SEGMENT: {
    /* the newsegment values are used to clip the input samples
     * and to convert the incoming timestamps to running time so
     * we can do QoS */
    g_mutex_lock(&scope->priv->config_lock);
    gst_event_copy_segment(event, &scope->priv->segment);
    if (scope->priv->segment.format != GST_FORMAT_TIME) {
      GST_WARNING_OBJECT(scope, "Unexpected segment format: %d",
                         scope->priv->segment.format);
    }
    scope->priv->pts_offset =
        scope->priv->segment.start; // or segment.position if it's a live seek
    scope->priv->pts_offset_initialized = TRUE;
    scope->priv->samples_consumed = 0;
    g_mutex_unlock(&scope->priv->config_lock);
    if (klass->segment_change) {
      klass->segment_change(scope, &scope->priv->segment);
    }
    res = gst_pad_push_event(scope->srcpad, event);
    GST_DEBUG_OBJECT(
        scope, "Segment start: %" GST_TIME_FORMAT ", stop: %" GST_TIME_FORMAT,
        GST_TIME_ARGS(scope->priv->segment.start),
        GST_TIME_ARGS(scope->priv->segment.stop));
    break;
  }
  default:
    res = gst_pad_event_default(pad, parent, event);
    break;
  }

  return res;
}

static GstClockTime calc_our_latency_unlocked(GstPMAudioVisualizer *scope,
                                              gint rate) {
  /* the max samples we must buffer */
  guint max_samples = MAX(scope->req_spf, scope->priv->spf);
  return gst_util_uint64_scale(max_samples, GST_SECOND, rate);
}

static void gst_pm_audio_visualizer_send_latency_if_needed_unlocked(
    GstPMAudioVisualizer *scope) {

  // send latency event if latency changed a lot
  GstClockTime latency =
      calc_our_latency_unlocked(scope, GST_AUDIO_INFO_RATE(&scope->ainfo));

  // check if the latency has changed enough to send an event
  if (ABS((GstClockTimeDiff)latency - scope->priv->last_reported_latency) >
      LATENCY_EVENT_MIN_CHANGE) {

    scope->priv->last_reported_latency = latency;
    g_mutex_unlock(&scope->priv->config_lock);
    gst_pad_push_event(scope->priv->sinkpad, gst_event_new_latency(latency));
    GST_INFO_OBJECT(scope, "Sent latency event to sink pad: %" GST_TIME_FORMAT,
                    GST_TIME_ARGS(latency));
    g_mutex_lock(&scope->priv->config_lock);
  }
}

static gboolean gst_pm_audio_visualizer_src_query(GstPad *pad,
                                                  GstObject *parent,
                                                  GstQuery *query) {
  gboolean res = FALSE;
  GstPMAudioVisualizer *scope;

  scope = GST_PM_AUDIO_VISUALIZER(parent);

  switch (GST_QUERY_TYPE(query)) {
  case GST_QUERY_LATENCY: {
    /* We need to send the query upstream and add the returned latency to our
     * own */
    GstClockTime min_latency, max_latency;
    gboolean us_live;
    GstClockTime our_latency;
    gint rate = GST_AUDIO_INFO_RATE(&scope->ainfo);

    if (rate == 0)
      break;

    if ((res = gst_pad_peer_query(scope->priv->sinkpad, query))) {
      gst_query_parse_latency(query, &us_live, &min_latency, &max_latency);

      GST_DEBUG_OBJECT(
          scope, "Peer latency: min %" GST_TIME_FORMAT " max %" GST_TIME_FORMAT,
          GST_TIME_ARGS(min_latency), GST_TIME_ARGS(max_latency));

      g_mutex_lock(&scope->priv->config_lock);
      our_latency = calc_our_latency_unlocked(scope, rate);
      g_mutex_unlock(&scope->priv->config_lock);

      GST_DEBUG_OBJECT(scope, "Our latency: %" GST_TIME_FORMAT,
                       GST_TIME_ARGS(our_latency));

      /* we add some latency but only if we need to buffer more than what
       * upstream gives us */
      min_latency += our_latency;
      if (max_latency != -1)
        max_latency += our_latency;

      GST_DEBUG_OBJECT(scope,
                       "Calculated total latency : min %" GST_TIME_FORMAT
                       " max %" GST_TIME_FORMAT,
                       GST_TIME_ARGS(min_latency), GST_TIME_ARGS(max_latency));

      gst_query_set_latency(query, TRUE, min_latency, max_latency);
      g_mutex_lock(&scope->priv->config_lock);
      scope->priv->last_reported_latency = our_latency;
      g_mutex_unlock(&scope->priv->config_lock);
    }
    break;
  }
  default:
    res = gst_pad_query_default(pad, parent, query);
    break;
  }

  return res;
}

static GstStateChangeReturn
gst_pm_audio_visualizer_parent_change_state(GstElement *element,
                                            GstStateChange transition) {

  GstPMAudioVisualizer *scope = GST_PM_AUDIO_VISUALIZER(element);

  switch (transition) {
  case GST_STATE_CHANGE_READY_TO_PAUSED:
    g_mutex_lock(&scope->priv->config_lock);
    gst_pm_audio_visualizer_reset_unlocked(scope);
    g_mutex_unlock(&scope->priv->config_lock);
    break;
  default:
    break;
  }

  GstStateChangeReturn ret =
      GST_ELEMENT_CLASS(parent_class)->change_state(element, transition);
  if (ret == GST_STATE_CHANGE_FAILURE)
    return ret;

  switch (transition) {
  case GST_STATE_CHANGE_PAUSED_TO_READY:
    gst_pm_audio_visualizer_set_allocation(scope, NULL, NULL, NULL, NULL);
    break;
  case GST_STATE_CHANGE_READY_TO_NULL:
    break;
  default:
    break;
  }

  GstPMAudioVisualizerClass *klass = GST_PM_AUDIO_VISUALIZER_GET_CLASS(scope);
  return klass->change_state(element, transition);
}

static GstStateChangeReturn
gst_pm_audio_visualizer_default_change_state(GstElement *element,
                                             GstStateChange transition) {
  return GST_STATE_CHANGE_SUCCESS;
}

static gboolean log_fps_change(gpointer message) {
  GST_INFO("%s", (gchar *)message);

  g_free(message);
  return G_SOURCE_REMOVE; // remove after run
}

void gst_pm_audio_visualizer_adjust_fps(GstPMAudioVisualizer *scope,
                                        guint64 frame_duration) {
  g_mutex_lock(&scope->priv->config_lock);

  guint64 set_duration;
  guint set_req_spf;

  // clamp for cap fps
  if (frame_duration <= scope->priv->caps_frame_duration) {
    set_duration = scope->priv->caps_frame_duration;
    set_req_spf = scope->priv->spf;
  } else {
    set_duration = frame_duration;
    // calculate samples per frame for the given frame duration
    set_req_spf =
        (guint)(((guint64)GST_AUDIO_INFO_RATE(&scope->ainfo) * frame_duration +
                 GST_SECOND / 2) /
                GST_SECOND);
  }

  // update for next frame
  if (scope->req_frame_duration != set_duration) {
    scope->req_frame_duration = set_duration;
    scope->req_spf = set_req_spf;
    scope->priv->fps_changed = TRUE;
  }

  g_mutex_unlock(&scope->priv->config_lock);

  if (gst_debug_category_get_threshold(pm_audio_visualizer_debug) >=
      GST_LEVEL_WARNING) {

    gchar *message =
        g_strdup_printf("Adjusting framerate, max fps: %f, using "
                        "frame-duration: %" GST_TIME_FORMAT ", spf: %u",
                        (gdouble)frame_duration / GST_SECOND,
                        GST_TIME_ARGS(set_duration), set_req_spf);

    g_idle_add(log_fps_change, message);
  }
}
