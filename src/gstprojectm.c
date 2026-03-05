#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "gstprojectm.h"

#include "debug.h"
#include "gstglbaseaudiovisualizer.h"
#include "gstprojectmcaps.h"
#include "gstprojectmconfig.h"

#include <gst/gst.h>
#include <string.h>

GST_DEBUG_CATEGORY_STATIC(gst_projectm_debug);
#define GST_CAT_DEFAULT gst_projectm_debug

static void gst_projectm_set_property(GObject *object, guint prop_id,
                                      const GValue *value, GParamSpec *pspec);
static void gst_projectm_get_property(GObject *object, guint prop_id,
                                      GValue *value, GParamSpec *pspec);
static void gst_projectm_init(GstProjectM *plugin);
static void gst_projectm_finalize(GObject *object);
static gboolean gst_projectm_gl_start(GstGLBaseAudioVisualizer *glav);
static void gst_projectm_gl_stop(GstGLBaseAudioVisualizer *glav);
static gboolean gst_projectm_fill_gl_memory(GstAVRenderParams *render_data);
static void gst_projectm_class_init(GstProjectMClass *klass);
static gboolean gst_projectm_setup(GstGLBaseAudioVisualizer *glav);
static void gst_projectm_on_preset_changed(const char *preset_name,
                                            gboolean is_hard_cut,
                                            GstClockTime pts,
                                            gpointer user_data);

struct _GstProjectMPrivate {

  GstBaseProjectMPrivate base;

  /**
   * Source pad for preset change JSON buffers.
   */
  GstPad *preset_srcpad;
};

G_DEFINE_TYPE_WITH_CODE(GstProjectM, gst_projectm,
                        GST_TYPE_GL_BASE_AUDIO_VISUALIZER,
                        G_ADD_PRIVATE(GstProjectM)
                            GST_DEBUG_CATEGORY_INIT(gst_projectm_debug,
                                                    "gstprojectm", 0,
                                                    "Plugin Root"));

void gst_projectm_set_property(GObject *object, guint property_id,
                               const GValue *value, GParamSpec *pspec) {

  GstProjectM *plugin = GST_PROJECTM(object);

  gst_projectm_base_set_property(object, &plugin->settings, property_id, value,
                                 pspec);
}

void gst_projectm_get_property(GObject *object, guint property_id,
                               GValue *value, GParamSpec *pspec) {
  GstProjectM *plugin = GST_PROJECTM(object);

  gst_projectm_base_get_property(object, &plugin->settings, property_id, value,
                                 pspec);
}

static void gst_projectm_init(GstProjectM *plugin) {
  plugin->priv = gst_projectm_get_instance_private(plugin);

  gst_gl_memory_init_once();

  gst_projectm_base_init(&plugin->settings, &plugin->priv->base);

  /* Create the preset change source pad from the template */
  GstPadTemplate *preset_tmpl = gst_element_class_get_pad_template(
      GST_ELEMENT_GET_CLASS(plugin), "preset_src");
  plugin->priv->preset_srcpad =
      gst_pad_new_from_template(preset_tmpl, "preset_src");
  gst_pad_use_fixed_caps(plugin->priv->preset_srcpad);
  gst_pad_set_active(plugin->priv->preset_srcpad, TRUE);
  gst_element_add_pad(GST_ELEMENT(plugin), plugin->priv->preset_srcpad);

  /* Register the preset change notification so we can push JSON buffers */
  gst_projectm_base_set_preset_changed_callback(
      &plugin->priv->base, gst_projectm_on_preset_changed, plugin);
}

static void gst_projectm_finalize(GObject *object) {

  GstProjectM *plugin = GST_PROJECTM(object);

  gst_projectm_base_finalize(&plugin->settings, &plugin->priv->base);
  G_OBJECT_CLASS(gst_projectm_parent_class)->finalize(object);
}

static void gst_projectm_gl_stop(GstGLBaseAudioVisualizer *glav) {

  GstProjectM *plugin = GST_PROJECTM(glav);

  gst_projectm_base_gl_stop(G_OBJECT(glav), &plugin->priv->base);
}

static gboolean gst_projectm_gl_start(GstGLBaseAudioVisualizer *glav) {
  // Cast the audio visualizer to the ProjectM plugin
  GstProjectM *plugin = GST_PROJECTM(glav);
  GstPMAudioVisualizer *pmav = GST_PM_AUDIO_VISUALIZER(glav);

  gboolean ret = gst_projectm_base_gl_start(G_OBJECT(glav), &plugin->priv->base,
                             &plugin->settings, glav->context, &pmav->vinfo);

  if (ret) {
    GST_INFO_OBJECT(plugin, "GL start complete");
  }

  return ret;
}

