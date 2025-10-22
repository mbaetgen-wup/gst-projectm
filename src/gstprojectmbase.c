
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "gstprojectmbase.h"

#include "debug.h"
#include "gstglbaseaudiovisualizer.h"
#include "gstprojectmconfig.h"

#include <gst/gl/gl.h>

#ifdef USE_GLEW
#include <GL/glew.h>
#endif

GST_DEBUG_CATEGORY_STATIC(gst_projectm_base_debug);
#define GST_CAT_DEFAULT gst_projectm_base_debug

enum {
  PROP_0,
  PROP_PRESET_PATH,
  PROP_TEXTURE_DIR_PATH,
  PROP_BEAT_SENSITIVITY,
  PROP_HARD_CUT_DURATION,
  PROP_HARD_CUT_ENABLED,
  PROP_HARD_CUT_SENSITIVITY,
  PROP_SOFT_CUT_DURATION,
  PROP_PRESET_DURATION,
  PROP_MESH_SIZE,
  PROP_ASPECT_CORRECTION,
  PROP_EASTER_EGG,
  PROP_PRESET_LOCKED,
  PROP_SHUFFLE_PRESETS,
  PROP_ENABLE_PLAYLIST,
  PROP_MIN_FPS,
  PROP_IS_LIVE
};

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
#define DEFAULT_MIN_FPS "1/1"
#define DEFAULT_MIN_FPS_N 1
#define DEFAULT_MIN_FPS_D 1
#define DEFAULT_IS_LIVE "auto"

static gboolean gst_projectm_base_log_preset_change(gpointer preset) {
  GST_INFO("Preset: %s", (char *)preset);

  projectm_free_string((char *)preset);

  return G_SOURCE_REMOVE; // remove after run
}

gboolean gst_projectm_base_parse_fraction(const gchar *str, gint *numerator,
                                          gint *denominator) {
  g_return_val_if_fail(str != NULL, FALSE);
  g_return_val_if_fail(numerator != NULL, FALSE);
  g_return_val_if_fail(denominator != NULL, FALSE);

  gchar **parts = g_strsplit(str, "/", 2);
  if (!parts[0] || !parts[1]) {
    g_strfreev(parts);
    return FALSE;
  }

  gchar *endptr = NULL;
  gint64 num = g_ascii_strtoll(parts[0], &endptr, 10);
  if (*endptr != '\0') {
    g_strfreev(parts);
    return FALSE;
  }

  gint64 denom = g_ascii_strtoll(parts[1], &endptr, 10);
  if (*endptr != '\0' || denom == 0) {
    g_strfreev(parts);
    return FALSE;
  }

  *numerator = (gint)num;
  *denominator = (gint)denom;

  g_strfreev(parts);
  return TRUE;
}

static void gst_projectm_base_handle_preset_change(bool is_hard_cut,
                                                   unsigned int index,
                                                   void *user_data) {

  if (gst_debug_category_get_threshold(gst_projectm_base_debug) >=
      GST_LEVEL_INFO) {

    char *name =
        projectm_playlist_item((projectm_playlist_handle)user_data, index);

    g_idle_add(gst_projectm_base_log_preset_change, name);
  }
}

