
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "renderbuffer.h"

GST_DEBUG_CATEGORY_STATIC(renderbuffer_debug);
#define GST_CAT_DEFAULT renderbuffer_debug

/**
 * Queue shutdown signal token.
 */
static gpointer RB_Q_SHUTDOWN_SIGNAL = &RB_Q_SHUTDOWN_SIGNAL;

/**
 * Number of frames inspected by EMA.
 */
#ifndef RB_EMA_FPS_ADJUST_INTERVAL
#define RB_EMA_FPS_ADJUST_INTERVAL 10
#endif

/**
 * EMA alpha = 0.25
 */
#ifndef RB_EMA_ALPHA_N
#define RB_EMA_ALPHA_N 1
#define RB_EMA_ALPHA_D 4
#endif

/**
 * Increase frame duration (slow down fps) in case of detected lag.
 * +20%
 */
#ifndef RB_EMA_FRAME_DURATION_INCREASE_N
#define RB_EMA_FRAME_DURATION_INCREASE_N 12
#define RB_EMA_FRAME_DURATION_INCREASE_D 10
#endif

/**
 * Decrease frame duration (speed up fps) in case rendering performance
 * recovers. -5%
 */
#ifndef RB_EMA_FRAME_DURATION_DECREASE_N
#define RB_EMA_FRAME_DURATION_DECREASE_N 95
#define RB_EMA_FRAME_DURATION_DECREASE_D 100
#endif

/**
 * Tolerance for being too slow.
 * Allow render time up to 1.1x
 */
#ifndef RB_EMA_FRAME_DURATION_TOLERANCE_UP_N
#define RB_EMA_FRAME_DURATION_TOLERANCE_UP_N 110
#define RB_EMA_FRAME_DURATION_TOLERANCE_UP_D 100
#endif

/**
 * Tolerance for being too fast.
 * allow render time as low as 0.9x
 */
#ifndef RB_EMA_FRAME_DURATION_TOLERANCE_DOWN_N
#define RB_EMA_FRAME_DURATION_TOLERANCE_DOWN_N 9
#define RB_EMA_FRAME_DURATION_TOLERANCE_DOWN_D 10
#endif

/**
 * Possible option to drop frames that are too late after rendering if they
 * would be dropped by a downstream sink anyway.
 * Experimental, tends to increase flicker in most cases. Disabled per default.
 */
// #define RB_DROP_LATE_FRAMES
#ifndef RB_DROP_LATE_FRAMES_TOLERANCE
#define RB_DROP_LATE_FRAMES_TOLERANCE (GST_MSECOND * 8)
#endif

/**
 * How much time has to be left of the time budget for scheduling before
 * entering wait. Tolerance to account for scheduling overhead etc. to guarantee
 * a defined max run-time of the scheduling process.
 */
#ifndef MIN_FREE_SLOT_SCHEDULE_WAIT
#define MIN_FREE_SLOT_SCHEDULE_WAIT (GST_MSECOND * 1)
#endif

/**
 * Exponential Moving Average (EMA)-based adaptive frame duration (fps)
 * adjustment. Determines desired frame duration change based on the
 * frame render duration and min/max fps configs.
 *
 * @param state Render state data.
 * @param render_duration Render duration for the last frame in nanos.
 * @param frame_duration Current desired frame duration in nanos (fps *
 * GST_SECOND).
 */
