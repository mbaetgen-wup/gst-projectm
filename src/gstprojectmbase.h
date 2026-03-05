
/*
 * Basic gst/projectM integration structs and functions that can be re-used for
 * alternative plugin implementations.
 */

#ifndef __GST_PROJECTM_BASE_H__
#define __GST_PROJECTM_BASE_H__

#include <gst/gl/gstgl_fwd.h>
#include <gst/gst.h>
#include <gst/video/video-info.h>
#include <projectM-4/playlist.h>
#include <projectM-4/projectM.h>

/**
 * Callback invoked when the projectM preset changes.
 *
 * @param preset_name Name/path of the new preset.
 * @param is_hard_cut TRUE if the preset change was a hard cut.
 * @param pts The last video presentation timestamp at the time of the change.
 * @param user_data Caller-supplied context pointer.
 */
typedef void (*GstProjectMPresetChangedFunc)(const char *preset_name,
                                             gboolean is_hard_cut,
                                             GstClockTime pts,
                                             gpointer user_data);

G_BEGIN_DECLS

/**
 * projectM config properties.
 */
struct _GstBaseProjectMSettings {

  gchar *preset_path;
  gchar *texture_dir_path;

  gfloat beat_sensitivity;
  gdouble hard_cut_duration;
  gboolean hard_cut_enabled;
  gfloat hard_cut_sensitivity;
  gdouble soft_cut_duration;
  gdouble preset_duration;
  gulong mesh_width;
  gulong mesh_height;
  gboolean aspect_correction;
  gfloat easter_egg;
  gboolean preset_locked;
  gboolean enable_playlist;
  gboolean shuffle_presets;
  gint min_fps_n;
  gint min_fps_d;

  /*< private >*/
  gpointer _padding[GST_PADDING];
};

/**
 * Variables needed for managing projectM.
 */
struct _GstBaseProjectMPrivate {
  projectm_handle handle;
  projectm_playlist_handle playlist;
  GMutex projectm_lock;

  GstClockTime first_frame_time;
  gboolean first_frame_received;

  /**
   * Last video pts timestamp, updated each frame.  Protected by projectm_lock.
   */
  GstClockTime last_pts;

  /**
   * Optional callback invoked on preset change.  Set once before gl_start;
   * read from the projectM callback thread -- no lock needed for the pointer
   * itself because it is immutable after gl_start.
   */
  GstProjectMPresetChangedFunc preset_changed_func;
  gpointer preset_changed_user_data;

  /*< private >*/
  gpointer _padding[GST_PADDING];
};

/**
 * projectM init result return arguments.
 */
struct _GstBaseProjectMInitResult {
  projectm_handle ret_handle;
  projectm_playlist_handle ret_playlist;
  gboolean success;

  /*< private >*/
  gpointer _padding[GST_PADDING];
};

typedef struct _GstBaseProjectMPrivate GstBaseProjectMPrivate;
typedef struct _GstBaseProjectMSettings GstBaseProjectMSettings;
typedef struct _GstBaseProjectMInitResult GstBaseProjectMInitResult;

/**
 * set_property delegate for projectM setting structs.
 *
 * @param object Plugin gst object.
 * @param settings Settings struct to update.
 * @param property_id Property id to update.
 * @param value Property value.
 * @param pspec Gst param type spec.
 */
void gst_projectm_base_set_property(GObject *object,
                                    GstBaseProjectMSettings *settings,
                                    guint property_id, const GValue *value,
                                    GParamSpec *pspec);

/**
 * get_property delegate for projectM setting structs.
 *
 * @param object Plugin gst object.
 * @param settings Settings struct to update.
 * @param property_id Property id to update.
 * @param value Property value.
 * @param pspec Gst param type spec.
 */
void gst_projectm_base_get_property(GObject *object,
                                    GstBaseProjectMSettings *settings,
                                    guint property_id, GValue *value,
                                    GParamSpec *pspec);

/**
 * Plugin init() delegate for projectM settings and priv.
 *
 * @param settings Settings to init.
 * @param priv Private obj to init.
 */
void gst_projectm_base_init(GstBaseProjectMSettings *settings,
                            GstBaseProjectMPrivate *priv);

