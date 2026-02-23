#ifndef __GST_PROJECTM_DEBUG_H__
#define __GST_PROJECTM_DEBUG_H__

#include <gst/gl/gl.h>

G_BEGIN_DECLS

/**
 * @brief OpenGL error codes.
 */
#ifndef GL_NO_ERROR
#define GL_NO_ERROR 0
#endif
#ifndef GL_INVALID_ENUM
#define GL_INVALID_ENUM 0x0500
#endif
#ifndef GL_INVALID_VALUE
#define GL_INVALID_VALUE 0x0501
#endif
#ifndef GL_INVALID_OPERATION
#define GL_INVALID_OPERATION 0x0502
#endif
#ifndef GL_STACK_OVERFLOW
#define GL_STACK_OVERFLOW 0x0503
#endif
#ifndef GL_STACK_UNDERFLOW
#define GL_STACK_UNDERFLOW 0x0504
#endif
#ifndef GL_OUT_OF_MEMORY
#define GL_OUT_OF_MEMORY 0x0505
#endif
#ifndef GL_INVALID_FRAMEBUFFER_OPERATION
#define GL_INVALID_FRAMEBUFFER_OPERATION 0x0506
#endif
#ifndef GL_CONTEXT_LOST
#define GL_CONTEXT_LOST 0x0507
#endif

/**
 * @brief Print the current OpenGL error to stderr.
 *
 * @param context The OpenGL context.
 * @param data Unused.
 */
void gl_error_handler(GstGLContext *context);

G_END_DECLS

#endif /* __GST_PROJECTM_DEBUG_H__ */
