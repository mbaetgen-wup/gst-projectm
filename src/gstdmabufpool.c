/*
 * GStreamer DMABuf Buffer Pool
 *
 * Unified DMABuf buffer pool using GBM for allocation.
 * Supports RGBA (GBM_FORMAT_ARGB8888) and NV12 (GBM_FORMAT_NV12).
 *
 * Allocation flow:
 *   1. gbm_bo_create() with RENDERING usage flag
 *   2. Extract per-plane fds via gbm_bo_get_fd_for_plane()
 *   3. Query modifier via gbm_bo_get_modifier()
 *   4. Wrap fds with gst_dmabuf_allocator
 *   5. Attach video meta with correct strides/offsets
 *
 * Thread safety: Internal mutex protects pool state.
 * No GL calls are made in this file.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#ifdef HAVE_DMABUF

#include "gstdmabufpool.h"

#include <gst/gst.h>
#include <gst/video/video.h>
#include <gst/allocators/gstdmabuf.h>

#include <gbm.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>

GST_DEBUG_CATEGORY_STATIC(gst_dmabuf_pool_debug);
#define GST_CAT_DEFAULT gst_dmabuf_pool_debug

struct _GstDmaBufPoolPrivate {
  struct gbm_device *gbm_device;
  GstDmaBufOutputMode mode;
  guint width;
  guint height;
  guint32 gbm_format;
  guint64 modifier;
  gboolean modifier_queried;

  GstAllocator *dmabuf_allocator;

  GMutex pool_lock;
};

#define gst_dmabuf_pool_parent_class parent_class
G_DEFINE_TYPE_WITH_CODE(
    GstDmaBufPool, gst_dmabuf_pool, GST_TYPE_BUFFER_POOL,
    G_ADD_PRIVATE(GstDmaBufPool)
        GST_DEBUG_CATEGORY_INIT(gst_dmabuf_pool_debug, "dmabufpool", 0,
                                "DMABuf Buffer Pool"));

static void gst_dmabuf_pool_finalize(GObject *object);
static GstFlowReturn gst_dmabuf_pool_alloc_buffer(GstBufferPool *pool,
                                                    GstBuffer **buffer,
                                                    GstBufferPoolAcquireParams *params);
static gboolean gst_dmabuf_pool_start(GstBufferPool *pool);
static gboolean gst_dmabuf_pool_stop(GstBufferPool *pool);

static void gst_dmabuf_pool_class_init(GstDmaBufPoolClass *klass) {
  GObjectClass *gobject_class = G_OBJECT_CLASS(klass);
  GstBufferPoolClass *pool_class = GST_BUFFER_POOL_CLASS(klass);

  gobject_class->finalize = gst_dmabuf_pool_finalize;

  pool_class->alloc_buffer = gst_dmabuf_pool_alloc_buffer;
  pool_class->start = gst_dmabuf_pool_start;
  pool_class->stop = gst_dmabuf_pool_stop;
}

static void gst_dmabuf_pool_init(GstDmaBufPool *pool) {
  pool->priv = gst_dmabuf_pool_get_instance_private(pool);
  pool->priv->gbm_device = NULL;
  pool->priv->modifier = 0;
  pool->priv->modifier_queried = FALSE;
  pool->priv->dmabuf_allocator = NULL;
  g_mutex_init(&pool->priv->pool_lock);
}

static void gst_dmabuf_pool_finalize(GObject *object) {
  GstDmaBufPool *pool = GST_DMABUF_POOL(object);

  if (pool->priv->dmabuf_allocator) {
    gst_object_unref(pool->priv->dmabuf_allocator);
    pool->priv->dmabuf_allocator = NULL;
  }

  g_mutex_clear(&pool->priv->pool_lock);

  G_OBJECT_CLASS(parent_class)->finalize(object);
}

/**
 * Map GstDmaBufOutputMode to GBM format fourcc.
 */
static guint32
dmabuf_mode_to_gbm_format(GstDmaBufOutputMode mode) {
  switch (mode) {
    case GST_DMABUF_MODE_DMABUF_RGBA:
      return GBM_FORMAT_ARGB8888;
    case GST_DMABUF_MODE_DMABUF_NV12:
      return GBM_FORMAT_NV12;
    default:
      return 0;
  }
}

/**
 * Allocate a single GBM buffer object and wrap its planes as DMABuf
 * memory in a GstBuffer.
 *
 * For RGBA: single plane.
 * For NV12: two planes (Y + interleaved UV at half resolution).
 */
