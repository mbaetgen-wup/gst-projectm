/*
 * A ring buffer based render buffer to allow offloading of
 * rendering tasks from the plugin chain function. The ring buffer consists of
 * a limited number of rendering slots.
 *
 *  For offline pipelines only:
 *
 * - A blocking call is used for rendering, bypasses queuing.
 *
 *  For real-time pipelines only:
 *
 * The buffer provides queueing for audio buffers to be rendered to video
 * frames. It uses a bound-wait-on-full approach to avoid dropping frames when
 * rendering duration exceeds the frame duration of the current fps:
 *
 * - In case a free slot is available queue
 *   immediately and return (async rendering).
 *
 * - In case the next available (not rendering) slot is scheduled (end of the
 *   ring + 1):
 *
 *   - Wait for defined time for a slot to become
 *     available, this wait may not exceed the current fps frame duration,
 *     otherwise the plugin loses audio sync and fails.
 *
 *   - In case the max wait deadline is met,
 *     and the next buffer still hasn't been picked up, it is overridden
 *     with the current frame (evicted), meaning the previous frame is being
 *     dropped as it is too late.
 *
 * ---
 *
 *  - If the render duration exceeds the fps *sometimes*, subsequent
 *    faster-than-real-time rendered frames (if any) compensate for the small
 *    lag, frames are dropped or eventually QoS events from the downstream
 *    sink will re-sync with the pipeline clock.
 *
 *  - If the render duration exceeds the fps *most of the time*, an Exponential
 *    Moving Average (EMA) based algorithm instructs the plugin to reduce fps.
 *    EMA will also recover fps when render performance increases again.
 */

#ifndef __RENDERBUFFER_H__
#define __RENDERBUFFER_H__

#include <gst/gl/gl.h>
#include <gst/gst.h>

G_BEGIN_DECLS

/**
 * Number of render slots that are used by the ring buffer.
 * 2 is the ideal size and there should be no reason to change it:
 * One slot for the gl thread to render the current frame while another slot is
 * available for queuing the next audio buffer to render.
 *
 * Note: Increasing the number of slots >2 is not fully supported.
 * GstSegments won't be handled correctly currently. See inline code comments.
 *
 * Valid values:
 *  1 - Wait for previous render to complete before scheduling.
 *  2 - Render and schedule one item and at the same time.
 */
#ifndef NUM_RENDER_SLOTS
#define NUM_RENDER_SLOTS 2
#endif

/**
 * Callback function pointer type for triggering a dynamic fps change.
 */
typedef void (*RBAdjustFpsFunc)(gpointer user_data, guint64 frame_duration);

/**
 * Current usage state of a render slot.
 */
typedef enum {
  /**
   * Slot is not in use at all.
   */
  RB_EMPTY,

  /**
   * Ready to render, in_audio buffer is filled.
   */
  RB_READY,

  /**
   * Slot is currently being rendered.
   */
  RB_BUSY
} RQSlotState;

/**
 * Result status of queuing a buffer for rendering.
 */
typedef enum {
  /**
   * Buffer has been queued.
   */
  RB_SUCCESS,

  /**
   * Queuing buffer evicted (overwrote) a previously queued buffer (frame drop).
   */
  RB_EVICTED,

  /**
   * Buffer could not be queued because the allowed wait could not be met.
   */
  RB_TIMEOUT
} RBQueueResult;

/**
 * A render slot represents an item in the render buffer. It holds an audio
 * input buffer used for a single frame, render context information like frame
 * pts and duration, and an output buffer for the rendered video frame.
 */
