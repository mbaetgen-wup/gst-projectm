
#include "pushbuffer.h"

#include "bufferdisposal.h"

GST_DEBUG_CATEGORY_STATIC(pushbuffer_debug);
#define GST_CAT_DEFAULT pushbuffer_debug

/**
 * EMA aloha for push schedule clock jitter average.
 */
#ifndef JITTER_EMA_ALPHA
#define JITTER_EMA_ALPHA 0.75
#endif

/**
 * EMA for push schedule clock jitter outlier threshold.
 */
#ifndef JITTER_EMA_OUTLIER_THRESHOLD
#define JITTER_EMA_OUTLIER_THRESHOLD (5 * GST_MSECOND)
#endif

/**
 * Tolerance / minimal wait time for scheduling a timed wait before pushing a
 * buffer. If the calculated wait time is less than this value, the wait will be
 * skipped.
 */
#ifndef MIN_PUSH_SCHEDULE_WAIT
#define MIN_PUSH_SCHEDULE_WAIT (GST_USECOND * 50)
#endif

gboolean pb_queue_buffer(PBPushBuffer *state, GstBuffer *buffer) {
  g_assert(state != NULL);
  g_assert(buffer != NULL);

  g_mutex_lock(&state->push_queue_mutex);

  // write to the next position in the ring
  state->push_queue_write_idx =
      (state->push_queue_write_idx + 1) % PUSH_QUEUE_SIZE;

  gboolean result = TRUE;
  // wait until next position is free
  while (state->push_queue[state->push_queue_write_idx] != NULL) {
    g_cond_wait(&state->push_queue_free_cond, &state->push_queue_mutex);
    if (!g_atomic_int_get(&state->running)) {
      result = FALSE;
      break;
    }
  }

  if (result) {
    // take the spot and signal that queue has changed
    state->push_queue[state->push_queue_write_idx] = buffer;
    g_cond_signal(&state->push_queue_cond);
  }

  g_mutex_unlock(&state->push_queue_mutex);

  return result;
}

void pb_calculate_avg_jitter(PBPushBuffer *state,
                             const GstClockTimeDiff jitter) {
  // ignore outliers
  if (ABS(jitter) > JITTER_EMA_OUTLIER_THRESHOLD)
    return;

  if (!state->avg_jitter_init) {
    state->avg_jitter = (gdouble)jitter;
    state->avg_jitter_init = TRUE;
  } else {
    state->avg_jitter = JITTER_EMA_ALPHA * state->avg_jitter +
                        (1.0 - JITTER_EMA_ALPHA) * (gdouble)jitter;
  }
}

static void pb_jitter_correction(PBPushBuffer *state, GstBuffer *outbuf) {

  if (GST_BUFFER_PTS(outbuf) != GST_CLOCK_TIME_NONE) {
    GstClockTime correction = llabs((guint64)state->avg_jitter);

    if (state->avg_jitter > 0.0) {
      GST_BUFFER_PTS(outbuf) -= correction;
    } else {
      GST_BUFFER_PTS(outbuf) += correction;
    }
  }
}

/**
 * Consume buffers to push and wait until it's PTS time to push.
 * Used for real-time rendering only.
 *
 *
 * @param user_data Render buffer to use.
 * @return NULL
 */