static GstBaseProjectMInitResult
projectm_init(GObject *plugin, GstBaseProjectMSettings *settings,
              GstVideoInfo *vinfo) {

  GstBaseProjectMInitResult result;
  result.ret_handle = NULL;
  result.ret_playlist = NULL;
  result.success = FALSE;

  // Create ProjectM instance
  GST_DEBUG_OBJECT(plugin, "Creating projectM instance..");
  result.ret_handle = projectm_create();

  if (!result.ret_handle) {
    GST_DEBUG_OBJECT(
        plugin,
        "project_create() returned NULL, projectM instance was not created!");

    return result;
  } else {
    GST_DEBUG_OBJECT(plugin, "Created projectM instance!");
  }

  if (settings->enable_playlist) {
    GST_DEBUG_OBJECT(plugin, "Playlist enabled");

    // initialize preset playlist
    result.ret_playlist = projectm_playlist_create(result.ret_handle);
    projectm_playlist_set_shuffle(result.ret_playlist,
                                  settings->shuffle_presets);

    // add handler to print preset change
    projectm_playlist_set_preset_switched_event_callback(
        result.ret_playlist, gst_projectm_base_handle_preset_change,
        result.ret_playlist);
  } else {
    GST_DEBUG_OBJECT(plugin, "Playlist disabled");
  }
  // Log properties
  GST_INFO_OBJECT(plugin,
                  "Using Properties: "
                  "preset=%s, "
                  "texture-dir=%s, "
                  "beat-sensitivity=%f, "
                  "hard-cut-duration=%f, "
                  "hard-cut-enabled=%d, "
                  "hard-cut-sensitivity=%f, "
                  "soft-cut-duration=%f, "
                  "preset-duration=%f, "
                  "mesh-size=(%lu, %lu), "
                  "aspect-correction=%d, "
                  "easter-egg=%f, "
                  "preset-locked=%d, "
                  "enable-playlist=%d, "
                  "shuffle-presets=%d, "
                  "min-fps=%d/%d, "
                  "is-live=%s",
                  settings->preset_path, settings->texture_dir_path,
                  settings->beat_sensitivity, settings->hard_cut_duration,
                  settings->hard_cut_enabled, settings->hard_cut_sensitivity,
                  settings->soft_cut_duration, settings->preset_duration,
                  settings->mesh_width, settings->mesh_height,
                  settings->aspect_correction, settings->easter_egg,
                  settings->preset_locked, settings->enable_playlist,
                  settings->shuffle_presets, settings->min_fps_n,
                  settings->min_fps_d, settings->is_live);

  // Load preset file if path is provided
  if (settings->preset_path != NULL) {
    if (result.ret_playlist != NULL) {
      unsigned int added_count = projectm_playlist_add_path(
          result.ret_playlist, settings->preset_path, true, false);
      GST_INFO_OBJECT(plugin, "Loaded preset path: %s, presets found: %d",
                      settings->preset_path, added_count);
    } else {
      projectm_load_preset_file(result.ret_handle, settings->preset_path,
                                false);
      GST_INFO_OBJECT(plugin, "Loaded preset file: %s", settings->preset_path);
    }
  }

  // Set texture search path if directory path is provided
  if (settings->texture_dir_path != NULL) {
    const gchar *texturePaths[1] = {settings->texture_dir_path};
    projectm_set_texture_search_paths(result.ret_handle, texturePaths, 1);
  }

  // Set properties
  projectm_set_beat_sensitivity(result.ret_handle, settings->beat_sensitivity);
  projectm_set_hard_cut_duration(result.ret_handle,
                                 settings->hard_cut_duration);
  projectm_set_hard_cut_enabled(result.ret_handle, settings->hard_cut_enabled);
  projectm_set_hard_cut_sensitivity(result.ret_handle,
                                    settings->hard_cut_sensitivity);
  projectm_set_soft_cut_duration(result.ret_handle,
                                 settings->soft_cut_duration);

  // Set preset duration, or set to in infinite duration if zero
  if (settings->preset_duration > 0.0) {
    projectm_set_preset_duration(result.ret_handle, settings->preset_duration);
    // kick off the first preset
    if (projectm_playlist_size(result.ret_playlist) > 1 &&
        !settings->preset_locked) {
      projectm_playlist_play_next(result.ret_playlist, true);
    }
  } else {
    projectm_set_preset_duration(result.ret_handle, 999999.0);
  }

  projectm_set_mesh_size(result.ret_handle, settings->mesh_width,
                         settings->mesh_height);
  projectm_set_aspect_correction(result.ret_handle,
                                 settings->aspect_correction);
  projectm_set_easter_egg(result.ret_handle, settings->easter_egg);
  projectm_set_preset_locked(result.ret_handle, settings->preset_locked);

  gdouble fps;
  gst_util_fraction_to_double(GST_VIDEO_INFO_FPS_N(vinfo),
                              GST_VIDEO_INFO_FPS_D(vinfo), &fps);

  projectm_set_fps(result.ret_handle, gst_util_gdouble_to_guint64(fps));
  projectm_set_window_size(result.ret_handle, GST_VIDEO_INFO_WIDTH(vinfo),
                           GST_VIDEO_INFO_HEIGHT(vinfo));

  result.success = TRUE;
  return result;
}

