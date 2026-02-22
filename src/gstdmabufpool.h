/*
 * GStreamer DMABuf Buffer Pool
 *
 * Unified DMABuf buffer pool supporting RGBA (GBM_FORMAT_ARGB8888)
 * and NV12 (GBM_FORMAT_NV12) allocations via GBM. Buffers are backed
 * by GBM buffer objects with per-plane DMABuf file descriptors.
 *
 * This pool is used by the GL base audio visualizer for zero-copy
 * output to VA-API encoders and other DMABuf consumers.
 *
 * Thread safety: Pool state is protected by an internal mutex.
 * No GL calls are made inside pool allocation.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 */

#ifndef __GST_DMABUF_POOL_H__
#define __GST_DMABUF_POOL_H__

#ifdef HAVE_DMABUF

#include <gst/gst.h>
#include <gst/video/video.h>
#include <gst/allocators/gstdmabuf.h>

#include <gbm.h>

G_BEGIN_DECLS

#define GST_TYPE_DMABUF_POOL (gst_dmabuf_pool_get_type())
#define GST_DMABUF_POOL(obj) \
  (G_TYPE_CHECK_INSTANCE_CAST((obj), GST_TYPE_DMABUF_POOL, GstDmaBufPool))
#define GST_DMABUF_POOL_CLASS(klass) \
  (G_TYPE_CHECK_CLASS_CAST((klass), GST_TYPE_DMABUF_POOL, GstDmaBufPoolClass))
#define GST_IS_DMABUF_POOL(obj) \
  (G_TYPE_CHECK_INSTANCE_TYPE((obj), GST_TYPE_DMABUF_POOL))

/**
 * Output mode for the DMABuf pool.
 */
typedef enum {
  /**
   * Standard GLMemory RGBA output (existing path).
   */
  GST_DMABUF_MODE_GLMEMORY_RGBA,

  /**
   * DMABuf RGBA output (ARGB8888 GBM format).
   */
  GST_DMABUF_MODE_DMABUF_RGBA,

  /**
   * DMABuf NV12 output (NV12 GBM format) for VA encoders.
   */
  GST_DMABUF_MODE_DMABUF_NV12
} GstDmaBufOutputMode;

typedef struct _GstDmaBufPool GstDmaBufPool;
typedef struct _GstDmaBufPoolClass GstDmaBufPoolClass;
typedef struct _GstDmaBufPoolPrivate GstDmaBufPoolPrivate;

/**
 * GstDmaBufPool:
 *
 * A buffer pool that allocates DMABuf-backed buffers via GBM.
 * Supports both RGBA and NV12 formats. Allocated buffers have
 * their DMABuf fds wrapped using gst_dmabuf_allocator, with
 * correct per-plane stride and offset information.
 */
struct _GstDmaBufPool {
  GstBufferPool parent;

  GstDmaBufPoolPrivate *priv;
};

struct _GstDmaBufPoolClass {
  GstBufferPoolClass parent_class;
};

GType gst_dmabuf_pool_get_type(void);

/**
 * Create a new DMABuf buffer pool.
 *
 * @param gbm_device GBM device to use for allocation. The pool does
 *                   not take ownership; the caller must keep it alive.
 * @param mode       Output mode (RGBA or NV12 DMABuf).
 * @param width      Buffer width in pixels.
 * @param height     Buffer height in pixels.
 *
 * @return A new GstDmaBufPool, or NULL on failure.
 */
GstDmaBufPool *gst_dmabuf_pool_new(struct gbm_device *gbm_device,
                                    GstDmaBufOutputMode mode,
                                    guint width, guint height,
                                    guint64 modifier);

/**
 * Get the DRM modifier used by this pool's GBM allocations.
 *
 * @param pool The DMABuf pool.
 *
 * @return The DRM modifier value (e.g. DRM_FORMAT_MOD_LINEAR).
 */
guint64 gst_dmabuf_pool_get_modifier(GstDmaBufPool *pool);

/**
 * Get the GBM format used by this pool.
 *
 * @param pool The DMABuf pool.
 *
 * @return The GBM/DRM fourcc format code.
 */
guint32 gst_dmabuf_pool_get_gbm_format(GstDmaBufPool *pool);

G_END_DECLS

#endif /* HAVE_DMABUF */

#endif /* __GST_DMABUF_POOL_H__ */
