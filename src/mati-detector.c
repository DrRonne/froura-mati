#include "mati-detector.h"

GST_DEBUG_CATEGORY_STATIC (mati_detector_debug);

struct _MatiDetector
{
    GObject parent_instance;

    MatiCommunicator *communicator;

    gboolean is_in_motion;

    GstElement *pipeline;

    GstElement *queue_connect;
    GstElement *motion;
    GstElement *valve;

    gboolean is_encoding;
};

G_DEFINE_TYPE (MatiDetector, mati_detector, G_TYPE_OBJECT);

enum MatiDetectorSignals
{
    EOS_MESSAGE,
    PIPELINE_ERROR,
    PENDING,
    PLAYING,
    LAST
};

static guint signals[LAST];

GstStateChangeReturn mati_detector_stop (MatiDetector *self);

static void
mati_detector_init (MatiDetector *self)
{
    self->is_in_motion = FALSE;
    self->is_encoding = FALSE;
}

static void
mati_detector_finalize (GObject *object)
{
    MatiDetector *self = MATI_DETECTOR (object);

    g_return_if_fail (GST_IS_ELEMENT (self->pipeline));

    if (mati_detector_stop (self) != GST_STATE_CHANGE_SUCCESS)
    {
        g_critical ("Couldn't stop the pipeline, exiting...");
        return;
    }
    else
    {
        GST_DEBUG ("Deleting pipeline");
        gst_clear_object (&self->pipeline);
    }

    G_OBJECT_CLASS (mati_detector_parent_class)->finalize (object);
}

static void
mati_detector_class_init (MatiDetectorClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS (klass);

    object_class->finalize = mati_detector_finalize;

    signals[EOS_MESSAGE] = g_signal_new ("eos", MATI_TYPE_DETECTOR, G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL, G_TYPE_NONE, 0);
    signals[PIPELINE_ERROR] = g_signal_new ("error", MATI_TYPE_DETECTOR, G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL, G_TYPE_NONE, 2, GST_TYPE_ELEMENT, G_TYPE_STRING);
    signals[PENDING] = g_signal_new ("pending", MATI_TYPE_DETECTOR, G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL, G_TYPE_NONE, 0);
    signals[PLAYING] = g_signal_new ("playing", MATI_TYPE_DETECTOR, G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL, G_TYPE_NONE, 0);
}

static gboolean
on_pipeline_message (GstBus *bus, GstMessage *message, gpointer user_data)
{
    MatiDetector *self = MATI_DETECTOR (user_data);

    g_return_val_if_fail (GST_IS_BUS (bus), FALSE);
    g_return_val_if_fail (GST_IS_MESSAGE (message), FALSE);

    switch (GST_MESSAGE_TYPE (message))
    {
        case GST_MESSAGE_EOS:
        {
            g_signal_emit (self, signals[EOS_MESSAGE], 0);
            break;
        }
        case GST_MESSAGE_ERROR:
        {
            g_autoptr (GError) err = NULL;

            gst_message_parse_error (message, &err, NULL);
            g_signal_emit (self, signals[PIPELINE_ERROR], 0, message->src, err->message);
            break;
        }
        case GST_MESSAGE_STATE_CHANGED:
        {
            if (GST_MESSAGE_SRC (message) == (GObject *) self->pipeline)
            {
                GstState old_state, new_state;

                gst_message_parse_state_changed (message, &old_state, &new_state, NULL);
                GST_INFO ("Pipeline went from state %s to state %s", gst_element_state_get_name (old_state), gst_element_state_get_name (new_state));
                GST_DEBUG_BIN_TO_DOT_FILE (GST_BIN (self->pipeline), GST_DEBUG_GRAPH_SHOW_ALL, "/tmp/pipeline");
            }
            break;
        }
        case GST_MESSAGE_ELEMENT:
        {
            if (GST_MESSAGE_SRC (message) == (GObject *) self->motion)
            {
                GST_INFO ("motion %s!", self->is_in_motion ? "stopped" : "started");
                self->is_in_motion = !self->is_in_motion;
                g_object_set (G_OBJECT (self->valve), "drop", !self->is_in_motion, NULL);
            }
        }
        default:
            break;
    }
    return TRUE;
}

MatiDetector *
mati_detector_new (MatiCommunicator *communicator)
{
    g_autoptr (MatiDetector) self = g_object_new (MATI_TYPE_DETECTOR, NULL);
    g_autoptr (GstBus) pipeline_bus = NULL;

    self->pipeline = gst_pipeline_new ("mati");
    if (self->pipeline == NULL)
        return NULL;
    
    pipeline_bus = gst_element_get_bus (self->pipeline);
    gst_bus_add_watch (pipeline_bus, on_pipeline_message, self);

    self->communicator = communicator;

    return g_steal_pointer (&self);
}