static GstFlowReturn
gst_dmabuf_pool_alloc_buffer(GstBufferPool *bpool, GstBuffer **buffer,
                              GstBufferPoolAcquireParams *params) {
  GstDmaBufPool *pool = GST_DMABUF_POOL(bpool);
  GstDmaBufPoolPrivate *priv = pool->priv;
  struct gbm_bo *bo = NULL;
  GstBuffer *buf = NULL;
  gint num_planes;
  gint fds[GST_VIDEO_MAX_PLANES] = { -1, -1, -1, -1 };
  gsize offsets[GST_VIDEO_MAX_PLANES] = { 0, };
  gint strides[GST_VIDEO_MAX_PLANES] = { 0, };

  (void)params;

  g_mutex_lock(&priv->pool_lock);

  /* Allocate GBM buffer object.
   * Strategy:
   * 1. If a specific modifier was requested (from downstream caps),
   *    use gbm_bo_create_with_modifiers().
   * 2. Fall back to gbm_bo_create() with GBM_BO_USE_RENDERING.
   * 3. Fall back to gbm_bo_create() with no flags as last resort.
   */
  if (priv->modifier != 0) {
    uint64_t mods[1] = { priv->modifier };
    bo = gbm_bo_create_with_modifiers(priv->gbm_device,
                                       priv->width, priv->height,
                                       priv->gbm_format, mods, 1);
    if (!bo)
      GST_WARNING_OBJECT(pool, "gbm_bo_create_with_modifiers failed "
          "(errno=%d: %s)", errno, g_strerror(errno));
  }
  if (!bo) {
    bo = gbm_bo_create(priv->gbm_device, priv->width, priv->height,
                       priv->gbm_format, GBM_BO_USE_RENDERING);
    if (!bo)
      GST_WARNING_OBJECT(pool, "gbm_bo_create(RENDERING) failed "
          "(errno=%d: %s)", errno, g_strerror(errno));
  }
  if (!bo) {
    bo = gbm_bo_create(priv->gbm_device, priv->width, priv->height,
                       priv->gbm_format, 0);
    if (!bo)
      GST_WARNING_OBJECT(pool, "gbm_bo_create(no flags) failed "
          "(errno=%d: %s)", errno, g_strerror(errno));
  }

  if (!bo) {
    GST_ERROR_OBJECT(pool, "Failed to create GBM BO (%ux%u, format 0x%08x)",
                     priv->width, priv->height, priv->gbm_format);
    g_mutex_unlock(&priv->pool_lock);
    return GST_FLOW_ERROR;
  }

  /* Query modifier from the first allocation if not yet done */
  if (!priv->modifier_queried) {
    priv->modifier = gbm_bo_get_modifier(bo);
    priv->modifier_queried = TRUE;
    GST_INFO_OBJECT(pool, "GBM modifier queried: 0x%" G_GINT64_MODIFIER "x",
                    priv->modifier);
  }

  num_planes = gbm_bo_get_plane_count(bo);

  GST_DEBUG_OBJECT(pool, "GBM BO allocated: %d planes, modifier 0x%"
                   G_GINT64_MODIFIER "x", num_planes, priv->modifier);

  /* Extract per-plane file descriptors, strides, and offsets */
  for (gint i = 0; i < num_planes; i++) {
    fds[i] = gbm_bo_get_fd_for_plane(bo, i);
    if (fds[i] < 0) {
      GST_ERROR_OBJECT(pool, "Failed to get fd for plane %d", i);
      goto error_close_fds;
    }
    strides[i] = gbm_bo_get_stride_for_plane(bo, i);
    offsets[i] = gbm_bo_get_offset(bo, i);

    GST_DEBUG_OBJECT(pool, "  plane %d: fd=%d stride=%d offset=%" G_GSIZE_FORMAT,
                     i, fds[i], strides[i], offsets[i]);
  }

  /* Create GstBuffer and wrap DMABuf fds */
  buf = gst_buffer_new();

  for (gint i = 0; i < num_planes; i++) {
    gsize plane_size;
    GstMemory *mem;

    if (priv->mode == GST_DMABUF_MODE_DMABUF_NV12) {
      if (i == 0) {
        /* Y plane: full resolution */
        plane_size = (gsize)strides[i] * priv->height;
      } else {
        /* UV plane: half height */
        plane_size = (gsize)strides[i] * ((priv->height + 1) / 2);
      }
    } else {
      /* RGBA: single plane */
      plane_size = (gsize)strides[i] * priv->height;
    }

    mem = gst_dmabuf_allocator_alloc(priv->dmabuf_allocator, fds[i],
                                      plane_size);
    if (!mem) {
      GST_ERROR_OBJECT(pool, "Failed to wrap fd %d as DMABuf memory", fds[i]);
      gst_buffer_unref(buf);
      /* fds[i] was not consumed, close remaining */
      for (gint j = i; j < num_planes; j++) {
        if (fds[j] >= 0)
          close(fds[j]);
      }
      goto error_destroy_bo;
    }

    /* fd ownership transferred to the allocator */
    fds[i] = -1;

    gst_buffer_append_memory(buf, mem);
  }

  /* Add video meta with stride/offset information */
  if (priv->mode == GST_DMABUF_MODE_DMABUF_NV12) {
    gst_buffer_add_video_meta_full(buf, GST_VIDEO_FRAME_FLAG_NONE,
                                    GST_VIDEO_FORMAT_NV12,
                                    priv->width, priv->height,
                                    num_planes, offsets, strides);
  } else {
    gst_buffer_add_video_meta_full(buf, GST_VIDEO_FRAME_FLAG_NONE,
                                    GST_VIDEO_FORMAT_ARGB,
                                    priv->width, priv->height,
                                    num_planes, offsets, strides);
  }

  /* Destroy the GBM BO (fds keep the underlying allocation alive) */
  gbm_bo_destroy(bo);

  g_mutex_unlock(&priv->pool_lock);

  *buffer = buf;
  return GST_FLOW_OK;

error_close_fds:
  for (gint i = 0; i < GST_VIDEO_MAX_PLANES; i++) {
    if (fds[i] >= 0)
      close(fds[i]);
  }

error_destroy_bo:
  if (bo)
    gbm_bo_destroy(bo);

  g_mutex_unlock(&priv->pool_lock);
  return GST_FLOW_ERROR;
}

