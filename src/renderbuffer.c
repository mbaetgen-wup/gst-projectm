
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <math.h>

#include "renderbuffer.h"

GST_DEBUG_CATEGORY_STATIC(renderbuffer_debug);
#define GST_CAT_DEFAULT renderbuffer_debug

/**
 * Number of frames inspected by EMA.
 */
#ifndef RB_EMA_FPS_ADJUST_INTERVAL
#define RB_EMA_FPS_ADJUST_INTERVAL 10
#endif

/**
 * EMA alpha = 0.2
 */
#ifndef RB_EMA_ALPHA_N
#define RB_EMA_ALPHA_N 1
#endif

#ifndef RB_EMA_ALPHA_D
#define RB_EMA_ALPHA_D 5
#endif

/**
 * EMA increase frame duration (slow down fps) in case of detected lag.
 * +15%
 */
#ifndef RB_EMA_FRAME_DURATION_INCREASE_N
#define RB_EMA_FRAME_DURATION_INCREASE_N 115
#endif

#ifndef RB_EMA_FRAME_DURATION_INCREASE_D
#define RB_EMA_FRAME_DURATION_INCREASE_D 100
#endif

/**
 * EMA decrease frame duration (speed up fps) in case rendering performance
 * recovers. -5%
 */
#ifndef RB_EMA_FRAME_DURATION_DECREASE_N
#define RB_EMA_FRAME_DURATION_DECREASE_N 95
#endif

#ifndef RB_EMA_FRAME_DURATION_DECREASE_D
#define RB_EMA_FRAME_DURATION_DECREASE_D 100
#endif

/**
 * EMA tolerance for being too slow.
 * Allow render time up to 1.1x
 */
#ifndef RB_EMA_FRAME_DURATION_TOLERANCE_UP_N
#define RB_EMA_FRAME_DURATION_TOLERANCE_UP_N 110
#endif

#ifndef RB_EMA_FRAME_DURATION_TOLERANCE_UP_D
#define RB_EMA_FRAME_DURATION_TOLERANCE_UP_D 100
#endif

/**
 * EMA tolerance for being too fast.
 * allow render time as low as 0.95x
 */
#ifndef RB_EMA_FRAME_DURATION_TOLERANCE_DOWN_N
#define RB_EMA_FRAME_DURATION_TOLERANCE_DOWN_N 95
#endif

#ifndef RB_EMA_FRAME_DURATION_TOLERANCE_DOWN_D
#define RB_EMA_FRAME_DURATION_TOLERANCE_DOWN_D 100
#endif

/**
 * How much time has to be left of the time budget for scheduling before
 * entering wait. Tolerance to account for scheduling overhead etc. to guarantee
 * a defined max run-time of the scheduling process.
 */
#ifndef MIN_FREE_SLOT_SCHEDULE_WAIT
#define MIN_FREE_SLOT_SCHEDULE_WAIT GST_MSECOND
#endif

/**
 * Controls if pushing of gl buffers is done by the render loop (blocking)
 * directly, or deferred to the push buffer. Deferring allows a more responsive
 * render timing.
 *
 *  0 : Push blocks render loop.
 *  1 : Push is deferred to push buffer, may or may not block.
 */
