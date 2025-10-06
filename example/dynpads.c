#include <gst/gst.h>

#include <gst/gl/gstglmemory.h>

/**
 * Example for a "pad added" signal callback handler for handling gst
 * demuxer-like elements.
 *
 * @param element Callback param for the gst element receiving the event.
 * @param new_pad The pad being added.
 * @param data The gst element adding the pad (e.g. demuxer).
 */
static void on_pad_added(GstElement *element, GstPad *new_pad, gpointer data) {

  GstPad *sink_pad;
  GstElement *downstream_element = GST_ELEMENT(data);
  GstPadLinkReturn ret;
  GstCaps *new_pad_caps = NULL;
  GstStructure *new_pad_struct = NULL;
  const gchar *new_pad_type = NULL;

  g_print("Received new pad '%s' from '%s':\n", GST_PAD_NAME(new_pad),
          GST_ELEMENT_NAME(element));

  /* Check the new pad's capabilities to determine its media type */
  new_pad_caps = gst_pad_get_current_caps(new_pad);
  new_pad_struct = gst_caps_get_structure(new_pad_caps, 0);
  new_pad_type = gst_structure_get_name(new_pad_struct);

  /* Get the sink pad from the downstream element (either audio or video queue)
   */
  if (g_str_has_prefix(new_pad_type, "video/x-raw")) {
    sink_pad = gst_element_get_static_pad(downstream_element, "sink");
  } else if (g_str_has_prefix(new_pad_type, "audio/x-raw")) {
    sink_pad = gst_element_get_static_pad(downstream_element, "sink");
  } else {
    g_print("  It has type '%s', which we don't handle. Ignoring.\n",
            new_pad_type);
    goto exit;
  }

  /* Check if the pads are already linked */
  if (gst_pad_is_linked(sink_pad)) {
    g_print("  We already linked pad %s. Ignoring.\n", GST_PAD_NAME(new_pad));
    goto exit;
  }

  /* Link the new pad to the sink pad */
  ret = gst_pad_link(new_pad, sink_pad);
  if (GST_PAD_LINK_FAILED(ret)) {
    g_print("  Type is '%s' but link failed.\n", new_pad_type);
  } else {
    g_print("  Link succeeded (type '%s').\n", new_pad_type);
  }

exit:
  /* Clean up */
  if (new_pad_caps != NULL)
    gst_caps_unref(new_pad_caps);

  if (sink_pad != NULL)
    gst_object_unref(sink_pad);
}

/**
 * Main function to build and run the pipeline to consume a live audio stream
 * and render projectM to an OpenGL window in real-time.
 *
 * souphttpsrc location=... is-live=true ! queue  ! decodebin ! audioconvert !
 * "audio/x-raw, format=S16LE, rate=44100, channels=2, layout=interleaved" !
 * projectm preset=... preset-duration=... mesh-size=48,32 texture-dir=... !
 * video/x-raw(memory:GLMemory),width=1920,height=1080,framerate=60/1 ! queue
 * leaky=downstream max-size-buffers=1 ! glimagesink sync=true
 */
