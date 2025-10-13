
#include "bufferdisposal.h"

GST_DEBUG_CATEGORY_STATIC(buffercleanup_debug);
#define GST_CAT_DEFAULT buffercleanup_debug

/**
 * Queue shutdown signal token.
 */
static gpointer BC_Q_SHUTDOWN_SIGNAL = &BC_Q_SHUTDOWN_SIGNAL;

void bd_queue_gl_buffer_disposal(BDBufferDisposal *state, GstBuffer *buf) {
  g_assert(state != NULL);
  g_assert(buf != NULL);
  g_async_queue_push(state->disposal_queue, buf);
}

/**
 * Callback for scheduling gl buffer release with gl thread.
 * Needs to be called from the GL thread.
 *
 * @param context Current gl context.
 * @param buf GL buffer to release.
 */
void bd_gl_buffer_dispose_gl(GstGLContext *context, gpointer buf) {
  (void)context;
  gst_buffer_unref(GST_BUFFER(buf));
}

/**
 * Used to dispose of dropped gl buffers that are not making it to the src pad.
 * Consume buffers to clean-up and dispatch release through gl thread.
 *
 * @param user_data Queue state to use.
 * @return NULL
 */
static gpointer bd_dispose_thread_func(gpointer user_data) {

  BDBufferDisposal *state = (BDBufferDisposal *)user_data;
  g_assert(state != NULL);

  // consume gl buffers to dispatch to gl thread for cleanup
  while (g_atomic_int_get(&state->running)) {

    gpointer item = g_async_queue_pop(state->disposal_queue);

    if (!item || item == BC_Q_SHUTDOWN_SIGNAL)
      continue;

    gst_gl_context_thread_add(state->gl_context, bd_gl_buffer_dispose_gl, item);
  }
  return NULL;
}

/**
 * Dispose of all buffered currently queued.
 * Needs to be called from the GL thread.
 *
 * @param user_data Queue state to use.
 * @return NULL
 */
void bd_clear_queue_gl(GstGLContext* context, gpointer user_data) {
  BDBufferDisposal *state = (BDBufferDisposal *)user_data;
  g_assert(state != NULL);

  // make sure all gl buffers are released
  gpointer item;
  while ((item = g_async_queue_try_pop(state->disposal_queue)) != NULL) {
    bd_gl_buffer_dispose_gl(context, item);
  }
}

void bd_clear_disposal_queue(BDBufferDisposal *state) {
  gst_gl_context_thread_add(state->gl_context, bd_clear_queue_gl, state);
}

void bd_init_buffer_disposal(BDBufferDisposal *state,
                             GstGLContext *gl_context) {

  GST_DEBUG_CATEGORY_INIT(buffercleanup_debug, "buffercleanup", 0,
                          "projectM visualizer plugin buffer cleanup");

  // init clean up queue
  state->disposal_thread = NULL;
  state->gl_context = gl_context;
  g_atomic_int_set(&state->running, FALSE);
  state->disposal_queue = g_async_queue_new();
}

void bd_dispose_buffer_disposal(BDBufferDisposal *state) {
  g_async_queue_unref(state->disposal_queue);
  state->disposal_queue = NULL;
  state->gl_context = NULL;
}

void bd_start_buffer_disposal(BDBufferDisposal *state) {

  g_atomic_int_set(&state->running, TRUE);

  state->disposal_thread =
      g_thread_new("rb-cleanup-thread", bd_dispose_thread_func, state);
}

void bd_stop_buffer_disposal(BDBufferDisposal *state) {
  // signal and wait for cleanup thread to exit
  g_atomic_int_set(&state->running, FALSE);

  g_async_queue_push(state->disposal_queue, BC_Q_SHUTDOWN_SIGNAL);
  g_thread_join(state->disposal_thread);
  state->disposal_thread = NULL;
}