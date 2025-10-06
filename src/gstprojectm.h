#ifndef __GST_PROJECTM_H__
#define __GST_PROJECTM_H__

#include "gstglbaseaudiovisualizer.h"
#include "gstprojectmbase.h"

typedef struct _GstProjectMPrivate GstProjectMPrivate;

G_BEGIN_DECLS

#define GST_TYPE_PROJECTM (gst_projectm_get_type())
G_DECLARE_FINAL_TYPE(GstProjectM, gst_projectm, GST, PROJECTM,
                     GstGLBaseAudioVisualizer)

/*
 * Main plug-in. Handles interactions with projectM.
 * Uses GstPMAudioVisualizer for handling audio-visualization (audio input,
 * timing, video frame data). GstGLBaseAudioVisualizer extends
 * GstPMAudioVisualizer to add gl context handling and is used by this plugin
 * directly. GstProjectM -> GstGLBaseAudioVisualizer -> GstPMAudioVisualizer.
 */
struct _GstProjectM {
  GstGLBaseAudioVisualizer element;

  GstBaseProjectMSettings settings;

  GstProjectMPrivate *priv;
};

struct _GstProjectMClass {
  GstGLBaseAudioVisualizerClass parent_class;
};

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

static gboolean plugin_init(GstPlugin *plugin);

static gboolean gst_projectm_setup(GstGLBaseAudioVisualizer *glav);

G_END_DECLS

#endif /* __GST_PROJECTM_H__ */
