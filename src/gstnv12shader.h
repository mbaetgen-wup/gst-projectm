/*
 * GStreamer NV12 Shader Conversion
 *
 * Shader-based RGBA → NV12 conversion for zero-copy DMABuf output.
 * Uses two render passes:
 *   Pass 1: RGBA → Y plane (full resolution)
 *   Pass 2: RGBA → UV plane (half resolution, interleaved)
 *
 * All operations are GPU-only; no CPU staging buffers are used.
 * Must be called from the GL thread only.
 *
 * Uses gst-gl APIs (GstGLShader, GstGLFramebuffer) where possible.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 */

#ifndef __GST_NV12_SHADER_H__
#define __GST_NV12_SHADER_H__

#ifdef HAVE_DMABUF

#include <gst/gl/gl.h>
#include <gst/gst.h>

G_BEGIN_DECLS

typedef struct _GstNV12ShaderConverter GstNV12ShaderConverter;

/**
 * GstNV12ShaderConverter:
 *
 * Manages GL resources for RGBA → NV12 conversion via shader passes.
 * Holds compiled shader programs for Y and UV extraction, and an
 * intermediate RGBA FBO for the scene render.
 */
struct _GstNV12ShaderConverter {
  GstGLContext *context; /* no ownership */

  /* Intermediate RGBA FBO for scene rendering */
  GstGLFramebuffer *rgba_fbo;
  guint rgba_tex;

  /* Shader programs */
  GstGLShader *y_shader;
  GstGLShader *uv_shader;

  /* Staging textures and FBOs for readback path */
  guint y_staging_tex;
  guint uv_staging_tex;
  guint y_staging_fbo;
  guint uv_staging_fbo;

  /* CPU staging buffer for readback */
  guint8 *readback_buf;
  gsize readback_buf_size;

  /* Fullscreen quad VAO/VBO */
  guint quad_vao;
  guint quad_vbo;

  guint width;
  guint height;

  gboolean initialized;
};

/**
 * Initialize the NV12 shader converter.
 * Must be called from GL thread.
 *
 * @param conv     Converter to initialize.
 * @param context  GL context to use.
 * @param width    Output width in pixels.
 * @param height   Output height in pixels.
 *
 * @return TRUE on success.
 */
gboolean gst_nv12_shader_init(GstNV12ShaderConverter *conv,
                               GstGLContext *context,
                               guint width, guint height);

/**
 * Convert an RGBA texture to NV12 planes, writing into the
 * given Y and UV DMABuf-backed textures.
 * Must be called from GL thread.
 *
 * @param conv         Initialized converter.
 * @param rgba_tex_id  Source RGBA texture with the rendered scene.
 * @param y_tex_id     Destination texture for Y plane (full res).
 * @param uv_tex_id    Destination texture for UV plane (half res).
 *
 * @return TRUE on success.
 */
gboolean gst_nv12_shader_convert(GstNV12ShaderConverter *conv,
                                  guint rgba_tex_id,
                                  guint y_tex_id,
                                  guint uv_tex_id);

/**
 * Get the intermediate RGBA FBO for scene rendering.
 * The caller should render their scene into this FBO, then
 * call gst_nv12_shader_convert() with the resulting texture.
 *
 * @param conv  Initialized converter.
 *
 * @return The intermediate RGBA FBO, or NULL if not initialized.
 */
GstGLFramebuffer *gst_nv12_shader_get_rgba_fbo(GstNV12ShaderConverter *conv);

/**
 * Get the intermediate RGBA texture ID.
 *
 * @param conv  Initialized converter.
 *
 * @return The GL texture ID of the intermediate RGBA texture.
 */
guint gst_nv12_shader_get_rgba_tex(GstNV12ShaderConverter *conv);

/**
 * Release all GL resources held by the converter.
 * Must be called from GL thread.
 *
 * @param conv  Converter to dispose.
 */
void gst_nv12_shader_dispose(GstNV12ShaderConverter *conv);

/**
 * Convert RGBA to NV12 via staging textures + glReadPixels, then
 * copy the result into the output buffer using gst_buffer_fill.
 */
gboolean gst_nv12_shader_convert_readback(GstNV12ShaderConverter *conv,
                                           guint rgba_tex_id,
                                           GstBuffer *out_buf,
                                           guint width, guint height);

G_END_DECLS

#endif /* HAVE_DMABUF */

#endif /* __GST_NV12_SHADER_H__ */