void gst_projectm_base_set_property(GObject *object,
                                    GstBaseProjectMSettings *settings,
                                    guint property_id, const GValue *value,
                                    GParamSpec *pspec) {

  GstGLBaseAudioVisualizer *glav = GST_GL_BASE_AUDIO_VISUALIZER(object);

  const gchar *property_name = g_param_spec_get_name(pspec);
  GST_DEBUG_OBJECT(object, "set-property <%s>", property_name);

  switch (property_id) {
  case PROP_PRESET_PATH:
    g_free(settings->preset_path);
    settings->preset_path = g_strdup(g_value_get_string(value));
    break;
  case PROP_TEXTURE_DIR_PATH:
    g_free(settings->texture_dir_path);
    settings->texture_dir_path = g_strdup(g_value_get_string(value));
    break;
  case PROP_BEAT_SENSITIVITY:
    settings->beat_sensitivity = g_value_get_float(value);
    break;
  case PROP_HARD_CUT_DURATION:
    settings->hard_cut_duration = g_value_get_double(value);
    break;
  case PROP_HARD_CUT_ENABLED:
    settings->hard_cut_enabled = g_value_get_boolean(value);
    break;
  case PROP_HARD_CUT_SENSITIVITY:
    settings->hard_cut_sensitivity = g_value_get_float(value);
    break;
  case PROP_SOFT_CUT_DURATION:
    settings->soft_cut_duration = g_value_get_double(value);
    break;
  case PROP_PRESET_DURATION:
    settings->preset_duration = g_value_get_double(value);
    break;
  case PROP_MESH_SIZE: {
    const gchar *meshSizeStr = g_value_get_string(value);

    if (meshSizeStr) {
      gchar **parts = g_strsplit(meshSizeStr, ",", 2);
      if (parts[0] && parts[1]) {
        settings->mesh_width = atoi(parts[0]);
        settings->mesh_height = atoi(parts[1]);
      }
      g_strfreev(parts);
    }
  } break;
  case PROP_ASPECT_CORRECTION:
    settings->aspect_correction = g_value_get_boolean(value);
    break;
  case PROP_EASTER_EGG:
    settings->easter_egg = g_value_get_float(value);
    break;
  case PROP_PRESET_LOCKED:
    settings->preset_locked = g_value_get_boolean(value);
    break;
  case PROP_ENABLE_PLAYLIST:
    settings->enable_playlist = g_value_get_boolean(value);
    break;
  case PROP_SHUFFLE_PRESETS:
    settings->shuffle_presets = g_value_get_boolean(value);
    break;
  case PROP_MIN_FPS:
    gint num, denom;
    gboolean success;
    const gchar *fpsStr = g_value_get_string(value);
    success = gst_projectm_base_parse_fraction(fpsStr, &num, &denom);
    if (success) {
      settings->min_fps_n = num;
      settings->min_fps_d = denom;
      g_object_set(G_OBJECT(glav), "min-fps-n", num, "min-fps-d", denom, NULL);
    }
    break;
  case PROP_IS_LIVE:
    g_free(settings->is_live);
    settings->is_live = g_strdup(g_value_get_string(value));
    g_object_set(G_OBJECT(glav), "pipeline-live", settings->is_live, NULL);
    break;
  default:
    G_OBJECT_WARN_INVALID_PROPERTY_ID(object, property_id, pspec);
    break;
  }
}

