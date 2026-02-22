/*
 * GStreamer NV12 Shader Conversion
 *
 * Shader-based RGBA → NV12 conversion.
 *
 * Y plane shader uses BT.601 luminance coefficients:
 *   Y = 0.299 * R + 0.587 * G + 0.114 * B
 *
 * UV plane shader computes at half resolution:
 *   U = -0.169 * R - 0.331 * G + 0.500 * B + 0.5
 *   V =  0.500 * R - 0.419 * G - 0.081 * B + 0.5
 *
 * Output is written as RG texture (two-channel) for UV interleaving.
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

#include "gstnv12shader.h"

#include <gst/gl/gl.h>
#include <gst/gl/gstglfuncs.h>

GST_DEBUG_CATEGORY_STATIC(gst_nv12_shader_debug);
#define GST_CAT_DEFAULT gst_nv12_shader_debug

/* Vertex shader: fullscreen quad */
static const gchar *VERTEX_SHADER =
    "#version 300 es\n"
    "precision mediump float;\n"
    "in vec2 a_position;\n"
    "in vec2 a_texcoord;\n"
    "out vec2 v_texcoord;\n"
    "void main() {\n"
    "  gl_Position = vec4(a_position, 0.0, 1.0);\n"
    "  v_texcoord = a_texcoord;\n"
    "}\n";

/* Also provide a GL3 version for desktop GL */
static const gchar *VERTEX_SHADER_GL3 =
    "#version 150\n"
    "in vec2 a_position;\n"
    "in vec2 a_texcoord;\n"
    "out vec2 v_texcoord;\n"
    "void main() {\n"
    "  gl_Position = vec4(a_position, 0.0, 1.0);\n"
    "  v_texcoord = a_texcoord;\n"
    "}\n";

/* Fragment shader: extract Y (luminance) from RGBA
 * BT.601 studio-range: Y = 16/255 + 0.257*R + 0.504*G + 0.098*B
 * Output to R8 texture: write Y into .r channel */
static const gchar *Y_FRAGMENT_SHADER =
    "#version 300 es\n"
    "precision highp float;\n"
    "in vec2 v_texcoord;\n"
    "uniform sampler2D u_rgba;\n"
    "out vec4 fragColor;\n"
    "void main() {\n"
    "  vec3 rgb = texture(u_rgba, v_texcoord).rgb;\n"
    "  float y = 0.257 * rgb.r + 0.504 * rgb.g + 0.098 * rgb.b + 0.0625;\n"
    "  fragColor = vec4(y, 0.0, 0.0, 1.0);\n"
    "}\n";

static const gchar *Y_FRAGMENT_SHADER_GL3 =
    "#version 150\n"
    "in vec2 v_texcoord;\n"
    "uniform sampler2D u_rgba;\n"
    "out vec4 fragColor;\n"
    "void main() {\n"
    "  vec3 rgb = texture(u_rgba, v_texcoord).rgb;\n"
    "  float y = 0.257 * rgb.r + 0.504 * rgb.g + 0.098 * rgb.b + 0.0625;\n"
    "  fragColor = vec4(y, 0.0, 0.0, 1.0);\n"
    "}\n";

/* Fragment shader: extract UV (chrominance) from RGBA at half resolution.
 * Proper 2x2 block averaging for correct NV12 chroma subsampling.
 * BT.601 studio-range:
 *   U = 128/255 - 0.148*R - 0.291*G + 0.439*B
 *   V = 128/255 + 0.439*R - 0.368*G - 0.071*B
 * Output to GR88 texture: U in .r, V in .g */
static const gchar *UV_FRAGMENT_SHADER =
    "#version 300 es\n"
    "precision highp float;\n"
    "in vec2 v_texcoord;\n"
    "uniform sampler2D u_rgba;\n"
    "out vec4 fragColor;\n"
    "void main() {\n"
    "  vec2 texSize = vec2(textureSize(u_rgba, 0));\n"
    "  vec2 texel = 1.0 / texSize;\n"
    "  vec2 base = v_texcoord - texel * 0.5;\n"
    "  vec3 c0 = texture(u_rgba, base).rgb;\n"
    "  vec3 c1 = texture(u_rgba, base + vec2(texel.x, 0.0)).rgb;\n"
    "  vec3 c2 = texture(u_rgba, base + vec2(0.0, texel.y)).rgb;\n"
    "  vec3 c3 = texture(u_rgba, base + texel).rgb;\n"
    "  vec3 avg = (c0 + c1 + c2 + c3) * 0.25;\n"
    "  float u = -0.148 * avg.r - 0.291 * avg.g + 0.439 * avg.b + 0.5;\n"
    "  float v =  0.439 * avg.r - 0.368 * avg.g - 0.071 * avg.b + 0.5;\n"
    "  fragColor = vec4(u, v, 0.0, 1.0);\n"
    "}\n";

