
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <gst/gst.h>

#include "gstprojectm.h"
#include "gstprojectmconfig.h"

/*
 * This unit registers all gst elements from this plugin library to make them
 * available to GStreamer.
 */

GST_DEBUG_CATEGORY(gst_projectm_debug);
#define GST_CAT_DEFAULT gst_projectm_debug

static gboolean plugin_init(GstPlugin *plugin) {

  gst_projectm_base_init_once();

  GST_DEBUG_CATEGORY_INIT(gst_projectm_debug, "projectm", 0,
                          "projectM visualizer plugin");

  // register main plugin projectM element
  gboolean p1 = gst_element_register(plugin, "projectm", GST_RANK_NONE,
                                     GST_TYPE_PROJECTM);

  // add additional elements here..

  return p1;
}

GST_PLUGIN_DEFINE(GST_VERSION_MAJOR, GST_VERSION_MINOR, projectm,
                  "plugin to visualize audio using the ProjectM library",
                  plugin_init, PACKAGE_VERSION, PACKAGE_LICENSE, PACKAGE_NAME,
                  PACKAGE_ORIGIN)
