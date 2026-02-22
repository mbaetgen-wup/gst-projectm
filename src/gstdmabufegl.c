/*
 * GStreamer DMABuf EGL Import
 *
 * Implementation for DMABuf → EGLImage → GL texture import path.
 *
 * Uses gst-gl APIs exclusively:
 *   - GstGLFuncs vtable for all GL calls (GenTextures, BindTexture, etc.)
 *   - gst_gl_context_get_proc_address() for EGL function resolution
 *   - GstGLDisplayEGL for obtaining the EGL display handle
 *
 * EGL constants are defined locally to avoid direct
 * <EGL/egl.h> or <GLES2/gl2.h> system header includes.
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

#include "gstdmabufegl.h"

#include <gst/gl/gl.h>
#include <gst/gl/gstglfuncs.h>
#include <gst/gl/egl/gstgldisplay_egl.h>
#include <gst/gl/egl/gsteglimage.h>

#include <string.h>
#include <unistd.h>
#include <drm_fourcc.h>

/*
 * EGL constants needed for DMABuf import.
 * Defined locally to avoid direct <EGL/egl.h> and <EGL/eglext.h> includes.
 * These are stable ABI values from the EGL specification.
 */
#define LOCAL_EGL_NO_DISPLAY          ((gpointer)0)
#define LOCAL_EGL_NO_IMAGE_KHR        ((gpointer)0)
#define LOCAL_EGL_NO_CONTEXT          ((gpointer)0)
#define LOCAL_EGL_LINUX_DMA_BUF_EXT           0x3270
#define LOCAL_EGL_LINUX_DRM_FOURCC_EXT        0x3271
#define LOCAL_EGL_DMA_BUF_PLANE0_FD_EXT       0x3272
#define LOCAL_EGL_DMA_BUF_PLANE0_OFFSET_EXT   0x3273
#define LOCAL_EGL_DMA_BUF_PLANE0_PITCH_EXT    0x3274
#define LOCAL_EGL_DMA_BUF_PLANE1_FD_EXT       0x3275
#define LOCAL_EGL_DMA_BUF_PLANE1_OFFSET_EXT   0x3276
#define LOCAL_EGL_DMA_BUF_PLANE1_PITCH_EXT    0x3277
#define LOCAL_EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT 0x3443
#define LOCAL_EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT 0x3444
#define LOCAL_EGL_DMA_BUF_PLANE1_MODIFIER_LO_EXT 0x3445
#define LOCAL_EGL_DMA_BUF_PLANE1_MODIFIER_HI_EXT 0x3446
#define LOCAL_EGL_WIDTH                       0x3057
#define LOCAL_EGL_HEIGHT                      0x3056
#define LOCAL_EGL_NONE                        0x3038

/* EGL function pointer typedefs using gpointer for handles */
typedef gpointer (*PFN_eglCreateImageKHR)(gpointer dpy, gpointer ctx,
                                           guint target,
                                           gpointer buffer,
                                           const gint *attrib_list);
typedef guint (*PFN_eglDestroyImageKHR)(gpointer dpy, gpointer image);
typedef gint (*PFN_eglGetError)(void);

GST_DEBUG_CATEGORY_STATIC(gst_dmabuf_egl_debug);
#define GST_CAT_DEFAULT gst_dmabuf_egl_debug

static void
ensure_debug_category(void) {
  static gsize debug_inited = 0;
  if (g_once_init_enter(&debug_inited)) {
    GST_DEBUG_CATEGORY_INIT(gst_dmabuf_egl_debug, "dmabufegl", 0,
                            "DMABuf EGL Import");
    g_once_init_leave(&debug_inited, 1);
  }
}

static gpointer
get_egl_display(GstGLContext *context) {
  GstGLDisplay *display;
  GstGLDisplayEGL *egl_display;
  gpointer egl_dpy;

  display = gst_gl_context_get_display(context);
  if (!display)
    return LOCAL_EGL_NO_DISPLAY;

  egl_display = gst_gl_display_egl_from_gl_display(display);
  gst_object_unref(display);

  if (!egl_display)
    return LOCAL_EGL_NO_DISPLAY;

  egl_dpy = (gpointer)gst_gl_display_get_handle(
      GST_GL_DISPLAY(egl_display));

  gst_object_unref(egl_display);
  return egl_dpy;
}

static gboolean
resolve_egl_funcs(GstGLContext *context,
                  PFN_eglCreateImageKHR *out_create,
                  PFN_eglDestroyImageKHR *out_destroy) {
  *out_create = (PFN_eglCreateImageKHR)
      gst_gl_context_get_proc_address(context, "eglCreateImageKHR");
  *out_destroy = (PFN_eglDestroyImageKHR)
      gst_gl_context_get_proc_address(context, "eglDestroyImageKHR");
  if (!*out_create || !*out_destroy) {
    GST_ERROR("Failed to resolve eglCreateImageKHR/eglDestroyImageKHR");
    return FALSE;
  }
  return TRUE;
}