static const gchar *UV_FRAGMENT_SHADER_GL3 =
    "#version 150\n"
    "in vec2 v_texcoord;\n"
    "uniform sampler2D u_rgba;\n"
    "out vec4 fragColor;\n"
    "void main() {\n"
    "  vec2 texSize = vec2(textureSize(u_rgba, 0));\n"
    "  vec2 texel = 1.0 / texSize;\n"
    "  vec2 base = v_texcoord - texel * 0.5;\n"
    "  vec3 c0 = texture(u_rgba, base).rgb;\n"
    "  vec3 c1 = texture(u_rgba, base + vec2(texel.x, 0.0)).rgb;\n"
    "  vec3 c2 = texture(u_rgba, base + vec2(0.0, texel.y)).rgb;\n"
    "  vec3 c3 = texture(u_rgba, base + texel).rgb;\n"
    "  vec3 avg = (c0 + c1 + c2 + c3) * 0.25;\n"
    "  float u = -0.148 * avg.r - 0.291 * avg.g + 0.439 * avg.b + 0.5;\n"
    "  float v =  0.439 * avg.r - 0.368 * avg.g - 0.071 * avg.b + 0.5;\n"
    "  fragColor = vec4(u, v, 0.0, 1.0);\n"
    "}\n";

/* Fullscreen quad vertices: position (xy) + texcoord (uv) */
static const gfloat QUAD_VERTICES[] = {
  /* x,    y,    u,    v  */
  -1.0f, -1.0f, 0.0f, 0.0f,
   1.0f, -1.0f, 1.0f, 0.0f,
  -1.0f,  1.0f, 0.0f, 1.0f,
   1.0f,  1.0f, 1.0f, 1.0f,
};

/**
 * Select shader source based on GL API (GLES vs desktop GL).
 */
static const gchar *
select_vertex_shader(GstGLContext *context) {
  GstGLAPI api = gst_gl_context_get_gl_api(context);
  if (api & GST_GL_API_GLES2)
    return VERTEX_SHADER;
  return VERTEX_SHADER_GL3;
}

static const gchar *
select_y_fragment_shader(GstGLContext *context) {
  GstGLAPI api = gst_gl_context_get_gl_api(context);
  if (api & GST_GL_API_GLES2)
    return Y_FRAGMENT_SHADER;
  return Y_FRAGMENT_SHADER_GL3;
}

static const gchar *
select_uv_fragment_shader(GstGLContext *context) {
  GstGLAPI api = gst_gl_context_get_gl_api(context);
  if (api & GST_GL_API_GLES2)
    return UV_FRAGMENT_SHADER;
  return UV_FRAGMENT_SHADER_GL3;
}

/**
 * Compile a shader program from vertex + fragment source.
 * Uses gst-gl shader APIs which handle version/profile negotiation.
 */
static GstGLShader *
compile_shader(GstGLContext *context, const gchar *vert_src,
               const gchar *frag_src, GError **error) {
  GstGLShader *shader;
  GstGLSLStage *vert_stage, *frag_stage;
  GstGLSLVersion version;
  GstGLSLProfile profile;

  shader = gst_gl_shader_new(context);

  /* Let gst-gl determine the correct GLSL version/profile for the context */
  if (gst_gl_context_get_gl_api(context) & GST_GL_API_GLES2) {
    version = GST_GLSL_VERSION_300;
    profile = GST_GLSL_PROFILE_ES;
  } else {
    version = GST_GLSL_VERSION_150;
    profile = GST_GLSL_PROFILE_CORE;
  }

  vert_stage = gst_glsl_stage_new_with_string(context, GL_VERTEX_SHADER,
      version, profile, vert_src);
  if (!gst_gl_shader_compile_attach_stage(shader, vert_stage, error)) {
    gst_object_unref(vert_stage);
    gst_object_unref(shader);
    return NULL;
  }

  frag_stage = gst_glsl_stage_new_with_string(context, GL_FRAGMENT_SHADER,
      version, profile, frag_src);
  if (!gst_gl_shader_compile_attach_stage(shader, frag_stage, error)) {
    gst_object_unref(frag_stage);
    gst_object_unref(shader);
    return NULL;
  }

  if (!gst_gl_shader_link(shader, error)) {
    gst_object_unref(shader);
    return NULL;
  }

  return shader;
}

/**
 * Create the fullscreen quad VAO/VBO.
 */