#ifndef PUSH_BUFFER_ENABLED
#define PUSH_BUFFER_ENABLED 1
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
  g_assert(state != NULL);

  state->ema_frame_counter++;

  // EMA smoothing: smoothed = alpha * x + (1 - alpha) * prev
  state->ema_smoothed_render_time =

      gst_util_uint64_scale_int(render_duration, RB_EMA_ALPHA_N,
                                RB_EMA_ALPHA_D) +

      gst_util_uint64_scale_int(state->ema_smoothed_render_time,
                                RB_EMA_ALPHA_D - RB_EMA_ALPHA_N,
                                RB_EMA_ALPHA_D);

  if (state->ema_frame_counter >= RB_EMA_FPS_ADJUST_INTERVAL) {

    GstClockTime new_duration;
    state->ema_frame_counter = 0;

    const GstClockTime upper_threshold = gst_util_uint64_scale_int(
        frame_duration, RB_EMA_FRAME_DURATION_TOLERANCE_UP_N,
        RB_EMA_FRAME_DURATION_TOLERANCE_UP_D);

    const GstClockTime lower_threshold = gst_util_uint64_scale_int(
        frame_duration, RB_EMA_FRAME_DURATION_TOLERANCE_DOWN_N,
        RB_EMA_FRAME_DURATION_TOLERANCE_DOWN_D);

    if (state->ema_smoothed_render_time > upper_threshold) {

      // rendering too slow, increase frame duration (drop FPS)
      new_duration = gst_util_uint64_scale_int(
          frame_duration, RB_EMA_FRAME_DURATION_INCREASE_N,
          RB_EMA_FRAME_DURATION_INCREASE_D);
    } else if (state->ema_smoothed_render_time < lower_threshold) {

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

void rb_init_render_buffer(RBRenderBuffer *state, GstObject *plugin,
                           GstGLContext *gl_context, GstPad *src_pad,
                           const GstGLContextThreadFunc gl_fill_func,
                           const RBAdjustFpsFunc adjust_fps_func,
                           const GstClockTime max_frame_duration,
                           const GstClockTime caps_frame_duration,
                           const gboolean is_qos_enabled,
                           const gboolean is_realtime) {

  g_assert(state != NULL);
  g_assert(plugin != NULL);
  g_assert(gl_context != NULL);
  g_assert(src_pad != NULL);
  g_assert(gl_fill_func != NULL);
  g_assert(adjust_fps_func != NULL);
  g_assert(max_frame_duration >= caps_frame_duration);

  static gsize _debug_initialized = 0;
  if (g_once_init_enter(&_debug_initialized)) {
    GST_DEBUG_CATEGORY_INIT(renderbuffer_debug, "renderbuffer", 0,
                            "projectM visualizer plugin render buffer");
  }

  // context config without ownership
  state->plugin = plugin;
  state->adjust_fps_func = adjust_fps_func;
  state->gl_context = gl_context;
  state->src_pad = src_pad;

  // never changed after init
  state->qos_enabled = is_qos_enabled;
  state->is_realtime = is_realtime;
  state->caps_frame_duration = caps_frame_duration;
  state->max_frame_duration = max_frame_duration;

  // changed all the time
  g_atomic_int_set(&state->running, FALSE);

  // init render queue
  state->render_thread = NULL;
  state->render_write_idx = NUM_RENDER_SLOTS - 1;
  state->render_read_idx = NUM_RENDER_SLOTS - 1;
  g_mutex_init(&state->slot_lock);
  g_cond_init(&state->slot_available_cond);
  g_cond_init(&state->render_queued_cond);

  for (guint i = 0; i < NUM_RENDER_SLOTS; i++) {
    state->slots[i].state = RB_EMPTY;
    state->slots[i].plugin = plugin;
    state->slots[i].gl_result = FALSE;
    state->slots[i].pts = GST_CLOCK_TIME_NONE;
    state->slots[i].frame_duration = 0;
    state->slots[i].out_buf = NULL;
    state->slots[i].gl_fill_func = gl_fill_func;
    state->slots[i].in_audio = NULL;
  }

  // init EMA
  state->ema_frame_counter = 0;
  state->ema_smoothed_render_time = 0;

  bd_init_buffer_disposal(&state->buffer_disposal, gl_context);
  pb_init_push_buffer(&state->push_buffer, &state->buffer_disposal, plugin,
                      src_pad);
}

void rb_dispose_render_buffer(RBRenderBuffer *state) {
  g_assert(state != NULL);

  g_cond_clear(&state->slot_available_cond);
  g_cond_clear(&state->render_queued_cond);

  g_mutex_clear(&state->slot_lock);

  pb_dispose_push_buffer(&state->push_buffer);
  bd_dispose_buffer_disposal(&state->buffer_disposal);
}

RBQueueResult rb_queue_render_task(RBQueueArgs *args) {
  g_assert(args != NULL);

  RBRenderBuffer *state = args->render_buffer;
  g_assert(state != NULL);

  const gboolean wait_is_limited = args->max_wait != GST_CLOCK_TIME_NONE;
  const GstClockTime start = gst_util_get_timestamp();
  GstClockTimeDiff used_wait = 0;

  g_mutex_lock(&state->slot_lock);

  RBSlot *slot = NULL;
  gint slot_index = 0;

  gboolean found_slot = FALSE;
  while (!found_slot) {

    // next slot to insert to
    slot_index = (state->render_write_idx + 1) % NUM_RENDER_SLOTS;
    slot = &state->slots[slot_index];

    // jump over busy slot that's currently rendering if needed
    if (slot->state == RB_BUSY) {
      slot_index = (state->render_write_idx + 2) % NUM_RENDER_SLOTS;
    }

    // in case there is only one slot, it may still be busy
    found_slot =
        slot->state != RB_BUSY && (wait_is_limited || slot->state == RB_EMPTY);

    if (!found_slot) {
      if (wait_is_limited) {
        const GstClockTimeDiff remaining_wait =
            (GstClockTimeDiff)args->max_wait - used_wait;
        // not waiting until the very last millisecond
        // to avoid exceeding time budget
        if (remaining_wait > MIN_FREE_SLOT_SCHEDULE_WAIT) {

          // this is in microseconds for a change
          const gint64 now = g_get_monotonic_time();
          const gint64 deadline = now + remaining_wait / 1000;

          g_cond_wait_until(&state->slot_available_cond, &state->slot_lock,
                            deadline);

          used_wait = GST_CLOCK_DIFF(start, gst_util_get_timestamp());
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

  state->render_write_idx = slot_index;

  // evict if already in use and clear buffers
  const gboolean is_evicted = slot->state == RB_READY;

  if (slot->in_audio != NULL) {
    gst_buffer_unref(slot->in_audio);
  }

  if (slot->out_buf != NULL) {
    bd_dispose_gl_buffer(&state->buffer_disposal, slot->out_buf);
    slot->out_buf = NULL;
  }

  // populate slot
  slot->state = RB_READY;
  slot->gl_result = FALSE;
  slot->pts = args->pts;
  slot->frame_duration = args->frame_duration;
  slot->in_audio = gst_buffer_copy_deep(args->in_audio);

  // signal render thread that there is something to do
  g_cond_signal(&state->render_queued_cond);
  g_mutex_unlock(&state->slot_lock);

  RBQueueResult result;
  if (found_slot) {
    result = is_evicted == FALSE ? RB_SUCCESS : RB_EVICTED;
  } else {
    result = RB_TIMEOUT;
  }
  return result;
}

void rb_queue_render_task_log(RBQueueArgs *args) {

  g_assert(args != NULL);

  RBRenderBuffer *state = args->render_buffer;

  g_assert(state != NULL);

  const GstClockTime start_ts = gst_util_get_timestamp();

  const RBQueueResult result = rb_queue_render_task(args);

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
  gst_object_unref(clock);
  return GST_CLOCK_DIFF(base_time, now);
}

gboolean rb_is_render_too_late(GstElement *element, const GstClockTime latency,
                               const GstClockTime running_time,
                               const GstClockTime tolerance) {

  g_assert(element != NULL);

  if (latency == GST_CLOCK_TIME_NONE) {
    return FALSE;
  }

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
 * Render one frame for the given slot.
 *
 * @param state Render buffer to use.
 * @param slot Prepared slot to render.
 *
 * @return Render duration.
 */
GstClockTime rb_render_slot(RBRenderBuffer *state, RBSlot *slot) {
  // measure rendering for QoS
  const GstClockTime render_start = gst_util_get_timestamp();

  // Dispatch slot to GL thread
  gst_gl_context_thread_add(state->gl_context, slot->gl_fill_func, slot);

  const GstClockTime render_time =
      GST_CLOCK_DIFF(render_start, gst_util_get_timestamp());

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

  return render_time;
}

/**
 * Send a video buffer to the source pad downstream.
 * Buffer is checked and timestamps are populated before sending.
 * Push is blocking for offline rendering, and for real-time rendering queued if
 * capacity is available, otherwise blocking.
 *
 * @param state Render buffer to use.
 * @param outbuf Video buffer to send downstream (takes ownership).
 * @param pts Frame PTS.
 * @param frame_duration Frame duration.
 * @return TRUE if the buffer was pushed successfully.
 */
static GstFlowReturn rb_handle_push_buffer(RBRenderBuffer *state,
                                           GstBuffer *outbuf,
                                           const GstClockTime pts,
                                           const GstClockTime frame_duration) {
  g_assert(state != NULL);

  if (gst_buffer_get_size(outbuf) == 0) {
    GST_WARNING_OBJECT(state->plugin, "Empty or invalid buffer, dropping.");
    bd_dispose_gl_buffer(&state->buffer_disposal, outbuf);
    return GST_FLOW_OK;
  }

  // populate timestamps after rendering so they can't be changed by accident
  GST_BUFFER_PTS(outbuf) = pts;
  GST_BUFFER_DTS(outbuf) = pts;
  GST_BUFFER_DURATION(outbuf) = frame_duration;

  GstFlowReturn ret;
  if (state->is_realtime) {
#if PUSH_BUFFER_ENABLED == 1
    // for real-time, we need to wait until it's time to push the buffer
    // dispatch to queue may block until capacity is available
    gboolean result = pb_queue_buffer(&state->push_buffer, outbuf);
    if (result) {
      ret = GST_FLOW_OK;
    } else {
      bd_dispose_gl_buffer(&state->buffer_disposal, outbuf);
      ret = GST_FLOW_ERROR;
    }
  } else {
#else
    // blocking wait until it's time to push the buffer,
    // then push directly
    const GstClockReturn clock_return =
        pb_wait_to_push(&state->push_buffer, pts);
    if (clock_return != GST_CLOCK_UNSUPPORTED) {
      // apply wait jitter correction ro buffer
      pb_jitter_correction(&state->push_buffer, outbuf);
    }
  }
#endif
    // push buffer downstream directly for offline rendering or if
    // queuing is disabled
    ret = gst_pad_push(state->src_pad, outbuf);
    if (ret == GST_FLOW_FLUSHING) {
      GST_INFO_OBJECT(state->plugin,
                      "Pad is flushing and does not accept buffers anymore");
    } else if (ret != GST_FLOW_OK) {
      GST_WARNING_OBJECT(state->plugin, "Failed to push buffer to pad");
    }
#if PUSH_BUFFER_ENABLED == 1
  } // endif(state->is_realtime)
#endif
  return ret;
}

/**
 * Reset render buffer references, set slot state to RB_EMPTY and signal.
 *
 * @param state Render buffer to use.
 * @param slot Slot to release.
 */
static void rb_release_slot(RBRenderBuffer *state, RBSlot *slot) {
  g_assert(state != NULL);

  // Lock and reset slot data
  g_mutex_lock(&state->slot_lock);

  slot->in_audio = NULL;
  slot->state = RB_EMPTY;
  slot->out_buf = NULL;
  slot->gl_result = FALSE;

  // let queuing know that a slot is available
  g_cond_signal(&state->slot_available_cond);
  g_mutex_unlock(&state->slot_lock);
}

GstFlowReturn rb_render_blocking(RBRenderBuffer *state, GstBuffer *in_audio,
                                 GstClockTime pts,
                                 GstClockTime frame_duration) {
  g_assert(state != NULL);

  // Lock and reset slot data
  g_mutex_lock(&state->slot_lock);

  RBSlot *slot = &state->slots[0];
  slot->in_audio = in_audio;
  slot->state = RB_BUSY;
  slot->out_buf = NULL;
  slot->pts = pts;
  slot->gl_result = FALSE;
  slot->frame_duration = frame_duration;

  // perform rendering
  rb_render_slot(state, slot);

  GstFlowReturn ret =
      rb_handle_push_buffer(state, slot->out_buf, pts, frame_duration);

  // reset slot
  slot->in_audio = NULL;
  slot->state = RB_EMPTY;
  slot->out_buf = NULL;
  slot->gl_result = FALSE;

  g_mutex_unlock(&state->slot_lock);

  return ret;
}

/**
 * Clears all render slots, releases all buffers currently held, resets the
 * render queue state and EMA state.
 * Needs to be called from the GL thread.
 *
 * @param state The render buffer to clear.
 */
static void rb_clear_slots(RBRenderBuffer *state) {
  g_assert(state != NULL);

  g_mutex_lock(&state->slot_lock);

  // clean up queue and state
  for (guint i = 0; i < NUM_RENDER_SLOTS; i++) {
    if (state->slots[i].state == RB_READY) {
      if (state->slots[i].in_audio) {
        gst_buffer_unref(state->slots[i].in_audio);
        state->slots[i].in_audio = NULL;
      }
      if (state->slots[i].out_buf) {
        bd_dispose_gl_buffer(&state->buffer_disposal, state->slots[i].out_buf);
        state->slots[i].out_buf = NULL;
      }
      state->slots[i].state = RB_EMPTY;
    }
  }

  state->render_write_idx = NUM_RENDER_SLOTS - 1;
  state->render_read_idx = NUM_RENDER_SLOTS - 1;
  state->ema_frame_counter = 0;
  state->ema_smoothed_render_time = 0;

  g_mutex_unlock(&state->slot_lock);
}

/**
 * Render thread main worker function.
 *
 * @param user_data Render buffer to work on.
 * @return NULL
 */
static gpointer _rb_render_thread_func(gpointer user_data) {

  RBRenderBuffer *state = (RBRenderBuffer *)user_data;
  g_assert(state != NULL);

#if NUM_RENDER_SLOTS > 2
  GstClockTime last_pts = GST_CLOCK_TIME_NONE;
#endif
  // slot modifications are locked

  // start working on rendering frames until we shut down
  while (g_atomic_int_get(&state->running)) {

    // first find a slot with data that's ready to render
    gboolean found_slot = FALSE;
    RBSlot *slot = NULL;
    gint render_index = 0;

    g_mutex_lock(&state->slot_lock);

    while (!found_slot) {
      render_index = (state->render_read_idx + 1) % NUM_RENDER_SLOTS;

      slot = &state->slots[render_index];

      // find a slot with audio input data
      // also check if it's already older than the last frame or if it's the
      // first frame (shouldn't happen unless the ring buffer capacity > 2)
      if (slot->state == RB_READY
#if NUM_RENDER_SLOTS > 2
          // wontfix: segment events would need to be handled for this check to
          // work right otherwise last_pts is not reset when the pts offset
          // changes. If this is ever desired, each queued frame should have an
          // incrementing id field to use for this check

          // check if next frame is already outdated, may happen if write
          // pointer jumps over the read pointer.
          && (last_pts == GST_CLOCK_TIME_NONE || slot->pts > last_pts)
#endif
      ) {
        found_slot = TRUE;
      } else {
        // no data is ready, wait for a new audio buffer being pushed
        g_cond_wait(&state->render_queued_cond, &state->slot_lock);
        if (g_atomic_int_get(&state->running) == FALSE) {
          break;
        }
      }
    }

    // no slot means we're not running anymore
    if (found_slot == FALSE) {
      g_mutex_unlock(&state->slot_lock);
      break;
    }

    // update read maker
    state->render_read_idx = render_index;
#if NUM_RENDER_SLOTS > 2
    last_pts = slot->pts;
#endif

    // nobody else is allowed to touch the slot anymore, it's owned by the
    // render thread now
    slot->state = RB_BUSY;

    g_mutex_unlock(&state->slot_lock);

    // perform gl rendering
    const GstClockTime render_time = rb_render_slot(state, slot);

    // copy params to locals vars to release the slot
    GstBuffer *audio_buffer = slot->in_audio;
    const GstClockTime frame_duration = slot->frame_duration;
    const GstClockTime pts = slot->pts;

    // copy results to locals vars to release the slot
    GstBuffer *outbuf = slot->out_buf;
    const gboolean gl_result = slot->gl_result;

    // release slot and signal
    rb_release_slot(state, slot);

    // send out buffer downstream
    // call will block if rendering is running ahead
    // and throttle render loop
    if (rb_handle_push_buffer(state, outbuf, pts, frame_duration) ==
        GST_FLOW_OK) {

      // process rendering fps QoS in case frame was pushed
      if (state->qos_enabled) {
        rb_handle_adaptive_fps_ema(state, render_time, frame_duration);
      }
    }
    outbuf = NULL;

    gst_buffer_unref(audio_buffer);

    if (!gl_result) {
      GST_WARNING_OBJECT(
          state->plugin,
          "Failed to render buffer, gl rendering returned error");
    }
  }

  return NULL;
}

/**
 * Clears all queues.
 * Needs to be called from GL thread.
 *
 * @param state Render buffer to clear.
 */
void rb_clear(RBRenderBuffer *state) {
  g_assert(state != NULL);

  rb_clear_slots(state);
#if PUSH_BUFFER_ENABLED == 1
  pb_clear_queue(&state->push_buffer);
#endif
  bd_clear(&state->buffer_disposal);
}

void rb_start(RBRenderBuffer *state) {
  g_assert(state != NULL);

  g_atomic_int_set(&state->running, TRUE);

  // threads are not needed for offline rendering
  if (state->is_realtime) {
    bd_start_buffer_disposal(&state->buffer_disposal);
    pb_start_push_buffer(&state->push_buffer);
    state->render_thread =
        g_thread_new("rb-render-thread", _rb_render_thread_func, state);
  }

  GST_INFO_OBJECT(state->plugin, "Started render buffer");
}

void rb_stop(RBRenderBuffer *state) {
  g_assert(state != NULL);

  g_atomic_int_set(&state->running, FALSE);

  // threads are not needed for offline rendering
  if (state->is_realtime) {
    // wake up render thread to signal loop exit
    g_mutex_lock(&state->slot_lock);
    g_cond_broadcast(&state->render_queued_cond);
    g_cond_broadcast(&state->slot_available_cond);
    g_mutex_unlock(&state->slot_lock);

    pb_stop_push_buffer(&state->push_buffer);

    // wait for render thread to exit
    g_thread_join(state->render_thread);
    state->render_thread = NULL;

    bd_stop_buffer_disposal(&state->buffer_disposal);
  }

  GST_INFO_OBJECT(state->plugin, "Stopped render buffer");
}

void rb_set_caps_frame_duration(RBRenderBuffer *state,
                                const GstClockTime caps_frame_duration) {
  g_assert(state != NULL);

  g_mutex_lock(&state->slot_lock);
  state->caps_frame_duration = caps_frame_duration;
  g_mutex_unlock(&state->slot_lock);
}