static void rb_handle_adaptive_fps_ema(RBRenderBuffer *state,
                                       const GstClockTime render_duration,
                                       const GstClockTime frame_duration) {
  state->frame_counter++;

  // EMA smoothing: smoothed = alpha * x + (1 - alpha) * prev
  state->smoothed_render_time =

      gst_util_uint64_scale_int(render_duration, RB_EMA_ALPHA_N,
                                RB_EMA_ALPHA_D) +

      gst_util_uint64_scale_int(state->smoothed_render_time,
                                RB_EMA_ALPHA_D - RB_EMA_ALPHA_N,
                                RB_EMA_ALPHA_D);

  if (state->frame_counter >= RB_EMA_FPS_ADJUST_INTERVAL) {

    GstClockTime new_duration;
    state->frame_counter = 0;

    const GstClockTime upper_threshold = gst_util_uint64_scale_int(
        frame_duration, RB_EMA_FRAME_DURATION_TOLERANCE_UP_N,
        RB_EMA_FRAME_DURATION_TOLERANCE_UP_D);

    const GstClockTime lower_threshold = gst_util_uint64_scale_int(
        frame_duration, RB_EMA_FRAME_DURATION_TOLERANCE_DOWN_N,
        RB_EMA_FRAME_DURATION_TOLERANCE_DOWN_D);

    if (state->smoothed_render_time > upper_threshold) {

      // rendering too slow, increase frame duration (drop FPS)
      new_duration = gst_util_uint64_scale_int(
          frame_duration, RB_EMA_FRAME_DURATION_INCREASE_N,
          RB_EMA_FRAME_DURATION_INCREASE_D);
    } else if (state->smoothed_render_time < lower_threshold) {

      // rendering fast enough, try to decrease frame duration (increase FPS)
      new_duration = gst_util_uint64_scale_int(
          frame_duration, RB_EMA_FRAME_DURATION_DECREASE_N,
          RB_EMA_FRAME_DURATION_DECREASE_D);
    } else {
      // within tolerance, no change
      return;
    }

    g_mutex_lock(&state->slot_lock);

    // clamp min/max frame duration (fps) according to config
    if (new_duration > state->max_frame_duration) {
      new_duration = state->max_frame_duration;
    } else if (new_duration < state->caps_frame_duration) {
      new_duration = state->caps_frame_duration;
    }

    g_mutex_unlock(&state->slot_lock);

    if (new_duration != frame_duration) {
      GST_DEBUG_OBJECT(
          state->plugin,
          "Adaptive FPS: frame duration changed from %" GST_TIME_FORMAT
          " to %" GST_TIME_FORMAT,
          GST_TIME_ARGS(frame_duration), GST_TIME_ARGS(new_duration));

      // pass new frame duration to callback
      state->adjust_fps_func(state, new_duration);
    }
  }
}

static void rb_queue_gl_buffer_cleanup(const RBRenderBuffer *state,
                                       GstBuffer *out) {
  g_async_queue_push(state->buffer_cleanup_queue, out);
}

void rb_set_qos_enabled(RBRenderBuffer *state, const gboolean is_qos_enabled) {
  g_mutex_lock(&state->slot_lock);
  state->qos_enabled = is_qos_enabled;
  g_mutex_unlock(&state->slot_lock);
}

void rb_init_render_buffer(RBRenderBuffer *state, GstObject *plugin,
                           const GstGLContextThreadFunc gl_fill_func,
                           const RBAdjustFpsFunc adjust_fps_func,
                           const GstClockTime max_frame_duration,
                           const GstClockTime caps_frame_duration,
                           const gboolean is_qos_enabled) {

  GST_DEBUG_CATEGORY_INIT(renderbuffer_debug, "renderbuffer", 0,
                          "projectM visualizer plugin render buffer");

  state->plugin = plugin;
  state->adjust_fps_func = adjust_fps_func;

  state->gl_context = NULL;
  state->src_pad = NULL;
  state->render_thread = NULL;
  state->running = FALSE;

  state->qos_enabled = is_qos_enabled;
  state->caps_frame_duration = caps_frame_duration;
  state->max_frame_duration = max_frame_duration;

  state->last_insert_index = -1;
  state->last_render_index = -1;
  state->frame_counter = 0;
  state->smoothed_render_time = 0;

  g_mutex_init(&state->slot_lock);
  g_cond_init(&state->slot_available_cond);
  g_cond_init(&state->render_queued_cond);
  g_cond_init(&state->render_complete_cond);
  state->buffer_cleanup_queue = g_async_queue_new();

  for (guint i = 0; i < NUM_RENDER_SLOTS; i++) {
    state->slots[i].state = RB_EMPTY;
    state->slots[i].plugin = plugin;
    state->slots[i].gl_result = FALSE;
    state->slots[i].pts = GST_CLOCK_TIME_NONE;
    state->slots[i].frame_duration = 0;
    state->slots[i].latency = GST_CLOCK_TIME_NONE;
    state->slots[i].running_time = GST_CLOCK_TIME_NONE;
    state->slots[i].out_buf = NULL;
    state->slots[i].gl_fill_func = gl_fill_func;
    state->slots[i].in_audio = NULL;
  }
}

void rb_dispose_render_buffer(RBRenderBuffer *state) {
  g_async_queue_unref(state->buffer_cleanup_queue);
  g_cond_clear(&state->slot_available_cond);
  g_cond_clear(&state->render_queued_cond);
  g_cond_clear(&state->render_complete_cond);
  g_mutex_clear(&state->slot_lock);
}