void
mati_detector_start (MatiDetector *self)
{
    g_message ("mati_detector_start");
    gboolean change_success = FALSE;

    g_return_if_fail (MATI_IS_DETECTOR (self));
    g_return_if_fail (GST_IS_ELEMENT (self->pipeline));

    GST_INFO ("Setting pipeline to playing state...");

    switch (gst_element_set_state (self->pipeline, GST_STATE_PLAYING))
    {
        case GST_STATE_CHANGE_SUCCESS:
        {
            GST_INFO ("Pipeline state succesfully changed to playing!");
            change_success = TRUE;
            break;
        }
        case GST_STATE_CHANGE_ASYNC:
        {
            GST_INFO ("The state change will happen asynchronously");
            change_success = TRUE;
            break;
        }
        case GST_STATE_CHANGE_FAILURE:
        {
            GST_INFO ("Failed to change state to playing!!");
            break;
        }
        case GST_STATE_CHANGE_NO_PREROLL:
        {
            GST_INFO ("State change succeeded, but element cannot produce data while paused");
            break;
        }
        default:
            break;
    }
    if (change_success)
        g_signal_emit (self, signals[PENDING], 0);
}

GstStateChangeReturn
mati_detector_stop (MatiDetector *self)
{
    g_autoptr (GstBus) bus = NULL;
    GstMessage *message = NULL;
    GstStateChangeReturn ret = GST_STATE_CHANGE_SUCCESS;

    g_message ("Stopping the pipeline...");
    g_return_val_if_fail (GST_IS_BIN (self->pipeline), GST_STATE_CHANGE_FAILURE);

    bus = gst_element_get_bus (GST_ELEMENT_CAST (self->pipeline));
    g_return_val_if_fail (GST_IS_BUS (bus), GST_STATE_CHANGE_FAILURE);

    message = gst_message_new_warning (GST_OBJECT (self->pipeline), NULL, "pipeline interrupted");
    gst_bus_post (bus, message);

    g_message ("Setting pipeline to NULL...");

    ret = gst_element_set_state (GST_ELEMENT (self->pipeline), GST_STATE_NULL);

    if (ret == GST_STATE_CHANGE_FAILURE)
    {
        g_critical ("Failed to set pipeline to NULL!");
    }
    else if (ret == GST_STATE_CHANGE_ASYNC)
    {
        g_message ("Waiting for pipeline to become NULL...");
        ret = gst_element_get_state (GST_ELEMENT_CAST (self->pipeline), NULL, NULL, GST_SECOND);
        if (ret == GST_STATE_CHANGE_FAILURE)
        {
            g_critical ("Failed to set pipeline to NULL!");
        }
        else if (ret == GST_STATE_CHANGE_ASYNC)
        {
            g_critical ("Failed to set pipeline to NULL within timeout!");
        }
    }
    return ret;
}

static gboolean
dump_dot (MatiDetector *self)
{
    g_message ("dumping dot");
    GST_DEBUG_BIN_TO_DOT_FILE (GST_BIN (self->pipeline), GST_DEBUG_GRAPH_SHOW_ALL, "/tmp/pipeline");
}

static void
pad_added_handler (GstElement *src, GstPad *new_pad, MatiDetector *self)
{
    g_message ("pad_added_handler");
    GstPad *sink_pad = gst_element_get_static_pad (self->queue_connect, "sink");
    GstPadLinkReturn ret;
    g_autoptr (GstCaps) new_pad_caps = NULL;
    GstStructure *new_pad_struct = NULL;
    const gchar *new_pad_type = NULL;

    g_message ("Received new pad '%s' from '%s':\n", GST_PAD_NAME (new_pad), GST_ELEMENT_NAME (src));

    new_pad_caps = gst_pad_get_current_caps (new_pad);
    new_pad_struct = gst_caps_get_structure (new_pad_caps, 0);
    new_pad_type = gst_structure_get_name (new_pad_struct);

    /* If our converter is already linked, we have nothing to do here */
    if (gst_pad_is_linked (sink_pad))
    {
        g_message ("We are already linked. Ignoring.\n");
        goto exit;
    }

    /* Attempt the link */
    ret = gst_pad_link (new_pad, sink_pad);
    if (GST_PAD_LINK_FAILED (ret))
    {
        GST_DEBUG_BIN_TO_DOT_FILE(self->pipeline, GST_DEBUG_GRAPH_SHOW_ALL, "/tmp/dotfile");
        g_critical ("Type is '%s' but link failed.\n", new_pad_type);
    } 
    else
    {
        g_message ("Link succeeded (type '%s').\n", new_pad_type);
    }

    g_timeout_add (10000, dump_dot, self);

exit:
    gst_object_unref (sink_pad);
}