void gst_projectm_base_get_property(GObject *object,
                                    GstBaseProjectMSettings *settings,
                                    guint property_id, GValue *value,
                                    GParamSpec *pspec) {

  const gchar *property_name = g_param_spec_get_name(pspec);
  GST_DEBUG_OBJECT(settings, "get-property <%s>", property_name);

  GstGLBaseAudioVisualizer *glav = GST_GL_BASE_AUDIO_VISUALIZER(object);

  switch (property_id) {
  case PROP_PRESET_PATH:
    g_value_set_string(value, settings->preset_path);
    break;
  case PROP_TEXTURE_DIR_PATH:
    g_value_set_string(value, settings->texture_dir_path);
    break;
  case PROP_BEAT_SENSITIVITY:
    g_value_set_float(value, settings->beat_sensitivity);
    break;
  case PROP_HARD_CUT_DURATION:
    g_value_set_double(value, settings->hard_cut_duration);
    break;
  case PROP_HARD_CUT_ENABLED:
    g_value_set_boolean(value, settings->hard_cut_enabled);
    break;
  case PROP_HARD_CUT_SENSITIVITY:
    g_value_set_float(value, settings->hard_cut_sensitivity);
    break;
  case PROP_SOFT_CUT_DURATION:
    g_value_set_double(value, settings->soft_cut_duration);
    break;
  case PROP_PRESET_DURATION:
    g_value_set_double(value, settings->preset_duration);
    break;
  case PROP_MESH_SIZE: {
    gchar *meshSizeStr =
        g_strdup_printf("%lu,%lu", settings->mesh_width, settings->mesh_height);
    g_value_set_string(value, meshSizeStr);
    g_free(meshSizeStr);
    break;
  }
  case PROP_ASPECT_CORRECTION:
    g_value_set_boolean(value, settings->aspect_correction);
    break;
  case PROP_EASTER_EGG:
    g_value_set_float(value, settings->easter_egg);
    break;
  case PROP_PRESET_LOCKED:
    g_value_set_boolean(value, settings->preset_locked);
    break;
  case PROP_ENABLE_PLAYLIST:
    g_value_set_boolean(value, settings->enable_playlist);
    break;
  case PROP_SHUFFLE_PRESETS:
    g_value_set_boolean(value, settings->shuffle_presets);
    break;
  case PROP_MIN_FPS:
    gchar *fpsStr =
        g_strdup_printf("%d/%d", settings->min_fps_n, settings->min_fps_d);
    g_value_set_string(value, fpsStr);
    g_free(fpsStr);

    g_object_set(G_OBJECT(glav), "min-fps-n", settings->min_fps_n, "min-fps-d",
                 settings->min_fps_d, NULL);
    break;
  case PROP_IS_LIVE:
    g_value_set_string(value, settings->is_live);
    break;
  default:
    G_OBJECT_WARN_INVALID_PROPERTY_ID(object, property_id, pspec);
    break;
  }
}