RBQueueResult rb_queue_render_job(RBQueueArgs *args) {

  RBRenderBuffer *state = args->render_buffer;
  const gboolean wait_is_limited = args->max_wait != GST_CLOCK_TIME_NONE;
  const GstClockTime start = gst_util_get_timestamp();
  GstClockTimeDiff used_wait = 0;

  g_mutex_lock(&state->slot_lock);

  RBSlot *slot = NULL;
  gint slot_index;

  gboolean found_slot = FALSE;
  while (!found_slot) {

    // next slot to insert to
    slot_index = (state->last_insert_index + 1) % NUM_RENDER_SLOTS;
    slot = &state->slots[slot_index];

    // jump over busy slot that's currently rendering if needed
    if (slot->state == RB_BUSY) {
      slot_index = (state->last_insert_index + 2) % NUM_RENDER_SLOTS;
    }

    // in case there is only one slot, it may still be busy
    found_slot =
        slot->state != RB_BUSY && (wait_is_limited || slot->state == RB_EMPTY);

    if (!found_slot) {
      if (wait_is_limited) {
        const GstClockTimeDiff remaining_wait = args->max_wait - used_wait;
        // not waiting until the very last millisecond
        // to avoid exceeding time budget
        if (remaining_wait > MIN_FREE_SLOT_SCHEDULE_WAIT) {

          // this is in microseconds for a change
          const gint64 now = g_get_monotonic_time();
          const gint64 deadline = now + remaining_wait / 1000;

          g_cond_wait_until(&state->slot_available_cond, &state->slot_lock,
                            deadline);

          used_wait = (GstClockTimeDiff)gst_util_get_timestamp() - start;
        } else {
          // not enough time left
          break;
        }
      } else {
        // no time constraints, frames are never dropped
        // we just wait and keep trying
        g_cond_wait(&state->slot_available_cond, &state->slot_lock);
      }
    }
  }

  if (slot->state == RB_BUSY) {
    // out of time, and we still can't schedule
    g_mutex_unlock(&state->slot_lock);
    return RB_TIMEOUT;
  }

  state->last_insert_index = slot_index;

  // evict if already in use and clear buffers
  const gboolean is_evicted = slot->state == RB_READY;

  if (slot->in_audio != NULL) {
    gst_buffer_unref(slot->in_audio);
  }

  if (slot->out_buf != NULL) {
    rb_queue_gl_buffer_cleanup(state, slot->out_buf);
    slot->out_buf = NULL;
  }

  // populate slot
  slot->state = RB_READY;
  slot->gl_result = FALSE;
  slot->pts = args->pts;
  slot->frame_duration = args->frame_duration;
  slot->latency = args->latency;
  slot->running_time = args->running_time;
  slot->in_audio = gst_buffer_copy_deep(args->in_audio);

  // signal render thread that there is something to do
  g_cond_signal(&state->render_queued_cond);

  if (args->sync_rendering) {
    // block until rendering completed, if requested
    g_cond_wait(&state->render_complete_cond, &state->slot_lock);
  }

  g_mutex_unlock(&state->slot_lock);

  RBQueueResult result;
  if (found_slot) {
    result = is_evicted == FALSE ? RB_SUCCESS : RB_EVICTED;
  } else {
    result = RB_TIMEOUT;
  }
  return result;
}

void rb_queue_render_job_warn(RBQueueArgs *args) {

  RBRenderBuffer *state = args->render_buffer;
  const GstClockTime start_ts = gst_util_get_timestamp();

  const RBQueueResult result = rb_queue_render_job(args);

  switch (result) {
  case RB_EVICTED: {
    GST_DEBUG_OBJECT(state->plugin,
                     "Dropping previous GL frame from render buffer, "
                     "it was not picked up for rendering in time (evicted). "
                     "max-wait: %" GST_TIME_FORMAT ", pts: %" GST_TIME_FORMAT,
                     GST_TIME_ARGS(args->max_wait), GST_TIME_ARGS(args->pts));
    break;
  }

  case RB_TIMEOUT: {
    const GstClockTime now = gst_util_get_timestamp();
    GST_DEBUG_OBJECT(
        state->plugin,
        "Dropping GL frame from render buffer, waiting for free slot took too "
        "long. elapsed: %" GST_TIME_FORMAT ", max-wait: %" GST_TIME_FORMAT
        ", pts: %" GST_TIME_FORMAT,
        GST_TIME_ARGS(now - start_ts), GST_TIME_ARGS(args->max_wait),
        GST_TIME_ARGS(args->pts));
    break;
  }

  case RB_SUCCESS:
    break;
  }
}

/**
 * Calculate current time based on given element's clock for QoS checks.
 *
 * @param element The plugin element.
 * @return Current time as determined by clock used by element.
 */
static GstClockTime rb_element_render_time(GstElement *element) {
  const GstClockTime base_time = gst_element_get_base_time(element);
  GstClock *clock = gst_element_get_clock(element);
  const GstClockTime now = gst_clock_get_time(clock);
  return now - base_time;
}

