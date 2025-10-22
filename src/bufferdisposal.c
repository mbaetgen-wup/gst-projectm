#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "bufferdisposal.h"

#include <gst/gl/gl.h>

GST_DEBUG_CATEGORY_STATIC(buffercleanup_debug);
#define GST_CAT_DEFAULT buffercleanup_debug

/**
 * Queue shutdown signal token.
 */
static gpointer BD_Q_SHUTDOWN_SIGNAL = &BD_Q_SHUTDOWN_SIGNAL;

/**
 * Callback for scheduling gl buffer release with gl thread.
 * Needs to be called from the GL thread.
 *
 * @param context Current gl context.
 * @param buf GL buffer to release.
 */
void bd_gl_buffer_dispose_gl(GstGLContext *context, gpointer buf) {

  GstBuffer *buffer = GST_BUFFER(buf);
  if (buffer == NULL)
    return;

  if (context != NULL) {
    GstGLSyncMeta *sync_meta = gst_buffer_get_gl_sync_meta(buffer);
    if (sync_meta)
      gst_gl_sync_meta_set_sync_point(sync_meta, context);
  }

  gst_buffer_unref(GST_BUFFER(buffer));
}

void bd_dispose_gl_buffer(BDBufferDisposal *state, GstBuffer *buf) {
  g_assert(state != NULL);
  g_assert(buf != NULL);
  if (gst_gl_context_get_current() == state->gl_context) {
    bd_gl_buffer_dispose_gl(state->gl_context, buf);
  } else {
    g_async_queue_push(state->disposal_queue, buf);
  }
}

/**
 * Disposal loop for dropped gl buffers that are not making it to the src pad.
 * Consume buffers to clean-up and dispatch release through gl thread.
 *
 * @param user_data Queue state to use.
 * @return NULL
 */
static gpointer _bd_dispose_thread_func(gpointer user_data) {

  BDBufferDisposal *state = (BDBufferDisposal *)user_data;
  g_assert(state != NULL);

  // consume gl buffers to dispatch to gl thread for cleanup
  while (g_atomic_int_get(&state->running)) {

    gpointer item = g_async_queue_pop(state->disposal_queue);

    if (item == BD_Q_SHUTDOWN_SIGNAL)
      break;

    if (!item)
      continue;

    gst_gl_context_thread_add(state->gl_context, bd_gl_buffer_dispose_gl, item);
  }

  return NULL;
}

/**
 * Dispose of all buffers currently queued.
 * Needs to be called from the GL thread.
 *
 * @param user_data Queue state to use.
 */
void bd_clear_queue_gl(GstGLContext *context, gpointer user_data) {
  BDBufferDisposal *state = (BDBufferDisposal *)user_data;
  g_assert(state != NULL);
  g_assert(gst_gl_context_get_current() == context);

  // make sure all gl buffers are released
  gpointer item;
  while ((item = g_async_queue_try_pop(state->disposal_queue)) != NULL) {
    bd_gl_buffer_dispose_gl(context, item);
  }
}

void bd_clear(BDBufferDisposal *state) {
  g_assert(state != NULL);

  if (gst_gl_context_get_current() == state->gl_context) {
    bd_clear_queue_gl(state->gl_context, state);
  } else {
    gst_gl_context_thread_add(state->gl_context, bd_clear_queue_gl, state);
  }
}

void bd_init_buffer_disposal(BDBufferDisposal *state,
                             GstGLContext *gl_context) {
  g_assert(state != NULL);

  static gsize _debug_initialized = 0;
  if (g_once_init_enter(&_debug_initialized)) {
    GST_DEBUG_CATEGORY_INIT(buffercleanup_debug, "buffercleanup", 0,
                            "projectM visualizer plugin buffer cleanup");
  }

  state->disposal_thread = NULL;
  state->gl_context = gl_context;
  g_atomic_int_set(&state->running, FALSE);
  state->disposal_queue = g_async_queue_new();
}

void bd_dispose_buffer_disposal(BDBufferDisposal *state) {
  g_assert(state != NULL);
  g_assert(state->disposal_thread == NULL);

  g_async_queue_unref(state->disposal_queue);
  state->disposal_queue = NULL;
  state->gl_context = NULL;
}

void bd_start_buffer_disposal(BDBufferDisposal *state) {
  g_assert(state != NULL);

  g_atomic_int_set(&state->running, TRUE);

  if (state->disposal_thread == NULL) {
    state->disposal_thread =
        g_thread_new("bd-disposal-thread", _bd_dispose_thread_func, state);
  }
}

void bd_stop_buffer_disposal(BDBufferDisposal *state) {
  g_assert(state != NULL);

  // signal and wait for cleanup thread to exit
  g_atomic_int_set(&state->running, FALSE);

  if (state->disposal_thread != NULL) {
    g_async_queue_push(state->disposal_queue, BD_Q_SHUTDOWN_SIGNAL);
    g_thread_join(state->disposal_thread);
    state->disposal_thread = NULL;
  }
}
