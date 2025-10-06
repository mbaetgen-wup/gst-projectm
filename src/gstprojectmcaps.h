#ifndef __GST_PROJECTM_CAPS_H__
#define __GST_PROJECTM_CAPS_H__

#include <glib.h>

#include "gstprojectm.h"

G_BEGIN_DECLS

/**
 * @brief Get audio sink caps based on the given type.
 *
 * @return The audio caps format string.
 */
const gchar *get_audio_sink_cap();

/**
 * Get video source caps based on the given type.
 *
 * @return The video caps format string.
 */
const gchar *get_video_src_cap();

G_END_DECLS

#endif /* __GST_PROJECTM_CAPS_H__ */