static gboolean
create_quad(GstNV12ShaderConverter *conv) {
  const GstGLFuncs *gl = conv->context->gl_vtable;

  gl->GenVertexArrays(1, &conv->quad_vao);
  gl->BindVertexArray(conv->quad_vao);

  gl->GenBuffers(1, &conv->quad_vbo);
  gl->BindBuffer(GL_ARRAY_BUFFER, conv->quad_vbo);
  gl->BufferData(GL_ARRAY_BUFFER, sizeof(QUAD_VERTICES), QUAD_VERTICES,
                 GL_STATIC_DRAW);

  /* Position attribute (location 0) */
  gl->EnableVertexAttribArray(0);
  gl->VertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(gfloat),
                          (void *)0);

  /* Texcoord attribute (location 1) */
  gl->EnableVertexAttribArray(1);
  gl->VertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(gfloat),
                          (void *)(2 * sizeof(gfloat)));

  gl->BindVertexArray(0);
  gl->BindBuffer(GL_ARRAY_BUFFER, 0);

  return TRUE;
}

/**
 * Create the intermediate RGBA texture and FBO for scene rendering.
 */
static gboolean
create_rgba_fbo(GstNV12ShaderConverter *conv) {
  const GstGLFuncs *gl = conv->context->gl_vtable;

  /* Create RGBA texture */
  gl->GenTextures(1, &conv->rgba_tex);
  gl->BindTexture(GL_TEXTURE_2D, conv->rgba_tex);
  gl->TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, conv->width, conv->height,
                 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
  gl->TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  gl->TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  gl->TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  gl->TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  gl->BindTexture(GL_TEXTURE_2D, 0);

  /* Create FBO using gst-gl helper with depth buffer */
  conv->rgba_fbo = gst_gl_framebuffer_new_with_default_depth(
      conv->context, conv->width, conv->height);

  if (!conv->rgba_fbo) {
    GST_ERROR("Failed to create intermediate RGBA FBO");
    return FALSE;
  }

  /* Attach our RGBA texture as the color target of the FBO.
   * gst_gl_framebuffer_new_with_default_depth() only creates a depth
   * attachment — without this step, the FBO has no color target and
   * rendering into it produces nothing (black). */
  {
    guint fbo_id = gst_gl_framebuffer_get_id(conv->rgba_fbo);
    gl->BindFramebuffer(GL_FRAMEBUFFER, fbo_id);
    gl->FramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                              GL_TEXTURE_2D, conv->rgba_tex, 0);

    GLenum status = gl->CheckFramebufferStatus(GL_FRAMEBUFFER);
    if (status != GL_FRAMEBUFFER_COMPLETE) {
      GST_ERROR("Intermediate RGBA FBO incomplete after attaching "
                "color texture: 0x%x", status);
      gl->BindFramebuffer(GL_FRAMEBUFFER, 0);
      return FALSE;
    }

    gl->BindFramebuffer(GL_FRAMEBUFFER, 0);
    GST_INFO("Attached rgba_tex %u to FBO %u (%ux%u)",
             conv->rgba_tex, fbo_id, conv->width, conv->height);
  }

  return TRUE;
}

gboolean
gst_nv12_shader_init(GstNV12ShaderConverter *conv,
                      GstGLContext *context,
                      guint width, guint height) {
  GError *error = NULL;

  GST_DEBUG_CATEGORY_INIT(gst_nv12_shader_debug, "nv12shader", 0,
                          "NV12 Shader Converter");

  g_return_val_if_fail(conv != NULL, FALSE);
  g_return_val_if_fail(context != NULL, FALSE);

  memset(conv, 0, sizeof(*conv));
  conv->context = context;
  conv->width = width;
  conv->height = height;

  /* Compile Y extraction shader */
  conv->y_shader = compile_shader(context,
      select_vertex_shader(context),
      select_y_fragment_shader(context), &error);
  if (!conv->y_shader) {
    GST_ERROR("Failed to compile Y shader: %s",
              error ? error->message : "unknown");
    g_clear_error(&error);
    return FALSE;
  }

  /* Compile UV extraction shader */
  conv->uv_shader = compile_shader(context,
      select_vertex_shader(context),
      select_uv_fragment_shader(context), &error);
  if (!conv->uv_shader) {
    GST_ERROR("Failed to compile UV shader: %s",
              error ? error->message : "unknown");
    g_clear_error(&error);
    gst_nv12_shader_dispose(conv);
    return FALSE;
  }

  /* Create fullscreen quad geometry */
  if (!create_quad(conv)) {
    GST_ERROR("Failed to create fullscreen quad");
    gst_nv12_shader_dispose(conv);
    return FALSE;
  }

  /* Create intermediate RGBA FBO for scene rendering */
  if (!create_rgba_fbo(conv)) {
    GST_ERROR("Failed to create intermediate RGBA FBO");
    gst_nv12_shader_dispose(conv);
    return FALSE;
  }

  conv->initialized = TRUE;
  GST_INFO("NV12 shader converter initialized (%ux%u)", width, height);

  return TRUE;
}

/**
 * Render a fullscreen quad with the given shader, sampling from src_tex,
 * into dst_tex attached to an FBO at the given viewport dimensions.
 */
