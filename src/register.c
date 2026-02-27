#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#ifdef _WIN32
#define EXPORT __declspec(dllexport)
#else
#define EXPORT
#endif

#include "gstprojectm.h"
#include "gstprojectmconfig.h"

#include <gst/gst.h>

/*
 * This unit registers all gst elements from this plugin library to make them
 * available to GStreamer.
 */
EXPORT gboolean plugin_init(GstPlugin *plugin) {

    // Log vendor metadata for static builds (visible via GST_DEBUG=projectm:4)
#ifdef HAVE_PROJECTM_VENDOR_INFO
    GST_INFO("projectM vendor: ref=%s commit=%s timestamp=%s",
             PROJECTM_VENDOR_REF, PROJECTM_VENDOR_COMMIT,
             PROJECTM_VENDOR_TIMESTAMP);
#endif

    // register main plugin projectM element
    gboolean p1 = gst_element_register(plugin, "projectm", GST_RANK_NONE,
                                       GST_TYPE_PROJECTM);

    // add additional elements here..

    return p1;
}

GST_PLUGIN_DEFINE(GST_VERSION_MAJOR, GST_VERSION_MINOR, projectm,
                  PACKAGE_DESCRIPTION,
                  plugin_init, PACKAGE_VERSION, PACKAGE_LICENSE, PACKAGE_NAME,
                  PACKAGE_ORIGIN)
