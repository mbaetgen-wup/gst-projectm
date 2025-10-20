/*
 * A ring buffer based queue to schedule GL buffers to be pushed
 * downstream at presentation time (PTS). The push queue decouples the render
 * loop from buffer push timing, allowing the render loop to render frames ahead
 * up to the queue capacity. The queue is consumed by a dedicated thread
 * (pb-push-thread) to wait for the next scheduled push. The queuing call will
 * block when capacity is reached until a free slot is available, throttling the
 * render loop. Frames are never dropped.
 */

#ifndef __PUSHBUFFER_H__
#define __PUSHBUFFER_H__

#include <gst/gst.h>

#include "bufferdisposal.h"

/**
 * Max number of gl frame buffers waiting in a scheduled state to be pushed.
 * Capacity should be low (1-2) to allow back-pressure from fps increases to
 * propagate quickly.
 *
 * 0  : Disable push queuing, block render loop directly until PTS of current
 *      frame is reached.
 *
 * >0 : Allow n buffers waiting in the queue for pushing while render thread
 *      continues.
 */
#ifndef PUSH_QUEUE_SIZE
#define PUSH_QUEUE_SIZE 1
#endif

/**
 * Push buffer state.
 */
typedef struct {

  // not re-assigned during push buffer lifetime
  // --------------------------------------------------------------

  /**
   * projectM plugin. No ownership.
   */
  BDBufferDisposal *buffer_disposal;

  /**
   * projectM plugin. No ownership.
   */
  GstObject *plugin;

  /**
   * projectM plugin source pad. No ownership.
   */
  GstPad *src_pad;

  /**
   * Thread for pushing gl buffers downstream.
   * Used for real-time, pushing needs to be scheduled to be synchronized with
   * the pipeline clock.
   */
  GThread *push_thread;

  /**
   * Ring buffer to schedule gl buffers for pushing.
   */
  GstBuffer *push_queue[PUSH_QUEUE_SIZE];

  /**
   * Mutex for push ring buffer.
   */
  GMutex push_queue_mutex;

  /**
   * Condition signaled when a buffer has been queued.
   */
  GCond push_queue_cond;

  /**
   * Condition signaled when a buffer has been pushed
   * and a slot is free.
   */
  GCond push_queue_free_cond;

  /**
   * Clock to use for scheduling.
   */
  GstClock *clock;

  // concurrent access, g_atomic
  // --------------------------------------------------------------

  /**
   * TRUE if rendering is currently running.
   */
  gboolean running;

  // concurrent access, protected by push_queue_mutex
  // --------------------------------------------------------------

  /**
   * Push ring buffer write position.
   */
  gint push_queue_write_idx;

  /**
   * Push ring buffer read position.
   */
  gint push_queue_read_idx;

  // used only by either render (offline) or push thread (real-time)
  // --------------------------------------------------------------

  /**
   * EMA based clock jitter average.
   */
  GstClockTimeDiff avg_jitter;

  /**
   * Clock jitter initialized.
   */
  gboolean avg_jitter_init;

} PBPushBuffer;

/**
 * Schedule a rendered gl buffer for getting pushed downstream.
 * The buffer will not be pushed until it's PTS time is reached.
 * This call will block until the buffer can be scheduled or
 * the render buffer is stopped.
 *
 * @param state The push buffer to use.
 * @param buffer The gl buffer to push. Takes ownership of the buffer.
 *
 * @return TRUE if the buffer was scheduled successfully, FALSE in case the
 *         buffer was stopped.
 */
gboolean pb_queue_buffer(PBPushBuffer *state, GstBuffer *buffer);

/**
 * Removes and disposes all queued buffers and resets queue state.
 *
 * @param state State to clear.
 */
void pb_clear_queue(PBPushBuffer *state);

/**
 * Applies jitter correction to the given buffer.
 *
 * @param state Current push buffer.
 * @param outbuf Buffer to apply correction to.
 */
void pb_jitter_correction(PBPushBuffer *state, GstBuffer *outbuf);

/**
 * Calculate current clock jitter average.
 *
 * @param state Current push buffer.
 * @param jitter Latest jitter value.
 */
void pb_calculate_avg_jitter(PBPushBuffer *state, GstClockTimeDiff jitter);

/**
 * Wait until reaching the given PTS. The clock jitter is recorded if it's
 * appropriate to do so.
 *
 * @param state Push buffer to use.
 * @param pts Wait until this PTS is reached.
 * @param locked Controls if unlocking is needed before entering wait.
 *
 * @return The clock wait result.
 */
GstClockReturn pb_wait_to_push(PBPushBuffer *state, GstClockTime pts);

/**
 * Init this push buffer.
 *
 * @param state Push buffer to use.
 * @param buffer_cleanup Buffer disposal to use.
 * @param plugin Context gst plugin element.
 * @param src_pad Source pad to push buffers to.
 */
void pb_init_push_buffer(PBPushBuffer *state, BDBufferDisposal *buffer_cleanup,
                         GstObject *plugin, GstPad *src_pad);

/**
 * Release all respources for this push buffer.
 *
 * @param state Push buffer to use.
 */
void pb_dispose_push_buffer(PBPushBuffer *state);

/**
 * Start push buffer worker thread.
 *
 * @param state Push buffer to use.
 */
void pb_start_push_buffer(PBPushBuffer *state);

/**
 * Stop push buffer worker thread.
 *
 * @param state Push buffer to use.
 */
void pb_stop_push_buffer(PBPushBuffer *state);

#endif