static gboolean
bind_egl_image_to_texture(const GstGLFuncs *gl, gpointer egl_image,
                           guint *out_tex) {
  gl->GenTextures(1, out_tex);
  gl->BindTexture(GL_TEXTURE_2D, *out_tex);
  gl->TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  gl->TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  gl->TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  gl->TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  gl->EGLImageTargetTexture2D(GL_TEXTURE_2D, egl_image);
  {
    GLenum gl_err = gl->GetError();
    if (gl_err != GL_NO_ERROR) {
      GST_ERROR("glEGLImageTargetTexture2DOES failed: 0x%x", gl_err);
      gl->DeleteTextures(1, out_tex);
      *out_tex = 0;
      return FALSE;
    }
  }
  gl->BindTexture(GL_TEXTURE_2D, 0);
  return TRUE;
}

/* ── Single-plane import (RGBA, R8, GR88, etc.) ── */

gboolean
gst_dmabuf_egl_import(GstDmaBufEGLImage *image,
                       GstGLContext *context,
                       gint fd,
                       guint width, guint height,
                       guint stride, guint offset,
                       guint32 drm_format,
                       guint64 modifier) {
  const GstGLFuncs *gl;
  gpointer egl_display, egl_image;
  gint attribs[32];
  gint i = 0;
  PFN_eglCreateImageKHR create_image;
  PFN_eglDestroyImageKHR destroy_image;

  ensure_debug_category();
  g_return_val_if_fail(image != NULL && context != NULL, FALSE);
  memset(image, 0, sizeof(*image));

  gl = context->gl_vtable;
  egl_display = get_egl_display(context);
  if (egl_display == LOCAL_EGL_NO_DISPLAY) {
    GST_ERROR("No EGL display");
    return FALSE;
  }
  if (!resolve_egl_funcs(context, &create_image, &destroy_image))
    return FALSE;

  attribs[i++] = LOCAL_EGL_WIDTH;
  attribs[i++] = (gint)width;
  attribs[i++] = LOCAL_EGL_HEIGHT;
  attribs[i++] = (gint)height;
  attribs[i++] = LOCAL_EGL_LINUX_DRM_FOURCC_EXT;
  attribs[i++] = (gint)drm_format;
  attribs[i++] = LOCAL_EGL_DMA_BUF_PLANE0_FD_EXT;
  attribs[i++] = fd;
  attribs[i++] = LOCAL_EGL_DMA_BUF_PLANE0_OFFSET_EXT;
  attribs[i++] = (gint)offset;
  attribs[i++] = LOCAL_EGL_DMA_BUF_PLANE0_PITCH_EXT;
  attribs[i++] = (gint)stride;

  if (modifier != 0 && modifier != ((guint64)0x00ffffffffffffff)) {
    attribs[i++] = LOCAL_EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT;
    attribs[i++] = (gint)(modifier & 0xFFFFFFFF);
    attribs[i++] = LOCAL_EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT;
    attribs[i++] = (gint)(modifier >> 32);
  }
  attribs[i++] = LOCAL_EGL_NONE;

  egl_image = create_image(egl_display, LOCAL_EGL_NO_CONTEXT,
                            LOCAL_EGL_LINUX_DMA_BUF_EXT, NULL, attribs);
  if (egl_image == LOCAL_EGL_NO_IMAGE_KHR) {
    GST_ERROR("eglCreateImageKHR failed: fd=%d fmt=0x%08x mod=0x%"
              G_GINT64_MODIFIER "x", fd, drm_format, modifier);
    return FALSE;
  }

  if (!bind_egl_image_to_texture(gl, egl_image, &image->gl_tex)) {
    destroy_image(egl_display, egl_image);
    return FALSE;
  }

  image->egl_display = egl_display;
  image->egl_image = egl_image;
  image->width = width;
  image->height = height;
  image->valid = TRUE;

  GST_DEBUG("Imported DMABuf fd=%d → EGLImage %p → tex %u (%ux%u)",
            fd, egl_image, image->gl_tex, width, height);
  return TRUE;
}

/* ── Multi-plane NV12 import ── */