/**
 * Plugin finalize() delegate for projectM settings and priv.
 *
 * @param settings Settings to init.
 * @param priv Private obj to init.
 */
void gst_projectm_base_finalize(GstBaseProjectMSettings *settings,
                                GstBaseProjectMPrivate *priv);

/**
 * GL start delegate to setup projectM fbo rendering.
 *
 * @param plugin Plugin gst object.
 * @param priv Plugin priv data.
 * @param settings Plugin settings.
 * @param context The gl context to use for projectM rendering.
 * @param vinfo Video rendering details.
 *
 * @return TRUE on success.
 */
gboolean gst_projectm_base_gl_start(GObject *plugin,
                                    GstBaseProjectMPrivate *priv,
                                    GstBaseProjectMSettings *settings,
                                    GstGLContext *context, GstVideoInfo *vinfo);

/**
 * GL stop delegate to clean up projectM rendering resources.
 *
 * @param plugin Plugin gst object.
 * @param priv Plugin priv data.
 */
void gst_projectm_base_gl_stop(GObject *plugin, GstBaseProjectMPrivate *priv);

/**
 * Just pushes audio data to projectM without rendering.
 *
 * @param priv Plugin priv data.
 * @param in_audio Audio data buffer to push to projectM.
 */
void gst_projectm_base_fill_audio_buffer_unlocked(GstBaseProjectMPrivate *priv,
                                                  GstBuffer *in_audio);

/**
 * Render one frame with projectM.
 *
 * @param priv Plugin priv data.
 * @param context ProjectM GL context.
 * @param pts Current pts timestamp.
 * @param in_audio Input audio buffer to push to projectM before rendering, may
 * be NULL.
 */
void gst_projectm_base_fill_gl_memory_callback(GstBaseProjectMPrivate *priv,
                                               GstGLContext *context,
                                               GstGLFramebuffer *fbo,
                                               GstClockTime pts,
                                               GstBuffer *in_audio);

/**
 * Reset time offset for a new segment.
 *
 * @param priv Plugin priv data.
 * @param pts_offset pts time offset for a new segment.
 */
void gst_projectm_base_set_segment_pts_offset(GstBaseProjectMPrivate *priv,
                                              gint64 pts_offset);

/**
 * Install properties from projectM settings to given plugin class.
 *
 * @param gobject_class Plugin class to install properties to.
 */
void gst_projectm_base_install_properties(GObjectClass *gobject_class);

/**
 * Utility to parse a fraction from a string.
 *
 * @param str Fraction as string, ex. 60/1
 * @param numerator Return ref for numerator.
 * @param denominator Return ref for denominator.
 *
 * @return TRUE if the fraction was parsed correctly.
 */
gboolean gst_projectm_base_parse_fraction(const gchar *str, gint *numerator,
                                          gint *denominator);

/**
 * Register a callback that will be invoked every time projectM switches to a
 * new preset.  Must be called before gst_projectm_base_gl_start().
 *
 * @param priv      Plugin priv data.
 * @param func      Callback function (may be NULL to unregister).
 * @param user_data Opaque pointer forwarded to @p func.
 */
void gst_projectm_base_set_preset_changed_callback(
    GstBaseProjectMPrivate *priv, GstProjectMPresetChangedFunc func,
    gpointer user_data);

/**
 * Pushes a preset changed message to the preset change source pad.
 *
 * @param preset_name Name of next preset.
 * @param is_hard_cut Was the preset change triggered by a hard cut.
 * @param pts Current presentation timestamp.
 * @param plugin Parent plugin.
 * @param pad The JSON text source pad to push to.
 * @return
 */
bool gst_projectm_base_push_preset_change(const char *preset_name, gboolean is_hard_cut, GstClockTime pts, GObject *plugin, GstPad *pad);


#define GST_PROJECTM_BASE_LOCK(priv) (g_mutex_lock(&priv->projectm_lock))
#define GST_PROJECTM_BASE_UNLOCK(priv) (g_mutex_unlock(&priv->projectm_lock))

G_END_DECLS

#endif // __GST_PROJECTM_BASE_H__
