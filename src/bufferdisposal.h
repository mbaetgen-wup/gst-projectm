/*
 * An async queue to dispose of (dropped) GL buffers. The queue is consumed by
 * a dedicated thread (bd-disposal-thread) to dispatch GL buffer unref to the
 * GL thread.
 */

#ifndef __BUFFERDISPOSAL_H__
#define __BUFFERDISPOSAL_H__

#include <gst/gl/gstgl_fwd.h>
#include <gst/gst.h>

typedef struct {

  // not re-assigned during render thread lifetime
  // --------------------------------------------------------------

  /**
   * Current gl context. No ownership.
   */
  GstGLContext *gl_context;

  /**
   * Thread running the gl buffer clean-up loop,
   * used to release dropped buffer from the gl thread.
   */
  GThread *disposal_thread;

  /**
   * Queue to dispose of dropped gl buffers.
   */
  GAsyncQueue *disposal_queue;

  // concurrent access, g_atomic
  // --------------------------------------------------------------

  /**
   * TRUE if rendering is currently running.
   */
  gboolean running;

  /*< private >*/
  gpointer _padding[GST_PADDING];
} BDBufferDisposal;

/**
 * Dispose given buffer from the GL thread.
 * Disposal will be queued if current thread is not the GL thread.
 *
 * @param state State to use.
 * @param buf Buffer to dispose.
 */
void bd_dispose_gl_buffer(BDBufferDisposal *state, GstBuffer *buf);

/**
 * Dispose of all buffers currently queued for disposal.
 *
 * @param state Renderbuffer owning cleanup queue to clear.
 */
void bd_clear(BDBufferDisposal *state);

/**
 * Init queue state.
 *
 * @param state Queue state to init.
 * @param gl_context GL context to use.
 */
void bd_init_buffer_disposal(BDBufferDisposal *state, GstGLContext *gl_context);

/**
 * Release all resources used by this queue.
 *
 * @param state Queue state to dispose.
 */
void bd_dispose_buffer_disposal(BDBufferDisposal *state);

/**
 * Start worker thread.
 *
 * @param state Queue state to use.
 */
void bd_start_buffer_disposal(BDBufferDisposal *state);

/**
 * Stop worker thread.
 *
 * @param state Queue state to use.
 */
void bd_stop_buffer_disposal(BDBufferDisposal *state);

#endif