void gst_projectm_base_init(GstBaseProjectMSettings *settings,
                            GstBaseProjectMPrivate *priv) {

  static gsize _debug_initialized = 0;
  if (g_once_init_enter(&_debug_initialized))
  {
    GST_DEBUG_CATEGORY_INIT(gst_projectm_base_debug, "projectm_base", 0,
                          "projectM visualizer plugin base");
  }

  // Set default values for properties
  settings->preset_path = DEFAULT_PRESET_PATH;
  settings->texture_dir_path = DEFAULT_TEXTURE_DIR_PATH;
  settings->beat_sensitivity = DEFAULT_BEAT_SENSITIVITY;
  settings->hard_cut_duration = DEFAULT_HARD_CUT_DURATION;
  settings->hard_cut_enabled = DEFAULT_HARD_CUT_ENABLED;
  settings->hard_cut_sensitivity = DEFAULT_HARD_CUT_SENSITIVITY;
  settings->soft_cut_duration = DEFAULT_SOFT_CUT_DURATION;
  settings->preset_duration = DEFAULT_PRESET_DURATION;
  settings->enable_playlist = DEFAULT_ENABLE_PLAYLIST;
  settings->shuffle_presets = DEFAULT_SHUFFLE_PRESETS;
  settings->min_fps_d = DEFAULT_MIN_FPS_D;
  settings->min_fps_n = DEFAULT_MIN_FPS_N;
  settings->is_live = g_strdup(DEFAULT_IS_LIVE);

  const gchar *meshSizeStr = DEFAULT_MESH_SIZE;

  if (meshSizeStr) {
    gchar **parts = g_strsplit(meshSizeStr, ",", 2);
    if (parts[0] && parts[1]) {
      settings->mesh_width = atoi(parts[0]);
      settings->mesh_height = atoi(parts[1]);
    }
    g_strfreev(parts);
  }

  settings->aspect_correction = DEFAULT_ASPECT_CORRECTION;
  settings->easter_egg = DEFAULT_EASTER_EGG;
  settings->preset_locked = DEFAULT_PRESET_LOCKED;

  gst_projectm_base_parse_fraction(DEFAULT_MIN_FPS, &settings->min_fps_n,
                                   &settings->min_fps_d);

  priv->first_frame_time = 0;
  priv->first_frame_received = FALSE;

  g_mutex_init(&priv->projectm_lock);
}

void gst_projectm_base_finalize(GstBaseProjectMSettings *settings,
                                GstBaseProjectMPrivate *priv) {
  g_free(settings->preset_path);
  g_free(settings->texture_dir_path);
  g_free(settings->is_live);
  g_mutex_clear(&priv->projectm_lock);
}

gboolean gst_projectm_base_gl_start(GObject *plugin,
                                    GstBaseProjectMPrivate *priv,
                                    GstBaseProjectMSettings *settings,
                                    GstGLContext *context,
                                    GstVideoInfo *vinfo) {

#ifdef USE_GLEW
  GST_DEBUG_OBJECT(plugin, "Initializing GLEW");
  GLenum err = glewInit();
  if (GLEW_OK != err) {
    GST_ERROR_OBJECT(plugin, "GLEW initialization failed");
    return FALSE;
  }
#endif

  GST_PROJECTM_BASE_LOCK(priv);

  // Check if ProjectM instance exists, and create if not
  if (!priv->handle) {
    // Create ProjectM instance
    priv->first_frame_received = FALSE;
    GstBaseProjectMInitResult result = projectm_init(plugin, settings, vinfo);
    if (!result.success) {
      GST_ERROR_OBJECT(plugin, "projectM could not be initialized");
      return FALSE;
    }
    gl_error_handler(context);
    priv->handle = result.ret_handle;
    priv->playlist = result.ret_playlist;
  }
  GST_PROJECTM_BASE_UNLOCK(priv);

  GST_INFO_OBJECT(plugin, "projectM GL start complete");
  return TRUE;
}

void gst_projectm_base_gl_stop(GObject *plugin, GstBaseProjectMPrivate *priv) {

  GST_PROJECTM_BASE_LOCK(priv);
  if (priv->handle) {
    GST_DEBUG_OBJECT(plugin, "Destroying ProjectM instance");
    projectm_destroy(priv->handle);
    priv->handle = NULL;
  }
  GST_PROJECTM_BASE_UNLOCK(priv);
}