static gboolean gst_projectm_setup(GstGLBaseAudioVisualizer *glav) {

  GstPMAudioVisualizer *pmav = GST_PM_AUDIO_VISUALIZER(glav);

  // Log audio info
  GST_DEBUG_OBJECT(
      glav, "Audio Information <Channels: %d, SampleRate: %d, Description: %s>",
      pmav->ainfo.channels, pmav->ainfo.rate, pmav->ainfo.finfo->description);

  // Log video info
  GST_DEBUG_OBJECT(
      glav,
      "Video Information <Dimensions: %dx%d, FPS: %d/%d, SamplesPerFrame: %d>",
      GST_VIDEO_INFO_WIDTH(&pmav->vinfo), GST_VIDEO_INFO_HEIGHT(&pmav->vinfo),
      pmav->vinfo.fps_n, pmav->vinfo.fps_d, pmav->req_spf);

  return TRUE;
}

static gboolean gst_projectm_fill_gl_memory_callback(gpointer stuff) {

  GstAVRenderParams *render_data = (GstAVRenderParams *)stuff;
  GstProjectM *plugin = GST_PROJECTM(render_data->plugin);
  GstGLBaseAudioVisualizer *glav =
      GST_GL_BASE_AUDIO_VISUALIZER(render_data->plugin);
  gboolean result = TRUE;

  // VIDEO
  GST_TRACE_OBJECT(plugin, "rendering projectM to fbo %d",
                   render_data->fbo->fbo_id);

  gst_projectm_base_fill_gl_memory_callback(&plugin->priv->base, glav->context,
                                            render_data->fbo, render_data->pts,
                                            render_data->in_audio);

  return result;
}

static gboolean gst_projectm_fill_gl_memory(GstAVRenderParams *render_data) {

  gboolean result = gst_gl_framebuffer_draw_to_texture(
      render_data->fbo, render_data->mem, gst_projectm_fill_gl_memory_callback,
      render_data);

  return result;
}

static void gst_projectm_segment_change(GstPMAudioVisualizer *scope,
                                        GstSegment *segment) {
  GstProjectM *plugin = GST_PROJECTM(scope);
  gint64 pts_offset = segment->time - segment->start;
  gst_projectm_base_set_segment_pts_offset(&plugin->priv->base, pts_offset);
}

/**
 * Called from the projectM preset-switched callback (GL render thread,
 * projectm_lock held).  Creates a JSON buffer and pushes it on the
 * preset_src pad.
 */
static void gst_projectm_on_preset_changed(const char *preset_name,
                                            gboolean is_hard_cut,
                                            GstClockTime pts,
                                            gpointer user_data) {
  GstProjectM *plugin = GST_PROJECTM(user_data);
  GstPad *pad = plugin->priv->preset_srcpad;

  gst_projectm_base_push_preset_change(preset_name, is_hard_cut, pts, (GObject*)plugin, pad);
}

static void gst_projectm_class_init(GstProjectMClass *klass) {
  GObjectClass *gobject_class = (GObjectClass *)klass;
  GstPMAudioVisualizerClass *parent_scope_class =
      GST_PM_AUDIO_VISUALIZER_CLASS(klass);
  GstGLBaseAudioVisualizerClass *scope_class =
      GST_GL_BASE_AUDIO_VISUALIZER_CLASS(klass);

  // Setup audio and video caps
  const gchar *audio_sink_caps = get_audio_sink_cap();
  const gchar *video_src_caps = get_video_src_cap();

  gst_element_class_add_pad_template(
      GST_ELEMENT_CLASS(klass),
      gst_pad_template_new("src", GST_PAD_SRC, GST_PAD_ALWAYS,
                           gst_caps_from_string(video_src_caps)));
  gst_element_class_add_pad_template(
      GST_ELEMENT_CLASS(klass),
      gst_pad_template_new("sink", GST_PAD_SINK, GST_PAD_ALWAYS,
                           gst_caps_from_string(audio_sink_caps)));

  /* Preset change source pad: produces JSON text buffers whenever the
   * active projectM preset changes. */
  gst_element_class_add_pad_template(
      GST_ELEMENT_CLASS(klass),
      gst_pad_template_new("preset_src", GST_PAD_SRC, GST_PAD_ALWAYS,
                           gst_caps_from_string("application/x-json")));

  gst_element_class_set_static_metadata(
      GST_ELEMENT_CLASS(klass), "ProjectM Visualizer", "Generic",
      PACKAGE_DESCRIPTION,
      "AnomieVision <anomievision@gmail.com> | Tristan Charpentier "
      "<tristan_charpentier@hotmail.com> | Michael Baetgen "
      "<michael -at- widerup.com>");

  // Setup properties
  gobject_class->set_property = gst_projectm_set_property;
  gobject_class->get_property = gst_projectm_get_property;

  gst_projectm_base_install_properties(gobject_class);

  gobject_class->finalize = gst_projectm_finalize;

  scope_class->supported_gl_api = GST_GL_API_OPENGL3 | GST_GL_API_GLES2;
  scope_class->gl_start = GST_DEBUG_FUNCPTR(gst_projectm_gl_start);
  scope_class->gl_stop = GST_DEBUG_FUNCPTR(gst_projectm_gl_stop);
  scope_class->fill_gl_memory = GST_DEBUG_FUNCPTR(gst_projectm_fill_gl_memory);
  scope_class->setup = GST_DEBUG_FUNCPTR(gst_projectm_setup);
  parent_scope_class->segment_change =
      GST_DEBUG_FUNCPTR(gst_projectm_segment_change);
}