static gpointer rb_push_thread_func(gpointer user_data) {

  PBPushBuffer *state = (PBPushBuffer *)user_data;
  g_assert(state != NULL);

  g_mutex_lock(&state->push_queue_mutex);

  while (g_atomic_int_get(&state->running)) {

    state->push_queue_read_idx =
        (state->push_queue_read_idx + 1) % PUSH_QUEUE_SIZE;

    // consume gl buffer to push
    gboolean stop = FALSE;
    while (state->push_queue[state->push_queue_read_idx] == NULL) {
      // no buffer to push, wait for one
      g_cond_wait(&state->push_queue_cond, &state->push_queue_mutex);
      if (!g_atomic_int_get(&state->running)) {
        stop = TRUE;
        break;
      }
    }

    if (stop) {
      break;
    }

    // found a buffer to push
    GstBuffer *outbuf = state->push_queue[state->push_queue_read_idx];

    // determine when it's time to push
    const GstClockTime pts = GST_BUFFER_PTS(outbuf);
    if (pts != GST_CLOCK_TIME_NONE) {
      GstClock *clock = gst_element_get_clock(GST_ELEMENT(state->plugin));
      if (clock) {
        const GstClockTime base_time =
            gst_element_get_base_time(GST_ELEMENT(state->plugin));

        const GstClockTime abs_time = pts + base_time;

        const GstClockTimeDiff remaining_wait =
            GST_CLOCK_DIFF(gst_clock_get_time(clock), abs_time);

        if (remaining_wait > MIN_PUSH_SCHEDULE_WAIT) {
          // we need to wait, unlock first
          g_mutex_unlock(&state->push_queue_mutex);

          GstClockTimeDiff jitter = 0;
          const GstClockID clock_id =
              gst_clock_new_single_shot_id(clock, abs_time);
          const GstClockReturn clock_return =
              gst_clock_id_wait(clock_id, &jitter);
          gst_clock_id_unref(clock_id);

          if (clock_return == GST_CLOCK_OK || clock_return == GST_CLOCK_EARLY) {
            // record jitter
            pb_calculate_avg_jitter(state, jitter);
          } else if (clock_return == GST_CLOCK_UNSCHEDULED) {
            // drop buffer if clock is not running
            g_mutex_lock(&state->push_queue_mutex);
            if (state->push_queue[state->push_queue_read_idx] == outbuf) {
              state->push_queue[state->push_queue_read_idx] = NULL;
              bd_queue_gl_buffer_disposal(state->buffer_disposal, outbuf);
            }
            gst_object_unref(clock);
            continue;
          }
          g_mutex_lock(&state->push_queue_mutex);
        }
      }
      gst_object_unref(clock);
    }

    // now we own the buffer to push
    state->push_queue[state->push_queue_read_idx] = NULL;
    g_cond_signal(&state->push_queue_free_cond);
    g_mutex_unlock(&state->push_queue_mutex);

    // apply wait jitter correction ro buffer
    pb_jitter_correction(state, outbuf);

    // push buffer downstream
    const GstFlowReturn ret = gst_pad_push(state->src_pad, outbuf);

    if (ret == GST_FLOW_FLUSHING) {
      GST_INFO_OBJECT(state->plugin,
                      "Pad is flushing and does not accept buffers anymore");
    } else if (ret != GST_FLOW_OK) {
      GST_WARNING_OBJECT(state->plugin, "Failed to push buffer to pad");
    }

    g_mutex_lock(&state->push_queue_mutex);
  }

  g_mutex_unlock(&state->push_queue_mutex);

  return NULL;
}

void pb_init_push_buffer(PBPushBuffer *state, BDBufferDisposal *buffer_cleanup,
                         GstObject *plugin, GstPad *src_pad) {

  GST_DEBUG_CATEGORY_INIT(pushbuffer_debug, "pushbuffer", 0,
                          "projectM visualizer plugin push buffer");

  state->buffer_disposal = buffer_cleanup;
  state->plugin = plugin;
  state->src_pad = src_pad;

  // init push queue
  state->push_thread = NULL;
  state->push_queue_read_idx = -1;
  state->push_queue_write_idx = -1;
  g_mutex_init(&state->push_queue_mutex);
  g_cond_init(&state->push_queue_cond);
  g_cond_init(&state->push_queue_free_cond);
  for (guint i = 0; i < PUSH_QUEUE_SIZE; i++) {
    state->push_queue[i] = NULL;
  }

  state->avg_jitter = 0.0;
  state->avg_jitter_init = FALSE;
}

void pb_dispose_push_buffer(PBPushBuffer *state) {

  g_cond_clear(&state->push_queue_cond);
  g_cond_clear(&state->push_queue_free_cond);
  g_mutex_clear(&state->push_queue_mutex);

  state->buffer_disposal = NULL;
  state->plugin = NULL;
  state->src_pad = NULL;
}

void pb_clear_queue(PBPushBuffer *state) {
  g_assert(state != NULL);
  g_mutex_lock(&state->push_queue_mutex);

  // release buffers that are still queued before cleanup thread shuts down
  for (guint i = 0; i < PUSH_QUEUE_SIZE; i++) {
    if (state->push_queue[i] != NULL) {
      bd_queue_gl_buffer_disposal(state->buffer_disposal, state->push_queue[i]);
      state->push_queue[i] = NULL;
    }
  }
  state->push_queue_read_idx = -1;
  state->push_queue_write_idx = -1;
  state->avg_jitter = 0.0;
  state->avg_jitter_init = FALSE;

  g_mutex_unlock(&state->push_queue_mutex);
}

void pb_start_push_buffer(PBPushBuffer *state) {
  g_atomic_int_set(&state->running, TRUE);

  state->push_thread =
      g_thread_new("pb-push-thread", rb_push_thread_func, state);
}

void pb_stop_push_buffer(PBPushBuffer *state) {
  g_atomic_int_set(&state->running, FALSE);

  // signal wake up push thread to signal loop exit
  g_mutex_lock(&state->push_queue_mutex);
  g_cond_broadcast(&state->push_queue_cond);
  g_cond_broadcast(&state->push_queue_free_cond);
  g_mutex_unlock(&state->push_queue_mutex);
  // wait for push thread to exit
  g_thread_join(state->push_thread);
  state->push_thread = NULL;
}
