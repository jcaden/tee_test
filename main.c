#include <gst/gst.h>
#include <stdlib.h>

#define GST_CAT_DEFAULT negotiation_test
GST_DEBUG_CATEGORY_STATIC (GST_CAT_DEFAULT);
#define GST_DEFAULT_NAME "tee_test"

#define TIMES 100000

#define BRANCHES_COUNT_KEY "branches-count"
#define BUFFERS_COUNT_KEY "buffers-count"
#define N_BRANCHES 200
#define NUM_BUFFERS 20

static gint times = TIMES;

static GOptionEntry entries[] = {
  {
    "number-times", 'n', 0, G_OPTION_ARG_INT, &times,
        "Number of times the test is executed", NULL
  },
  {NULL}
};

static GMainLoop *loop;
static guint error;

static gboolean
timeout_check (gpointer pipeline)
{
  gchar *timeout_file =
      g_strdup_printf ("timeout-%s", GST_OBJECT_NAME (pipeline));

  GST_DEBUG_BIN_TO_DOT_FILE_WITH_TS (GST_BIN (pipeline),
      GST_DEBUG_GRAPH_SHOW_ALL, timeout_file);
  g_free (timeout_file);

  GST_ERROR ("Test timeout on pipeline %s", GST_OBJECT_NAME (pipeline));
  g_atomic_int_set (&error, 1);
  g_main_loop_quit (loop);

  return G_SOURCE_CONTINUE;
}

static void
bus_message (GstBus * bus, GstMessage * msg, gpointer pipe)
{
  switch (GST_MESSAGE_TYPE (msg)) {
    case GST_MESSAGE_ERROR:{
      gchar *error_file = g_strdup_printf ("error-%s", GST_OBJECT_NAME (pipe));

      GST_ERROR ("Error: %" GST_PTR_FORMAT, msg);
      GST_DEBUG_BIN_TO_DOT_FILE_WITH_TS (GST_BIN (pipe),
          GST_DEBUG_GRAPH_SHOW_ALL, error_file);
      g_free (error_file);

      GST_ERROR ("Error received on bus in pipeline: %s", GST_OBJECT_NAME (pipe));
      g_atomic_int_set (&error, 1);
      g_main_loop_quit (loop);
      break;
    }
    case GST_MESSAGE_WARNING:{
      gchar *warn_file = g_strdup_printf ("warning-%s", GST_OBJECT_NAME (pipe));

      GST_WARNING ("Warning: %" GST_PTR_FORMAT, msg);
      GST_DEBUG_BIN_TO_DOT_FILE_WITH_TS (GST_BIN (pipe),
          GST_DEBUG_GRAPH_SHOW_ALL, warn_file);
      g_free (warn_file);
      break;
    }
    case GST_MESSAGE_EOS:
      GST_DEBUG ("Received eos event");
      g_main_loop_quit (loop);
      break;
    default:
      break;
  }
}
static gboolean
check_pipeline_termination (gpointer data)
{
  GstElement *pipeline = GST_ELEMENT (data);
  int *count = g_object_get_data (G_OBJECT (pipeline), BRANCHES_COUNT_KEY);

  if (count != NULL && g_atomic_int_dec_and_test (count)) {
    GST_DEBUG ("Terminating");
    g_main_loop_quit (loop);
  }

  return FALSE;
}

static GstFlowReturn
new_sample (GstElement * appsink, gpointer data)
{
  int *count = g_object_get_data (G_OBJECT (appsink), BUFFERS_COUNT_KEY);
  GstSample *sample;

  if (count == NULL) {
    count = g_malloc0 (sizeof (int));
    g_object_set_data_full (G_OBJECT (appsink), BUFFERS_COUNT_KEY, count, g_free);
  }

  g_signal_emit_by_name (appsink, "pull-sample", &sample);
  gst_sample_unref (sample);

  if (g_atomic_int_add (count, 1) == NUM_BUFFERS) {
    GST_DEBUG_OBJECT (appsink, "Terminatig");
    g_idle_add_full (G_PRIORITY_HIGH, check_pipeline_termination, gst_object_get_parent (GST_OBJECT (appsink)), g_object_unref);
  }

  return GST_FLOW_OK;
}

static GstPadProbeReturn
link_to_tee (GstPad * pad, GstPadProbeInfo * info, gpointer queue)
{
  GstElement *tee;

  if (gst_pad_is_linked (pad)) {
    return GST_PAD_PROBE_PASS;
  }

  GST_OBJECT_LOCK (pad);
  if (g_object_get_data (G_OBJECT (pad), "linking")) {
    GST_OBJECT_UNLOCK (pad);
    return GST_PAD_PROBE_PASS;
  }
  g_object_set_data (G_OBJECT (pad), "linking", GINT_TO_POINTER (TRUE));
  GST_OBJECT_UNLOCK (pad);

  tee = gst_pad_get_parent_element (pad);

  if (tee == NULL) {
    return GST_PAD_PROBE_PASS;
  }

  gst_element_link_pads (tee, GST_OBJECT_NAME (pad), queue, NULL);

  g_object_unref (tee);

  return GST_PAD_PROBE_REMOVE;
}