static gboolean
render_pass(GstNV12ShaderConverter *conv, GstGLShader *shader,
            guint src_tex, guint dst_tex,
            guint vp_width, guint vp_height) {
  const GstGLFuncs *gl = conv->context->gl_vtable;
  guint fbo_id;
  GLenum status;

  /* Create a temporary FBO for this pass */
  gl->GenFramebuffers(1, &fbo_id);
  gl->BindFramebuffer(GL_FRAMEBUFFER, fbo_id);
  gl->FramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                            GL_TEXTURE_2D, dst_tex, 0);

  status = gl->CheckFramebufferStatus(GL_FRAMEBUFFER);
  if (status != GL_FRAMEBUFFER_COMPLETE) {
    GST_ERROR("Framebuffer incomplete: 0x%x (dst_tex=%u %ux%u)",
              status, dst_tex, vp_width, vp_height);
    gl->BindFramebuffer(GL_FRAMEBUFFER, 0);
    gl->DeleteFramebuffers(1, &fbo_id);
    return FALSE;
  }

  gl->Viewport(0, 0, vp_width, vp_height);
  gl->ClearColor(0.0f, 0.0f, 0.0f, 1.0f);
  gl->Clear(GL_COLOR_BUFFER_BIT);

  gst_gl_shader_use(shader);
  gst_gl_shader_set_uniform_1i(shader, "u_rgba", 0);

  gl->ActiveTexture(GL_TEXTURE0);
  gl->BindTexture(GL_TEXTURE_2D, src_tex);

  gl->BindVertexArray(conv->quad_vao);
  gl->DrawArrays(GL_TRIANGLE_STRIP, 0, 4);
  gl->BindVertexArray(0);

  gl->BindTexture(GL_TEXTURE_2D, 0);

  /* Ensure rendering is complete before releasing the FBO.
   * This is critical for tiled DMABuf surfaces where the driver
   * may otherwise not flush writes before the encoder reads. */
  gl->Finish();

  gl->BindFramebuffer(GL_FRAMEBUFFER, 0);
  gl->DeleteFramebuffers(1, &fbo_id);

  return TRUE;
}

gboolean
gst_nv12_shader_convert(GstNV12ShaderConverter *conv,
                         guint rgba_tex_id,
                         guint y_tex_id,
                         guint uv_tex_id) {
  g_return_val_if_fail(conv != NULL && conv->initialized, FALSE);

  GST_TRACE("Converting RGBA tex %u → Y tex %u + UV tex %u",
            rgba_tex_id, y_tex_id, uv_tex_id);

  /* Pass 1: RGBA → Y at full resolution */
  if (!render_pass(conv, conv->y_shader, rgba_tex_id, y_tex_id,
                   conv->width, conv->height)) {
    GST_ERROR("Y plane render pass failed");
    return FALSE;
  }

  /* Pass 2: RGBA → UV at half resolution */
  if (!render_pass(conv, conv->uv_shader, rgba_tex_id, uv_tex_id,
                   (conv->width + 1) / 2, (conv->height + 1) / 2)) {
    GST_ERROR("UV plane render pass failed");
    return FALSE;
  }

  return TRUE;
}

GstGLFramebuffer *
gst_nv12_shader_get_rgba_fbo(GstNV12ShaderConverter *conv) {
  g_return_val_if_fail(conv != NULL, NULL);
  return conv->rgba_fbo;
}

guint
gst_nv12_shader_get_rgba_tex(GstNV12ShaderConverter *conv) {
  g_return_val_if_fail(conv != NULL, 0);
  return conv->rgba_tex;
}

void
gst_nv12_shader_dispose(GstNV12ShaderConverter *conv) {
  if (!conv)
    return;

  if (conv->context) {
    const GstGLFuncs *gl = conv->context->gl_vtable;

    if (conv->quad_vao) {
      gl->DeleteVertexArrays(1, &conv->quad_vao);
      conv->quad_vao = 0;
    }
    if (conv->quad_vbo) {
      gl->DeleteBuffers(1, &conv->quad_vbo);
      conv->quad_vbo = 0;
    }
    if (conv->rgba_tex) {
      gl->DeleteTextures(1, &conv->rgba_tex);
      conv->rgba_tex = 0;
    }
  }

  if (conv->rgba_fbo) {
    gst_object_unref(conv->rgba_fbo);
    conv->rgba_fbo = NULL;
  }
  if (conv->y_shader) {
    gst_object_unref(conv->y_shader);
    conv->y_shader = NULL;
  }
  if (conv->uv_shader) {
    gst_object_unref(conv->uv_shader);
    conv->uv_shader = NULL;
  }

  conv->initialized = FALSE;
  GST_DEBUG("NV12 shader converter disposed");
}

#endif /* HAVE_DMABUF */