/**
 * Determine if it's likely too late push a buffer, as it would likely be
 * dropped by a pipeline synchronized sink.
 *
 * @param element The plugin element.
 * @param latency Pipeline latency.
 * @param running_time Current buffer running time.
 * @return TRUE in case the buffer is too late.
 */
static gboolean rb_is_render_too_late(GstElement *element,
                                      const GstClockTime latency,
                                      const GstClockTime running_time) {

  if (latency == GST_CLOCK_TIME_NONE) {
    return FALSE;
  }

  const GstClockTime tolerance = RB_DROP_LATE_FRAMES_TOLERANCE;

  const GstClockTime render_time = rb_element_render_time(element);

  // latest time to push this buffer for it to make it to sink in time
  const GstClockTime latest_push_time = running_time + latency;

  if (render_time > latest_push_time + tolerance) {
    GST_DEBUG_OBJECT(element,
                     "Dropping late frame: render_time %" GST_TIME_FORMAT
                     " > buffer_running_time %" GST_TIME_FORMAT
                     " + latency %" GST_TIME_FORMAT
                     " + slack %" GST_TIME_FORMAT,
                     GST_TIME_ARGS(render_time), GST_TIME_ARGS(running_time),
                     GST_TIME_ARGS(latency), GST_TIME_ARGS(tolerance));
    return TRUE;
  }
  return FALSE;
}

/**
 * Callback for scheduling gl buffer release with gl thread.
 *
 * @param context Current gl context.
 * @param buf GL buffer to release.
 */
static void gl_buffer_cleanup(GstGLContext *context, gpointer buf) {
  gst_buffer_unref(GST_BUFFER(buf));
}

/**
 * Used to dispose of gl buffers only.
 * Consume buffers to clean-up and dispatches release through gl thread.
 *
 * @param user_data The GstBuffer pointer to release.
 * @return
 */
static gpointer cleanup_thread_func(gpointer user_data) {

  const RBRenderBuffer *state = (RBRenderBuffer *)user_data;
  while (state->running) {

    gpointer item = g_async_queue_pop(state->buffer_cleanup_queue);

    if (!item || item == RB_Q_SHUTDOWN_SIGNAL)
      continue;

    gst_gl_context_thread_add(state->gl_context, gl_buffer_cleanup, item);
  }
  return NULL;
}

/**
 * Render thread main worker function.
 *
 * @param user_data Render buffer to work on.
 * @return NULL
 */