static gboolean
connect_branch (gpointer pipeline)
{
  GstElement *tee = gst_bin_get_by_name (GST_BIN (pipeline), "tee");
  GstElement *queue, *sink;
  GstPad *tee_src;

  if (tee == NULL) {
    g_atomic_int_set (&error, TRUE);
    goto end;
  }

  queue = gst_element_factory_make ("queue", NULL);
  sink = gst_element_factory_make ("appsink", NULL);

  g_object_set (G_OBJECT (sink), "emit-signals", TRUE, "sync", FALSE, NULL);
  g_signal_connect_data (G_OBJECT (sink), "new-sample", G_CALLBACK (new_sample),
      NULL, NULL, 0);

  gst_bin_add_many (GST_BIN (pipeline), queue, sink, NULL);
  gst_element_link (queue, sink);
  gst_element_sync_state_with_parent (queue);
  gst_element_sync_state_with_parent (sink);

  tee_src = gst_element_get_request_pad (tee, "src_%u");
  gst_pad_add_probe (tee_src, GST_PAD_PROBE_TYPE_BLOCKING, link_to_tee,
      g_object_ref (queue), g_object_unref);

  g_object_unref (tee);

end:
  return G_SOURCE_REMOVE;
}

static void
execute_test (int count)
{
  guint timeout_id;
  gchar *name = g_strdup_printf ("negotiation_test_%d", count);
  GstElement *pipeline = gst_pipeline_new (name);
  GstElement *audiotestsrc = gst_element_factory_make ("audiotestsrc", NULL);
  GstElement *tee = gst_element_factory_make ("tee", "tee");
  GstElement *queue = gst_element_factory_make ("queue", NULL);
  GstElement *sink = gst_element_factory_make ("appsink", NULL);
  gint *branches, i;

  GstBus *bus = gst_pipeline_get_bus (GST_PIPELINE (pipeline));

  g_free (name);

  if (bus == NULL) {
    GST_ERROR ("Bus is NULL");
    g_atomic_int_set (&error, 1);
    return;
  }

  gst_bus_add_signal_watch (bus);
  g_signal_connect (bus, "message", G_CALLBACK (bus_message), pipeline);

  g_object_set (G_OBJECT (sink), "emit-signals", TRUE, "sync", FALSE, NULL);
  g_signal_connect_data (G_OBJECT (sink), "new-sample", G_CALLBACK (new_sample),
      NULL, NULL, 0);

  branches = g_malloc0 (sizeof (gint));
  *branches = N_BRANCHES;
  g_object_set_data_full (G_OBJECT (pipeline), BRANCHES_COUNT_KEY, branches, g_free);
  GST_INFO ("Connecting %d branches", N_BRANCHES);

  for (i = 1; i < N_BRANCHES; i++) {
    g_timeout_add (500, connect_branch, pipeline);
  }

  gst_bin_add_many (GST_BIN (pipeline), audiotestsrc, tee, queue, sink, NULL);
  gst_element_link_many (audiotestsrc, tee, queue, sink, NULL);

  gst_element_set_state (pipeline, GST_STATE_PLAYING);

  timeout_id = g_timeout_add_seconds (35, timeout_check, pipeline);

  g_main_loop_run (loop);

  if (!g_source_remove (timeout_id)) {
    GST_ERROR ("Error removing source");
    g_atomic_int_set (&error, 1);
    return;
  }

  gst_element_set_state (pipeline, GST_STATE_NULL);
  gst_bus_remove_signal_watch (bus);
  g_object_unref (bus);
  g_object_unref (pipeline);
}

int
main(int argc, char ** argv)
{
  GOptionContext *context;
  GError *gerror = NULL;
  int count = 0;

  error = 0;

  gst_init (&argc, &argv);

  GST_DEBUG_CATEGORY_INIT (GST_CAT_DEFAULT, GST_DEFAULT_NAME, 0,
      GST_DEFAULT_NAME);

  context = g_option_context_new (NULL);
  g_option_context_add_main_entries (context, entries, NULL);
  g_option_context_add_group (context, gst_init_get_option_group () );

  if (!g_option_context_parse (context, &argc, &argv, &gerror) ) {
    GST_ERROR ("option parsing failed: %s\n", gerror->message);
    g_option_context_free (context);
    g_error_free (gerror);
    return 1;
  }

  g_option_context_free (context);

  loop = g_main_loop_new (NULL, TRUE);

  while (count < times && !g_atomic_int_get (&error)) {
    execute_test (count++);
    GST_INFO ("Executed %d times", count);
    if (g_atomic_int_get (&error)) {
      GST_ERROR ("Test terminated with error");
    } else {
      GST_INFO ("Test terminated correctly");
    }
  }

  g_main_loop_unref (loop);

  return error;
}