gboolean
gst_dmabuf_egl_import_nv12(GstDmaBufEGLImage *image,
                             GstGLContext *context,
                             gint y_fd, gint uv_fd,
                             guint width, guint height,
                             guint y_stride, guint y_offset,
                             guint uv_stride, guint uv_offset,
                             guint64 modifier) {
  const GstGLFuncs *gl;
  gpointer egl_display, egl_image;
  gint attribs[64];
  gint i = 0;
  PFN_eglCreateImageKHR create_image;
  PFN_eglDestroyImageKHR destroy_image;

  ensure_debug_category();
  g_return_val_if_fail(image != NULL && context != NULL, FALSE);
  memset(image, 0, sizeof(*image));

  gl = context->gl_vtable;
  egl_display = get_egl_display(context);
  if (egl_display == LOCAL_EGL_NO_DISPLAY) {
    GST_ERROR("No EGL display");
    return FALSE;
  }
  if (!resolve_egl_funcs(context, &create_image, &destroy_image))
    return FALSE;

  /*
   * Build multi-plane NV12 EGLImage attributes.
   *
   * DRM_FORMAT_NV12 tells EGL this is a 2-plane YUV surface:
   *   PLANE0 = Y  (full res, 1 byte/pixel)
   *   PLANE1 = UV (half res, 2 bytes/pixel interleaved)
   *
   * Both planes carry the same modifier so the driver knows the
   * tiling layout.  For single-fd VA-API buffers y_fd == uv_fd
   * and planes differ by offset.
   */
  attribs[i++] = LOCAL_EGL_WIDTH;
  attribs[i++] = (gint)width;
  attribs[i++] = LOCAL_EGL_HEIGHT;
  attribs[i++] = (gint)height;
  attribs[i++] = LOCAL_EGL_LINUX_DRM_FOURCC_EXT;
  attribs[i++] = (gint)DRM_FORMAT_NV12;

  /* Plane 0: Y */
  attribs[i++] = LOCAL_EGL_DMA_BUF_PLANE0_FD_EXT;
  attribs[i++] = y_fd;
  attribs[i++] = LOCAL_EGL_DMA_BUF_PLANE0_OFFSET_EXT;
  attribs[i++] = (gint)y_offset;
  attribs[i++] = LOCAL_EGL_DMA_BUF_PLANE0_PITCH_EXT;
  attribs[i++] = (gint)y_stride;
  attribs[i++] = LOCAL_EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT;
  attribs[i++] = (gint)(modifier & 0xFFFFFFFF);
  attribs[i++] = LOCAL_EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT;
  attribs[i++] = (gint)(modifier >> 32);

  /* Plane 1: UV */
  attribs[i++] = LOCAL_EGL_DMA_BUF_PLANE1_FD_EXT;
  attribs[i++] = uv_fd;
  attribs[i++] = LOCAL_EGL_DMA_BUF_PLANE1_OFFSET_EXT;
  attribs[i++] = (gint)uv_offset;
  attribs[i++] = LOCAL_EGL_DMA_BUF_PLANE1_PITCH_EXT;
  attribs[i++] = (gint)uv_stride;
  attribs[i++] = LOCAL_EGL_DMA_BUF_PLANE1_MODIFIER_LO_EXT;
  attribs[i++] = (gint)(modifier & 0xFFFFFFFF);
  attribs[i++] = LOCAL_EGL_DMA_BUF_PLANE1_MODIFIER_HI_EXT;
  attribs[i++] = (gint)(modifier >> 32);

  attribs[i++] = LOCAL_EGL_NONE;

  GST_INFO("NV12 EGL import: y_fd=%d uv_fd=%d %ux%u "
           "Y(off=%u stride=%u) UV(off=%u stride=%u) "
           "modifier=0x%" G_GINT64_MODIFIER "x",
           y_fd, uv_fd, width, height,
           y_offset, y_stride, uv_offset, uv_stride, modifier);

  egl_image = create_image(egl_display, LOCAL_EGL_NO_CONTEXT,
                            LOCAL_EGL_LINUX_DMA_BUF_EXT, NULL, attribs);
  if (egl_image == LOCAL_EGL_NO_IMAGE_KHR) {
    PFN_eglGetError get_error = (PFN_eglGetError)
        gst_gl_context_get_proc_address(context, "eglGetError");
    gint egl_err = get_error ? get_error() : 0;
    GST_ERROR("NV12 eglCreateImageKHR failed (EGL error=0x%x) "
              "y_fd=%d uv_fd=%d modifier=0x%" G_GINT64_MODIFIER "x",
              egl_err, y_fd, uv_fd, modifier);
    return FALSE;
  }

  if (!bind_egl_image_to_texture(gl, egl_image, &image->gl_tex)) {
    destroy_image(egl_display, egl_image);
    return FALSE;
  }

  image->egl_display = egl_display;
  image->egl_image = egl_image;
  image->width = width;
  image->height = height;
  image->valid = TRUE;

  GST_INFO("NV12 imported → EGLImage %p → tex %u (%ux%u)",
           egl_image, image->gl_tex, width, height);
  return TRUE;
}

void
gst_dmabuf_egl_release(GstDmaBufEGLImage *image,
                         GstGLContext *context) {
  if (!image || !image->valid)
    return;

  const GstGLFuncs *gl = context->gl_vtable;

  if (image->gl_tex) {
    gl->DeleteTextures(1, &image->gl_tex);
    image->gl_tex = 0;
  }

  if (image->egl_image != LOCAL_EGL_NO_IMAGE_KHR) {
    PFN_eglDestroyImageKHR destroy_image =
      (PFN_eglDestroyImageKHR)
          gst_gl_context_get_proc_address(context, "eglDestroyImageKHR");
    if (destroy_image)
      destroy_image(image->egl_display, image->egl_image);
    image->egl_image = LOCAL_EGL_NO_IMAGE_KHR;
  }

  image->valid = FALSE;
  GST_DEBUG("Released DMABuf EGL image");
}

#endif /* HAVE_DMABUF */