gdouble get_seconds_since_first_frame_unlocked(GstBaseProjectMPrivate *priv,
                                               GstClockTime pts) {
  if (!priv->first_frame_received) {
    // store the timestamp of the first frame
    priv->first_frame_time = pts;
    priv->first_frame_received = TRUE;
    return 0.0;
  }

  // calculate elapsed time
  GstClockTime elapsed_time = pts - priv->first_frame_time;

  // convert to fractional seconds
  gdouble elapsed_seconds = (gdouble)elapsed_time / GST_SECOND;

  return elapsed_seconds;
}

void gst_projectm_base_fill_audio_buffer_unlocked(GstBaseProjectMPrivate *priv,
                                                  GstBuffer *in_audio) {

  if (in_audio != NULL) {

    GstMapInfo audioMap;

    gst_buffer_map(in_audio, &audioMap, GST_MAP_READ);

    projectm_pcm_add_int16(priv->handle, (gint16 *)audioMap.data,
                           audioMap.size / 4, PROJECTM_STEREO);

    gst_buffer_unmap(in_audio, &audioMap);
  }
}

void gst_projectm_base_fill_gl_memory_callback(GstBaseProjectMPrivate *priv,
                                               GstGLContext *context,
                                               GstGLFramebuffer *fbo,
                                               GstClockTime pts,
                                               GstBuffer *in_audio) {

  GST_PROJECTM_BASE_LOCK(priv);

  // get current gst sync time (pts) and set projectM time
  gdouble seconds_since_first_frame =
      get_seconds_since_first_frame_unlocked(priv, pts);

  projectm_set_frame_time(priv->handle, seconds_since_first_frame);

  // process audio buffer
  gst_projectm_base_fill_audio_buffer_unlocked(priv, in_audio);

  // render the frame
  projectm_opengl_render_frame_fbo(priv->handle, fbo->fbo_id);

  // removed for performance reasons: gl_error_handler(context);

  GST_PROJECTM_BASE_UNLOCK(priv);
}

void gst_projectm_base_set_segment_pts_offset(GstBaseProjectMPrivate *priv,
                                              gint64 pts_offset) {
  GST_PROJECTM_BASE_LOCK(priv);
  priv->first_frame_time = pts_offset;
  GST_PROJECTM_BASE_UNLOCK(priv);
}