typedef struct {

  // not re-assigned
  // --------------------------------------------------------------

  /**
   * projectM plugin.
   */
  GstObject *plugin;

  /**
   * Callback to render to gl texture buffer.
   */
  GstGLContextThreadFunc gl_fill_func;

  // input for rendering, updated by queuing for each frame
  // --------------------------------------------------------------

  /**
   * Presentation timestamp for this video frame.
   */
  GstClockTime pts;

  /**
   * Duration for this video frame (current fps).
   */
  GstClockTime frame_duration;

  /**
   * Audio data to feed to projectM for this frame.
   */
  GstBuffer *in_audio;

  // output from rendering, updated by gl thread for each frame
  // --------------------------------------------------------------

  /**
   * GL memory texture buffer for current frame.
   */
  GstBuffer *out_buf;

  /**
   * GL render result for current frame.
   */
  gboolean gl_result;

  // frequently updated, more than once for each frame
  // --------------------------------------------------------------

  /**
   * Usage state of this slot.
   */
  RQSlotState state;

} RBSlot;

/**
 * All render buffer data.
 */
typedef struct {

  // not re-assigned during render thread lifetime
  // --------------------------------------------------------------

  /**
   * projectM plugin. No ownership.
   */
  GstObject *plugin;

  /**
   * Current gl context. No ownership.
   */
  GstGLContext *gl_context;

  /**
   * projectM plugin source pad. No ownership.
   */
  GstPad *src_pad;

  /**
   * Thread running the render loop.
   */
  GThread *render_thread;

  /**
   * Thread running the gl buffer clean-up loop,
   * used to release dropped buffer from the gl thread.
   */
  GThread *cleanup_thread;

  /**
   * Thread for pushing gl buffers downstream.
   * Used for real-time, pushing needs to be scheduled to be synchronized with
   * the pipeline clock.
   */
  GThread *push_thread;

  /**
   * Queue to schedule gl buffers for pushing.
   */
  GAsyncQueue *buffer_push_queue;

  /**
   * Queue to dispose of dropped gl buffers.
   */
  GAsyncQueue *buffer_cleanup_queue;

  /**
   * Callback function pointer to let the plugin know to change fps.
   */
  RBAdjustFpsFunc adjust_fps_func;

  /**
   * Lock for shared state between chain function and render thread.
   */
  GMutex slot_lock;

  // concurrent access, g_atomic
  // --------------------------------------------------------------

  /**
   * TRUE if render thread is currently running.
   */
  gboolean running;

  // concurrent access, protected by slot_lock
  // --------------------------------------------------------------
  /**
   * Condition to wait for a buffer to queued for rendering.
   */
  GCond render_queued_cond;

  /**
   * Condition for slots becoming available after rendering completed.
   */
  GCond slot_available_cond;

  /**
   * Switch for real-time (render loop) QoS.
   */
  gboolean qos_enabled;

  /**
   * Is current pipeline using a real-time clock.
   */
  gboolean is_realtime;

  /**
   * Pipeline negotiated caps fps as frame duration.
   */
  GstClockTime caps_frame_duration;

  /**
   * Limit for max EMA fps changes as frame duration. Higher value = lower fps.
   */
  GstClockTime max_frame_duration;

  /**
   * Render ring buffer slots.
   */
  RBSlot slots[NUM_RENDER_SLOTS];

  // only used by the calling thread (chain function)
  // --------------------------------------------------------------

  /**
   * Last index that data was inserted at (insertion pointer).
   */
  gint last_insert_index;

  // only used by the render thread
  // --------------------------------------------------------------

  /**
   * Last index that data was rendered from (read pointer).
   */
  gint last_render_index;

  /**
   * EMA frame counter.
   */
  guint frame_counter;

  /**
   * EMA running average.
   */
  guint64 smoothed_render_time;

} RBRenderBuffer;

/**
 * Call argument struct, input for queuing a frame for rendering.
 */
typedef struct {

  /**
   * Render buffer to use.
   */
  RBRenderBuffer *render_buffer;

  /**
   * Max time to wait for queuing.
   */
  GstClockTime max_wait;

  /**
   * Presentation timestamp for this video frame.
   */
  GstClockTime pts;

  /**
   * Duration for this video frame (current fps).
   */
  GstClockTime frame_duration;

  /**
   * Audio data to feed to projectM for this frame.
   */
  GstBuffer *in_audio;

} RBQueueArgs;

