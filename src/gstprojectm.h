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
 * Main GstElement for this plug-in. Handles interactions with projectM.
 * Uses GstPMAudioVisualizer for handling audio-visualization (audio input,
 * timing, buffer pool, chain function). GstGLBaseAudioVisualizer (video frame
 * data, GL memory allocation, GL rendering) extends GstPMAudioVisualizer to add
 * gl context handling and is used by this plugin directly. Hierarchy:
 * GstProjectM -> GstGLBaseAudioVisualizer -> GstPMAudioVisualizer.
 */
struct _GstProjectM {
  GstGLBaseAudioVisualizer element;

  GstBaseProjectMSettings settings;

  GstProjectMPrivate *priv;

  /*< private >*/
  gpointer _padding[GST_PADDING];
};

struct _GstProjectMClass {
  GstGLBaseAudioVisualizerClass parent_class;

  /*< private >*/
  gpointer _padding[GST_PADDING];
};

G_END_DECLS

#endif /* __GST_PROJECTM_H__ */