void gst_projectm_base_install_properties(GObjectClass *gobject_class) {

  // setup properties
  g_object_class_install_property(
      gobject_class, PROP_PRESET_PATH,
      g_param_spec_string(
          "preset", "Preset",
          "Specifies the path to the preset file. The preset file determines "
          "the visual style and behavior of the audio visualizer.",
          DEFAULT_PRESET_PATH, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property(
      gobject_class, PROP_TEXTURE_DIR_PATH,
      g_param_spec_string("texture-dir", "Texture Directory",
                          "Sets the path to the directory containing textures "
                          "used in the visualizer.",
                          DEFAULT_TEXTURE_DIR_PATH,
                          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property(
      gobject_class, PROP_BEAT_SENSITIVITY,
      g_param_spec_float(
          "beat-sensitivity", "Beat Sensitivity",
          "Controls the sensitivity to audio beats. Higher values make the "
          "visualizer respond more strongly to beats.",
          0.0, 5.0, DEFAULT_BEAT_SENSITIVITY,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property(
      gobject_class, PROP_HARD_CUT_DURATION,
      g_param_spec_double("hard-cut-duration", "Hard Cut Duration",
                          "Sets the duration, in seconds, for hard cuts. Hard "
                          "cuts are abrupt transitions in the visualizer.",
                          0.0, 999999.0, DEFAULT_HARD_CUT_DURATION,
                          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property(
      gobject_class, PROP_HARD_CUT_ENABLED,
      g_param_spec_boolean(
          "hard-cut-enabled", "Hard Cut Enabled",
          "Enables or disables hard cuts. When enabled, the visualizer may "
          "exhibit sudden transitions based on the audio input.",
          DEFAULT_HARD_CUT_ENABLED,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property(
      gobject_class, PROP_HARD_CUT_SENSITIVITY,
      g_param_spec_float(
          "hard-cut-sensitivity", "Hard Cut Sensitivity",
          "Adjusts the sensitivity of the visualizer to hard cuts. Higher "
          "values increase the responsiveness to abrupt changes in audio.",
          0.0, 1.0, DEFAULT_HARD_CUT_SENSITIVITY,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property(
      gobject_class, PROP_SOFT_CUT_DURATION,
      g_param_spec_double(
          "soft-cut-duration", "Soft Cut Duration",
          "Sets the duration, in seconds, for soft cuts. Soft cuts are "
          "smoother transitions between visualizer states.",
          0.0, 999999.0, DEFAULT_SOFT_CUT_DURATION,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property(
      gobject_class, PROP_PRESET_DURATION,
      g_param_spec_double("preset-duration", "Preset Duration",
                          "Sets the duration, in seconds, for each preset. A "
                          "zero value causes the preset to play indefinitely.",
                          0.0, 999999.0, DEFAULT_PRESET_DURATION,
                          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property(
      gobject_class, PROP_MESH_SIZE,
      g_param_spec_string("mesh-size", "Mesh Size",
                          "Sets the size of the mesh used in rendering. The "
                          "format is 'width,height'.",
                          DEFAULT_MESH_SIZE,
                          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property(
      gobject_class, PROP_ASPECT_CORRECTION,
      g_param_spec_boolean(
          "aspect-correction", "Aspect Correction",
          "Enables or disables aspect ratio correction. When enabled, the "
          "visualizer adjusts for aspect ratio differences in rendering.",
          DEFAULT_ASPECT_CORRECTION,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property(
      gobject_class, PROP_EASTER_EGG,
      g_param_spec_float(
          "easter-egg", "Easter Egg",
          "Controls the activation of an Easter Egg feature. The value "
          "determines the likelihood of triggering the Easter Egg.",
          0.0, 1.0, DEFAULT_EASTER_EGG,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property(
      gobject_class, PROP_PRESET_LOCKED,
      g_param_spec_boolean(
          "preset-locked", "Preset Locked",
          "Locks or unlocks the current preset. When locked, the visualizer "
          "remains on the current preset without automatic changes.",
          DEFAULT_PRESET_LOCKED, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property(
      gobject_class, PROP_ENABLE_PLAYLIST,
      g_param_spec_boolean(
          "enable-playlist", "Enable Playlist",
          "Enables or disables the playlist feature. When enabled, the "
          "visualizer can switch between presets based on a provided "
          "playlist.",
          DEFAULT_ENABLE_PLAYLIST, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property(
      gobject_class, PROP_SHUFFLE_PRESETS,
      g_param_spec_boolean(
          "shuffle-presets", "Shuffle Presets",
          "Enables or disables preset shuffling. When enabled, the "
          "visualizer "
          "randomly selects presets from the playlist if presets are "
          "provided "
          "and not locked. Playlist must be enabled for this to take effect.",
          DEFAULT_SHUFFLE_PRESETS, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property(
      gobject_class, PROP_MIN_FPS,
      g_param_spec_string(
          "min-fps", "Minimum FPS",
          "Specifies the lower bound for EMA fps adjustments for real-time "
          "pipelines. How low the fps is allowed to be in case the rendering "
          "can't keep up with pipeline fps. Applies to real-time pipelines "
          "only.",
          DEFAULT_MIN_FPS, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property(
      gobject_class, PROP_IS_LIVE,
      g_param_spec_string(
          "is-live", "is live",
          "Specifies if the plugin renders in real-time or as fast as "
          "possible "
          "(offline). This setting is auto-detected for live pipelines, "
          "but can also be specified if auto-detection is "
          "not appropriate. Possible values are \"auto\", \"true\", "
          "\"false\". "
          "Default is \"auto\".",
          DEFAULT_IS_LIVE, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
}