static GstElement*
build_tcpsink (int tcp_port)
{
    GstElement *bin, *queue_streamer, *streamer_clientsink;
    GstGhostPad *video_sink_pad;

    queue_streamer = gst_element_factory_make ("queue", NULL);
    g_return_val_if_fail (GST_IS_ELEMENT (queue_streamer), FALSE);

    streamer_clientsink = gst_element_factory_make ("tcpclientsink", NULL);
    g_return_val_if_fail (GST_IS_ELEMENT (streamer_clientsink), FALSE);
    g_object_set (G_OBJECT (streamer_clientsink), "host", "localhost", "port", tcp_port, NULL);

    bin = gst_bin_new ("tcpsinkbin");
    gst_bin_add_many (GST_BIN (bin), queue_streamer, streamer_clientsink, NULL);
    if (!gst_element_link (queue_streamer, streamer_clientsink))
        g_critical ("Failed to link tcpsink elements!");

    video_sink_pad = gst_ghost_pad_new ("sink", gst_element_get_static_pad (queue_streamer, "sink"));
    if (!gst_element_add_pad (bin, video_sink_pad))
        g_critical ("Failed to set videosink pad in tcpsink bin!");

    return bin;
}

static GstElement*
build_filesink ()
{
    GstElement *bin, *queue_detector, *writer_detector;
    GstGhostPad *video_sink_pad;

    queue_detector = gst_element_factory_make ("queue", NULL);
    g_return_val_if_fail (GST_IS_ELEMENT (queue_detector), FALSE);

    writer_detector = gst_element_factory_make ("filesink", NULL);
    g_return_val_if_fail (GST_IS_ELEMENT (writer_detector), FALSE);
    g_object_set (G_OBJECT (writer_detector), "location", "/tmp/test.ogg", NULL);

    bin = gst_bin_new ("filesinkbin");
    gst_bin_add_many (GST_BIN (bin), queue_detector, writer_detector, NULL);
    if (!gst_element_link (queue_detector, writer_detector))
        g_critical ("Failed to link filesink elements!");

    video_sink_pad = gst_ghost_pad_new ("videosink", gst_element_get_static_pad (queue_detector, "sink"));
    if (!gst_element_add_pad (bin, video_sink_pad))
        g_critical ("Failed to set videosink pad in filesink bin!");

    return bin;
}

static GstElement*
build_fakesink ()
{
    GstElement *bin, *queue_fakesink, *fakesink;
    GstGhostPad *video_sink_pad;

    queue_fakesink = gst_element_factory_make ("queue", NULL);
    g_return_val_if_fail (GST_IS_ELEMENT (queue_fakesink), FALSE);

    fakesink = gst_element_factory_make ("fakevideosink", NULL);
    g_return_val_if_fail (GST_IS_ELEMENT (fakesink), FALSE);

    bin = gst_bin_new ("fakesinkbin");
    gst_bin_add_many (GST_BIN (bin), queue_fakesink, fakesink, NULL);
    if (!gst_element_link (queue_fakesink, fakesink))
        g_critical ("Failed to link fakesink elements!");

    video_sink_pad = gst_ghost_pad_new ("videosink", gst_element_get_static_pad (queue_fakesink, "sink"));
    if (!gst_element_add_pad (bin, video_sink_pad))
        g_critical ("Failed to set videosink pad in fakesink bin!");

    return bin;
}

static GstElement*
build_encoder_pipeline ()
{
    GstElement *bin;
    GstElement *videoconvert;
    GstElement *streamer_encoder;
    GstElement *streamer_mux;

    GstGhostPad *video_sink_pad;
    GstGhostPad *video_src_pad;

    videoconvert = gst_element_factory_make ("videoconvert", NULL);
    g_return_val_if_fail (GST_IS_ELEMENT (videoconvert), FALSE);

    streamer_encoder = gst_element_factory_make ("theoraenc", NULL);
    g_return_val_if_fail (GST_IS_ELEMENT (streamer_encoder), FALSE);

    streamer_mux = gst_element_factory_make ("oggmux", NULL);
    g_return_val_if_fail (GST_IS_ELEMENT (streamer_mux), FALSE);

    bin = gst_bin_new ("encoderbin");
    gst_bin_add_many (GST_BIN (bin),
                      videoconvert, streamer_encoder, streamer_mux,
                      NULL);

    if (!gst_element_link_many (videoconvert, streamer_encoder, streamer_mux,
                                NULL))
        g_critical ("Couldn't link all encoder elements!");

    video_sink_pad = gst_ghost_pad_new ("videosink", gst_element_get_static_pad (videoconvert, "sink"));
    if (!gst_element_add_pad (bin, video_sink_pad))
        g_critical ("Failed to set videosink pad in encoder bin!");
    video_src_pad = gst_ghost_pad_new ("videosrc", gst_element_get_static_pad (streamer_mux, "src"));
    if (!gst_element_add_pad (bin, video_src_pad))
        g_critical ("Failed to set videosrc pad in encoder bin!");
    
    return bin;
}