int main(int argc, char *argv[]) {
  GstElement *source, *demuxer, *queue, *audioconvert, *audio_capsfilter,
      *identity, *projectm_plugin, *video_capsfilter, *sync_queue, *sink;
  GstBus *bus;
  GstElement *pipeline;

  gst_init(&argc, &argv);

  // make audio caps
  GstCaps *audio_caps =
      gst_caps_new_simple("audio/x-raw", "format", G_TYPE_STRING, "S16LE",
                          "rate", G_TYPE_INT, 44100, "channels", G_TYPE_INT, 2,
                          "layout", G_TYPE_STRING, "interleaved", NULL);

  // make video caps
  // todo: adjust caps as desired, keep in mind the hardware needs to be able to
  // keep up in order for this plugin to work flawlessly.
  GstCaps *video_caps = gst_caps_new_simple(
      "video/x-raw", "format", G_TYPE_STRING, "RGBA", "width", G_TYPE_INT, 1920,
      "height", G_TYPE_INT, 1080, "framerate", GST_TYPE_FRACTION, 60, 1, NULL);

  // Create the GL memory feature set.
  GstCapsFeatures *features =
      gst_caps_features_new_single(GST_CAPS_FEATURE_MEMORY_GL_MEMORY);

  // Add the GL memory feature set to the structure.
  gst_caps_set_features(video_caps, 0, features);

  // Create pipeline elements
  source = gst_element_factory_make("souphttpsrc", "source");
  g_object_set(source,
               // todo: configure your stream here..
               "location", "http://your-stream-url", "is-live", TRUE, NULL);

  // basic stream buffering
  queue = gst_element_factory_make("queue", "queue");

  // decodebin to decode the stream audio format
  demuxer = gst_element_factory_make("decodebin", "demuxer");
  g_object_set(G_OBJECT(demuxer), "max-size-time", "100000000", NULL);

  // convert the audio stream to something we can understand (if needed)
  audioconvert = gst_element_factory_make("audioconvert", "audioconvert");

  // tell pipeline which audio format we need
  audio_capsfilter = gst_element_factory_make("capsfilter", "audio_capsfilter");
  g_object_set(G_OBJECT(audio_capsfilter), "caps", audio_caps, NULL);

  // create an identity element to provide a sream clock, since we won't get one
  // from souphttpsrc
  identity = gst_element_factory_make("identity", "identity");
  g_object_set(G_OBJECT(identity), "single-segment", TRUE, "sync", TRUE, NULL);

  // configure projectM plugin
  projectm_plugin = gst_element_factory_make("projectm", "projectm");

  // todo: configure your settings here..
  g_object_set(G_OBJECT(projectm_plugin), "preset-duration", 10.0,
               //"preset", "/your/presets/directory",
               "mesh-size", "48,32",
               //"texture-dir", "/your/presets-milkdrop-texture-pack-directory",
               NULL);

  // set video caps we want
  video_capsfilter = gst_element_factory_make("capsfilter", "video_capsfilter");
  g_object_set(G_OBJECT(video_capsfilter), "caps", video_caps, NULL);

  // optional: create a queue in front of the glimagesink to throw out buffers
  // that piling up in front of rendering just keep the latest one, the others
  // will most likely be late
  sync_queue = gst_element_factory_make("queue", "sync_queue");
  // 0 (no): The default behavior. The queue is not leaky and will block when
  // full. 1 (upstream): The queue drops new incoming buffers when it is full.
  // 2 (downstream): The queue drops the oldest buffers in the queue when it is
  // full.
  g_object_set(G_OBJECT(sync_queue), "leaky", 2, "max-size-buffers", 1, NULL);

  // create sink for real-time rendering (synced to the pipeline clock)
  sink = gst_element_factory_make("glimagesink", "sink");
  g_object_set(G_OBJECT(sink), "sync", TRUE, NULL);

  pipeline = gst_pipeline_new("test-pipeline");

  if (!pipeline || !source || !demuxer || !queue || !projectm_plugin ||
      !video_capsfilter || !sync_queue || !sink) {
    g_printerr("One or more elements could not be created. Exiting.\n");
    return -1;
  }

  /* Set up the pipeline */
  gst_bin_add_many(GST_BIN(pipeline), source, queue, demuxer, audioconvert,
                   audio_capsfilter, identity, projectm_plugin,
                   video_capsfilter, sync_queue, sink, NULL);

  /* Link the elements (but not the demuxer's dynamic pad yet) */
  if (!gst_element_link(source, queue)) {
    g_printerr("Elements could not be linked (source to queue). Exiting.\n");
    return -1;
  }
  if (!gst_element_link(queue, demuxer)) {
    g_printerr(
        "Elements could not be linked (queue, audioconvert). Exiting.\n");
    return -1;
  }
  /* not yet!
  if (!gst_element_link(demuxer, audioconvert)) {
    g_printerr("Elements could not be linked (demuxer, queue). Exiting.\n");
    return -1;
  }
  */
  if (!gst_element_link(audioconvert, audio_capsfilter)) {
    g_printerr("Elements could not be linked (audioconvert to "
               "audio_capsfilter). Exiting.\n");
    return -1;
  }
  if (!gst_element_link(audio_capsfilter, identity)) {
    g_printerr("Elements could not be linked (audio_capsfilter to identity). "
               "Exiting.\n");
    return -1;
  }
  if (!gst_element_link(identity, projectm_plugin)) {
    g_printerr("Elements could not be linked (identity to projectm_plugin). "
               "Exiting.\n");
    return -1;
  }
  if (!gst_element_link(projectm_plugin, video_capsfilter)) {
    g_printerr("Elements could not be linked (projectm_plugin to capsfilter). "
               "Exiting.\n");
    return -1;
  }
  if (!gst_element_link(video_capsfilter, sync_queue)) {
    g_printerr("Elements could not be linked (video_capsfilter to sync_queue). "
               "Exiting.\n");
    return -1;
  }
  if (!gst_element_link(sync_queue, sink)) {
    g_printerr("Elements could not be linked (sync_queue to sink). Exiting.\n");
    return -1;
  }

  gst_caps_unref(video_caps);
  gst_caps_unref(audio_caps);

  /* Connect the "pad-added" signal */
  g_signal_connect(demuxer, "pad-added", G_CALLBACK(on_pad_added),
                   audioconvert);

  /* Set the pipeline to the PLAYING state */
  GMainLoop *loop = g_main_loop_new(NULL, FALSE);
  gst_element_set_state(pipeline, GST_STATE_PLAYING);
  g_main_loop_run(loop);

  /* Wait until error or EOS */
  bus = gst_element_get_bus(pipeline);
  gst_bus_timed_pop_filtered(bus, GST_CLOCK_TIME_NONE,
                             GST_MESSAGE_ERROR | GST_MESSAGE_EOS);

  /* Clean up */
  gst_element_set_state(pipeline, GST_STATE_NULL);
  gst_object_unref(bus);
  gst_object_unref(pipeline);

  return 0;
}
