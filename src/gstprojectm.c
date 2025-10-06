#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#ifdef USE_GLEW
#include <GL/glew.h>
#endif

#include "gstprojectm.h"

#include <gst/gl/gstglfuncs.h>
#include <gst/gst.h>

#include "debug.h"
#include "gstglbaseaudiovisualizer.h"
#include "gstprojectmcaps.h"

GST_DEBUG_CATEGORY_STATIC(gst_projectm_debug);
#define GST_CAT_DEFAULT gst_projectm_debug

struct _GstProjectMPrivate {

  GstBaseProjectMPrivate base;
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
}

static void gst_projectm_finalize(GObject *object) {

  GstProjectM *plugin = GST_PROJECTM(object);

  gst_projectm_base_finalize(&plugin->settings, &plugin->priv->base);
  G_OBJECT_CLASS(gst_projectm_parent_class)->finalize(object);
}

static void gst_projectm_gl_stop(GstGLBaseAudioVisualizer *src) {

  GstProjectM *plugin = GST_PROJECTM(src);

  gst_projectm_base_gl_stop(G_OBJECT(src), &plugin->priv->base);
}

static gboolean gst_projectm_gl_start(GstGLBaseAudioVisualizer *glav) {
  // Cast the audio visualizer to the ProjectM plugin
  GstProjectM *plugin = GST_PROJECTM(glav);
  GstPMAudioVisualizer *pmav = GST_PM_AUDIO_VISUALIZER(glav);

  gst_projectm_base_gl_start(G_OBJECT(glav), &plugin->priv->base,
                             &plugin->settings, glav->context, &pmav->vinfo);

  GST_INFO_OBJECT(plugin, "GL start complete");

  return TRUE;
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

  gst_element_class_set_static_metadata(
      GST_ELEMENT_CLASS(klass), "ProjectM Visualizer", "Generic",
      "A plugin for visualizing music using ProjectM",
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