static GstElement*
build_common_pipeline (MatiDetector *self,
                       gchar        *uri)
{
    GstElement *bin;

    GstElement *videosource;
    GstElement *queue_common;
    GstElement *videoconvert;
    GstElement *motioncells;

    GstGhostPad *video_src_pad;

    videosource = gst_element_factory_make ("uridecodebin", NULL);
    g_return_val_if_fail (GST_IS_ELEMENT (videosource), FALSE);
    g_object_set (G_OBJECT (videosource), "uri", uri, NULL);
    g_signal_connect (videosource, "pad-added", G_CALLBACK (pad_added_handler), self);

    queue_common = gst_element_factory_make ("queue", NULL);
    g_return_val_if_fail (GST_IS_ELEMENT (queue_common), FALSE);
    self->queue_connect = queue_common;

    videoconvert = gst_element_factory_make ("videoconvert", NULL);
    g_return_val_if_fail (GST_IS_ELEMENT (videoconvert), FALSE);

    motioncells = gst_element_factory_make ("motioncells", NULL);
    g_return_val_if_fail (GST_IS_ELEMENT (motioncells), FALSE);
    g_object_set (G_OBJECT (motioncells), "display", FALSE, NULL);
    self->motion = motioncells;

    bin = gst_bin_new ("commonbin");
    gst_bin_add_many (GST_BIN (bin), videosource,
                      queue_common, videoconvert, motioncells,
                      NULL);

    if (!gst_element_link_many (queue_common, videoconvert, motioncells,
                                NULL))
        g_critical ("Couldn't link all common pipeline elements!");

    video_src_pad = gst_ghost_pad_new ("videosrc", gst_element_get_static_pad (motioncells, "src"));
    if (!gst_element_add_pad (bin, video_src_pad))
        g_critical ("Failed to set videosrc pad in common pipeline bin!");

    return bin;
}

gboolean
mati_detector_build (MatiDetector *self,
                     gchar        *uri,
                     gboolean      clockoverlay,
                     int           tcp_port)
{
    g_return_val_if_fail (MATI_IS_DETECTOR (self), FALSE);

    GstElement *common_pipeline;
    GstElement *tee;
    GstElement *fake_sink_bin;
    GstElement *encoder_bin;
    GstElement *tee2;
    GstElement *tcp_bin;
    GstElement *file_sink_bin;

    common_pipeline = build_common_pipeline (self, uri);
    tee = gst_element_factory_make ("tee", NULL);
    g_return_val_if_fail (GST_IS_ELEMENT (tee), FALSE);
    fake_sink_bin = build_fakesink ();
    encoder_bin = build_encoder_pipeline ();
    tee2 = gst_element_factory_make ("tee", NULL);
    tcp_bin = build_tcpsink (tcp_port);
    // file_sink_bin = build_filesink ();

    gst_bin_add_many (GST_BIN (self->pipeline), common_pipeline, tee, fake_sink_bin, encoder_bin, tee2, tcp_bin, /*file_sink_bin,*/ NULL);
    g_message ("about to dump dot");
    dump_dot (self);

    if (!gst_element_link_many (common_pipeline, tee, fake_sink_bin, NULL))
    {
        g_critical ("Couldn't link common pipeline to fakesinkbin!");
        return FALSE;
    }

    if (!gst_element_link_many (tee, encoder_bin, tee2, NULL))
    {
        g_critical ("Couldn't link common pipeline to encoderbin!");
        return FALSE;
    }

    if (!gst_element_link_many (tee2, tcp_bin, NULL))
    {
        g_critical ("Couldn't link encoderbin to tcp bin!");
        return FALSE;
    }

    // if (!gst_element_link_many (tee2, file_sink_bin, NULL))
    // {
    //     g_critical ("Couldn't link encoderbin to filesink bin!");
    //     return FALSE;
    // }

    return TRUE;
}