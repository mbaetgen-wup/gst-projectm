/*
 * GStreamer DMABuf EGL Import
 *
 * Handles importing DMABuf file descriptors into GL textures via
 * EGL_EXT_image_dma_buf_import. Creates EGLImage objects and binds
 * them to GL textures for rendering.
 *
 * Uses gst-gl for GL function dispatch (GstGLFuncs vtable) and
 * EGL proc address resolution (gst_gl_context_get_proc_address).
 * Direct EGL/GLES2 system headers are NOT included; all GL/EGL
 * types and functions come through gst-gl headers.
 *
 * Must be called from GL thread only.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 */

#ifndef __GST_DMABUF_EGL_H__
#define __GST_DMABUF_EGL_H__

#ifdef HAVE_DMABUF

#include <gst/gl/gl.h>
#include <gst/gl/gstglfuncs.h>
#include <gst/gl/egl/gstgldisplay_egl.h>
#include <gst/gl/egl/gsteglimage.h>
#include <gst/gst.h>

G_BEGIN_DECLS

typedef struct _GstDmaBufEGLImage GstDmaBufEGLImage;

/**
 * GstDmaBufEGLImage:
 *
 * Wraps an EGLImage created from a DMABuf fd, bound to a GL texture.
 * Manages lifecycle of both the EGLImage and the associated texture.
 */
struct _GstDmaBufEGLImage {
  gpointer egl_display;   /* EGLDisplay, stored as gpointer */
  gpointer egl_image;     /* EGLImageKHR, stored as gpointer */
  guint gl_tex;
  guint width;
  guint height;
  gboolean valid;
};

/**
 * Import a single-plane DMABuf as an EGLImage → GL texture.
 * Used for RGBA imports and per-plane R8/GR88 imports.
 */
gboolean gst_dmabuf_egl_import(GstDmaBufEGLImage *image,
                                GstGLContext *context,
                                gint fd,
                                guint width, guint height,
                                guint stride, guint offset,
                                guint32 drm_format,
                                guint64 modifier);

/**
 * Import a multi-plane NV12 DMABuf as a single EGLImage → GL texture.
 *
 * Uses DRM_FORMAT_NV12 with PLANE0 (Y) and PLANE1 (UV) EGL attributes,
 * including the modifier for each plane.  This is the correct method for
 * importing VA-API allocated tiled NV12 surfaces.
 *
 * The resulting GL texture represents the whole NV12 surface.
 * It can be bound to an FBO for rendering if the driver supports it.
 *
 * For single-fd VA-API buffers pass the same fd for y_fd and uv_fd.
 */
gboolean gst_dmabuf_egl_import_nv12(GstDmaBufEGLImage *image,
                                     GstGLContext *context,
                                     gint y_fd, gint uv_fd,
                                     guint width, guint height,
                                     guint y_stride, guint y_offset,
                                     guint uv_stride, guint uv_offset,
                                     guint64 modifier);

/**
 * Release the EGLImage and GL texture.
 * Must be called from GL thread.
 */
void gst_dmabuf_egl_release(GstDmaBufEGLImage *image,
                              GstGLContext *context);

G_END_DECLS

#endif /* HAVE_DMABUF */

#endif /* __GST_DMABUF_EGL_H__ */