/**
 * One time initialization for the given render buffer.
 *
 * @param state Render buffer to use.
 * @param plugin Plugin using the render buffer.
 * @param gl_fill_func GL rendering function callback.
 * @param adjust_fps_func FPS adjustment function callback.
 * @param max_frame_duration FPS adjustment lower limit.
 * @param caps_frame_duration FPS requested by pipeline caps.
 * @param is_qos_enabled Controls if render-time QoS is enabled (EMA).
 */
void rb_init_render_buffer(RBRenderBuffer *state, GstObject *plugin,
                           GstGLContextThreadFunc gl_fill_func,
                           RBAdjustFpsFunc adjust_fps_func,
                           GstClockTime max_frame_duration,
                           GstClockTime caps_frame_duration,
                           gboolean is_qos_enabled, gboolean is_realtime);

/**
 * Release resources for the given render buffer.
 *
 * @param state Render buffer to clean up.
 */
void rb_dispose_render_buffer(RBRenderBuffer *state);

/**
 * Queue an audio buffer for rendering. The queuing is guaranteed to return
 * within the given max time budget. The buffer will be dropped if queuing is
 * not possible within the given time budget.
 *
 * @param args Audio buffer and frame details for rendering. The render buffer
 * does not take ownership of the given pointer. The given audio buffer is
 * copied.
 */
RBQueueResult rb_queue_render_task(RBQueueArgs *args);

/**
 * Queue an audio buffer for rendering. The queuing is guaranteed to return
 * within the given max time budget. The buffer will be dropped if queuing is
 * not possible within the given time budget.
 *
 * Convenience function that also handles queuing result by logging if frames
 * are dropped (DEBUG level).
 *
 * @param args Audio buffer and frame details for rendering. The render buffer
 * does not take ownership of the given pointer. The given audio buffer is
 * copied.
 */
void rb_queue_render_task_log(RBQueueArgs *args);

/**
 * Render one frame synchronously. Using synchronous rendering is exclusive,
 * queuing may not be used with the same render buffer at the same time.
 *
 * @param state Render buffer to use.
 * @param pts Frame PTS.
 * @param frame_duration Frame duration.
 * @return The downstream push result.
 */
GstFlowReturn rb_render_blocking(RBRenderBuffer *state, GstBuffer *in_audio,
                                 GstClockTime pts, GstClockTime frame_duration);

/**
 * Determine if it's likely too late push a buffer, as it would likely be
 * dropped by a pipeline synchronized sink.
 *
 * @param element The plugin element.
 * @param latency Pipeline latency.
 * @param running_time Current buffer running time.
 * @param tolerance Tolerance to account for scheduling overhead.
 * @return TRUE in case the buffer is too late.
 */
static gboolean rb_is_render_too_late(GstElement *element, GstClockTime latency,
                                      GstClockTime running_time,
                                      GstClockTime tolerance);

/**
 * Start render loop.
 *
 * @param state Render buffer to use.
 * @param gl_context GL context to use for rendering.
 * @param src_pad Source pad to push video buffers to.
 */
void rb_start(RBRenderBuffer *state, GstGLContext *gl_context, GstPad *src_pad);

/**
 * Stop render loop. Active threads will be joined before returning.
 *
 * @param state Render buffer to use.
 */
void rb_stop(RBRenderBuffer *state);

/**
 * Update caps as they get negotiated by the pipeline. Thread safe.
 *
 * @param state Render buffer to update.
 * @param caps_frame_duration Frame duration from pipeline caps.
 */
void rb_set_caps_frame_duration(RBRenderBuffer *state,
                                GstClockTime caps_frame_duration);

G_END_DECLS

#endif // __RENDERBUFFER_H__
