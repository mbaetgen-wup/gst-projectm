#ifndef __GST_PROJECTM_CONFIG_H__
#define __GST_PROJECTM_CONFIG_H__

#include <glib.h>

G_BEGIN_DECLS

/**
 * @brief Plugin Details
 */

#define PACKAGE "GstProjectM"
#define PACKAGE_NAME "GstProjectM"
#define PACKAGE_LICENSE "LGPL"
#define PACKAGE_ORIGIN "https://github.com/projectM-visualizer/gst-projectm"

/* PACKAGE_VERSION is set by cmake via target_compile_definitions.
   The fallback covers standalone / non-cmake builds. */
#ifndef PACKAGE_VERSION
#define PACKAGE_VERSION "unspecified"
#endif

/* Vendor metadata for static builds (set by cmake when projectM is
   statically linked).  Empty strings indicate a dynamic or local build. */
#ifndef PROJECTM_VENDOR_REF
#define PROJECTM_VENDOR_REF ""
#endif
#ifndef PROJECTM_VENDOR_COMMIT
#define PROJECTM_VENDOR_COMMIT ""
#endif
#ifndef PROJECTM_VENDOR_TIMESTAMP
#define PROJECTM_VENDOR_TIMESTAMP ""
#endif
#ifndef PROJECTM_VENDOR_GL_VARIANT
#define PROJECTM_VENDOR_GL_VARIANT ""
#endif

/**
 * @brief Plugin description string.
 *
 * When HAVE_PROJECTM_VENDOR_INFO is defined (via cmake -DPROJECTM_VENDOR_REF=...),
 * the description includes the vendored projectM ref, commit and timestamp.
 * Otherwise it falls back to a generic description suitable for dynamic builds.
 */
#ifdef HAVE_PROJECTM_VENDOR_INFO
#define PACKAGE_DESCRIPTION \
    "projectM audio visualizer" \
    " (projectM " PROJECTM_VENDOR_REF "@" PROJECTM_VENDOR_COMMIT \
    " " PROJECTM_VENDOR_TIMESTAMP " static-" \
    PROJECTM_VENDOR_GL_VARIANT ")"
#else
#define PACKAGE_DESCRIPTION \
    "projectM audio visualizer"
#endif

/**
 * @brief ProjectM Settings (defaults)
 */

#define DEFAULT_PRESET_PATH NULL
#define DEFAULT_TEXTURE_DIR_PATH NULL
#define DEFAULT_BEAT_SENSITIVITY 1.0
#define DEFAULT_HARD_CUT_DURATION 3.0
#define DEFAULT_HARD_CUT_ENABLED FALSE
#define DEFAULT_HARD_CUT_SENSITIVITY 1.0
#define DEFAULT_SOFT_CUT_DURATION 3.0
#define DEFAULT_PRESET_DURATION 0.0
#define DEFAULT_MESH_SIZE "48,32"
#define DEFAULT_ASPECT_CORRECTION TRUE
#define DEFAULT_EASTER_EGG 0.0
#define DEFAULT_PRESET_LOCKED FALSE
#define DEFAULT_ENABLE_PLAYLIST TRUE
#define DEFAULT_SHUFFLE_PRESETS TRUE // depends on ENABLE_PLAYLIST

G_END_DECLS

#endif /* __GST_PROJECTM_CONFIG_H__ */