static gpointer rb_render_thread_func(gpointer user_data) {

  RBRenderBuffer *state = (RBRenderBuffer *)user_data;
#if NUM_RENDER_SLOTS > 2
  GstClockTime last_pts = 0;
#endif
  // slot modifications are locked
  g_mutex_lock(&state->slot_lock);

  // start working on rendering frames until we shut down
  while (state->running) {

    // first find a slot with data that's ready to render
    gboolean found_slot = FALSE;
    RBSlot *slot;
    gint render_index;
    while (!found_slot) {
      render_index = (state->last_render_index + 1) % NUM_RENDER_SLOTS;

      slot = &state->slots[render_index];

      // find a slot with audio input data
      // also check if it's already older than the last frame or if it's the
      // first frame (shouldn't happen unless the ring buffer capacity > 2)
      if (slot->state == RB_READY
#if NUM_RENDER_SLOTS > 2
          // wontfix: segment events would need to be handled for this check to
          // work right otherwise last_pts is not reset when the pts changes.
          && (last_pts == 0 || slot->pts > last_pts)
#endif
      ) {
        found_slot = TRUE;
      } else {
        // no data is ready, wait for a new audio buffer being pushed
        g_cond_wait(&state->render_queued_cond, &state->slot_lock);
        if (state->running == FALSE) {
          break;
        }
      }
    }

    // no slot means we're not running anymore
    if (found_slot == FALSE) {
      g_cond_signal(&state->render_complete_cond);
      break;
    }

    // update iteration maker
    state->last_render_index = render_index;
#if NUM_RENDER_SLOTS > 2
    last_pts = slot->pts;
#endif

    // nobody else is allowed to touch the slot anymore, it's owned by the
    // render thread now
    slot->state = RB_BUSY;

    g_mutex_unlock(&state->slot_lock);

    // measure rendering for QoS
    GstClockTime render_start = gst_util_get_timestamp();

    // Dispatch slot to GL thread
    gst_gl_context_thread_add(state->gl_context, slot->gl_fill_func, slot);

    GstClockTime render_time = gst_util_get_timestamp() - render_start;

    // render took longer than the frame duration, this is a problem for
    // real-time rendering if it happens too often
    if (render_time > slot->frame_duration) {
      GST_DEBUG_OBJECT(
          state->plugin,
          "Render GL frame took too long: %" GST_TIME_FORMAT
          ", frame-duration: %" GST_TIME_FORMAT ", pts: %" GST_TIME_FORMAT,
          GST_TIME_ARGS(render_time), GST_TIME_ARGS(slot->frame_duration),
          GST_TIME_ARGS(slot->pts));
    }

    // copy params to locals vars to release the slot
    GstBuffer *audio_buffer = slot->in_audio;
    const GstClockTime frame_duration = slot->frame_duration;
    const GstClockTime pts = slot->pts;
#ifdef RB_DROP_LATE_FRAMES
    const GstClockTime latency = slot->latency;
    const GstClockTime running_time = slot->running_time;
#endif

    // copy results to locals vars to release the slot
    GstBuffer *outbuf = slot->out_buf;
    const gboolean gl_result = slot->gl_result;

    // Lock and reset slot data
    g_mutex_lock(&state->slot_lock);

    // signal render complete
    g_cond_signal(&state->render_complete_cond);

    slot->in_audio = NULL;
    slot->state = RB_EMPTY;
    slot->out_buf = NULL;
    slot->gl_result = FALSE;

    // let queuing know that a slot is available
    g_cond_signal(&state->slot_available_cond);
    g_mutex_unlock(&state->slot_lock);

    // populate timestamps after rendering so they can't be changed by accident
    GST_BUFFER_PTS(outbuf) = pts;
    GST_BUFFER_DTS(outbuf) = pts;
    GST_BUFFER_DURATION(outbuf) = frame_duration;

    if (gst_buffer_get_size(outbuf) == 0) {
      GST_WARNING_OBJECT(state->plugin, "Empty or invalid buffer, dropping.");
      rb_queue_gl_buffer_cleanup(state, outbuf);
      outbuf = NULL;
    } else {

      // we got a rendered buffer, perform rendering loop QoS
#ifdef RB_DROP_LATE_FRAMES
      gboolean dropped = FALSE;

      if (state->is_pipeline_realtime) {
        dropped = rb_is_render_too_late(GST_ELEMENT(state->plugin), latency,
                                        running_time);
      }
      if (!dropped) {
#endif

        // push buffer downstream
        const GstFlowReturn ret = gst_pad_push(state->src_pad, outbuf);
        if (ret != GST_FLOW_OK) {
          GST_WARNING("Failed to push buffer to pad");
        }

#ifdef RB_DROP_LATE_FRAMES
      } else {
        rb_queue_gl_buffer_cleanup(state, outbuf);
        outbuf = NULL;
      }
#endif

      // process rendering fps QoS
      if (state->qos_enabled) {
        rb_handle_adaptive_fps_ema(state, render_time, frame_duration);
      }
    }

    gst_buffer_unref(audio_buffer);

    if (!gl_result) {
      GST_WARNING_OBJECT(
          state->plugin,
          "Failed to render buffer, gl rendering returned error");
    }

    g_mutex_lock(&state->slot_lock);
  }

  g_mutex_unlock(&state->slot_lock);

  return NULL;
}

void rb_start_render_thread(RBRenderBuffer *state, GstGLContext *gl_context,
                            GstPad *src_pad) {
  state->gl_context = gl_context;
  state->src_pad = src_pad;
  state->running = TRUE;
  state->render_thread =
      g_thread_new("rb-render-thread", rb_render_thread_func, state);
  state->cleanup_thread =
      g_thread_new("rb-cleanup-thread", cleanup_thread_func, state);

  GST_INFO_OBJECT(state->plugin, "Started render buffer");
}

void rb_stop_render_thread(RBRenderBuffer *state) {

  g_mutex_lock(&state->slot_lock);
  state->running = FALSE;

  g_cond_broadcast(&state->render_queued_cond);
  g_mutex_unlock(&state->slot_lock);

  g_thread_join(state->render_thread);
  g_async_queue_push(state->buffer_cleanup_queue, RB_Q_SHUTDOWN_SIGNAL);
  g_thread_join(state->cleanup_thread);
  state->gl_context = NULL;
  state->src_pad = NULL;
  GST_INFO_OBJECT(state->plugin, "Stopped render buffer");
}

void rb_set_caps_frame_duration(RBRenderBuffer *state,
                                const GstClockTime caps_frame_duration) {
  g_mutex_lock(&state->slot_lock);
  state->caps_frame_duration = caps_frame_duration;
  g_mutex_unlock(&state->slot_lock);
}