static gboolean gst_dmabuf_pool_start(GstBufferPool *bpool) {
  GstDmaBufPool *pool = GST_DMABUF_POOL(bpool);

  GST_DEBUG_OBJECT(pool, "Starting DMABuf pool (mode=%d, %ux%u)",
                   pool->priv->mode, pool->priv->width, pool->priv->height);

  /* Do NOT chain to parent's start().  The default implementation
   * preallocates min_buffers via alloc_buffer(), but GBM allocation
   * may fail during preallocation when the DRM device has specific
   * constraints (tiling, modifier requirements, etc.).  Our buffers
   * are allocated on-demand in alloc_buffer() which is called by
   * acquire_buffer() — this is safe because GBM allocation is fast. */
  return TRUE;
}

static gboolean gst_dmabuf_pool_stop(GstBufferPool *bpool) {
  GstDmaBufPool *pool = GST_DMABUF_POOL(bpool);

  GST_DEBUG_OBJECT(pool, "Stopping DMABuf pool");

  return GST_BUFFER_POOL_CLASS(parent_class)->stop(bpool);
}

/**
 * gst_dmabuf_pool_new:
 * @gbm_device: GBM device for allocation (caller retains ownership).
 * @mode: DMABuf output mode.
 * @width: Buffer width.
 * @height: Buffer height.
 *
 * Creates a new DMABuf buffer pool backed by GBM allocations.
 *
 * Returns: (transfer full): A new #GstDmaBufPool, or %NULL on error.
 */
GstDmaBufPool *
gst_dmabuf_pool_new(struct gbm_device *gbm_device,
                     GstDmaBufOutputMode mode,
                     guint width, guint height,
                     guint64 modifier) {
  GstDmaBufPool *pool;
  guint32 fmt;

  g_return_val_if_fail(gbm_device != NULL, NULL);
  g_return_val_if_fail(mode == GST_DMABUF_MODE_DMABUF_RGBA ||
                       mode == GST_DMABUF_MODE_DMABUF_NV12, NULL);

  fmt = dmabuf_mode_to_gbm_format(mode);
  if (fmt == 0) {
    GST_ERROR("Invalid DMABuf mode %d", mode);
    return NULL;
  }

  pool = g_object_new(GST_TYPE_DMABUF_POOL, NULL);
  pool->priv->gbm_device = gbm_device;
  pool->priv->mode = mode;
  pool->priv->width = width;
  pool->priv->height = height;
  pool->priv->gbm_format = fmt;
  pool->priv->modifier = modifier;

  pool->priv->dmabuf_allocator = gst_dmabuf_allocator_new();
  if (!pool->priv->dmabuf_allocator) {
    GST_ERROR_OBJECT(pool, "Failed to create DMABuf allocator");
    gst_object_unref(pool);
    return NULL;
  }

  GST_INFO_OBJECT(pool,
                  "Created DMABuf pool: mode=%d, size=%ux%u, gbm_format=0x%08x",
                  mode, width, height, fmt);

  return pool;
}

guint64
gst_dmabuf_pool_get_modifier(GstDmaBufPool *pool) {
  guint64 mod;
  g_return_val_if_fail(GST_IS_DMABUF_POOL(pool), 0);
  g_mutex_lock(&pool->priv->pool_lock);
  mod = pool->priv->modifier;
  g_mutex_unlock(&pool->priv->pool_lock);
  return mod;
}

guint32
gst_dmabuf_pool_get_gbm_format(GstDmaBufPool *pool) {
  g_return_val_if_fail(GST_IS_DMABUF_POOL(pool), 0);
  return pool->priv->gbm_format;
}

#endif /* HAVE_DMABUF */
