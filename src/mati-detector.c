#include "mati-detector.h"

GST_DEBUG_CATEGORY_STATIC (mati_detector_debug);

struct _MatiDetector
{
    GObject parent_instance;

    MatiCommunicator *communicator;

    gboolean is_in_motion;

    GstElement *pipeline;

    GstElement *convert;
    GstElement *motion;
    GstElement *valve;
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
    signals[PIPELINE_ERROR] = g_signal_new ("eos", MATI_TYPE_DETECTOR, G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL, G_TYPE_NONE, 2, GST_TYPE_ELEMENT, G_TYPE_STRING);
    signals[PENDING] = g_signal_new ("eos", MATI_TYPE_DETECTOR, G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL, G_TYPE_NONE, 0);
    signals[PLAYING] = g_signal_new ("eos", MATI_TYPE_DETECTOR, G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL, G_TYPE_NONE, 0);
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
            }
            break;
        }
        case GST_MESSAGE_ELEMENT:
        {
            if (GST_MESSAGE_SRC (message) == (GObject *) self->motion)
            {
                g_message ("motion");
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

static void
pad_added_handler (GstElement *src, GstPad *new_pad, MatiDetector *self)
{
    GstPad *sink_pad = gst_element_get_static_pad (self->convert, "sink");
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
        GST_DEBUG_BIN_TO_DOT_FILE(self->pipeline, GST_DEBUG_GRAPH_SHOW_ALL, "/tmp/dotfile.dot");
        g_critical ("Type is '%s' but link failed.\n", new_pad_type);
    } 
    else
    {
        g_message ("Link succeeded (type '%s').\n", new_pad_type);
    }

exit:
    gst_object_unref (sink_pad);
}

static void
file_written_handler (GstElement *src, guint *fragment_id, MatiDetector *self)
{
    g_message ("file written, next one: %d", fragment_id);
    mati_communicator_emit_next_file_signal (self->communicator, fragment_id);
}

gboolean
mati_detector_build (MatiDetector *self, gchar *uri, gboolean clockoverlay)
{
    g_return_val_if_fail (MATI_IS_DETECTOR (self), FALSE);
    GstElement *clockoverlay_detector;
    GstElement *clockoverlay_uploader;

    GstElement *videosource = gst_element_factory_make ("uridecodebin", NULL);
    g_return_val_if_fail (GST_IS_ELEMENT (videosource), FALSE);
    g_object_set (G_OBJECT (videosource), "uri", uri, NULL);
    g_signal_connect (videosource, "pad-added", G_CALLBACK (pad_added_handler), self);

    GstElement *videoconvert = gst_element_factory_make ("videoconvert", NULL);
    g_return_val_if_fail (GST_IS_ELEMENT (videoconvert), FALSE);
    self->convert = videoconvert;

    GstElement *tee = gst_element_factory_make ("tee", NULL);
    g_return_val_if_fail (GST_IS_ELEMENT (tee), FALSE);

    GstElement *queue_detector = gst_element_factory_make ("queue", NULL);
    g_return_val_if_fail (GST_IS_ELEMENT (queue_detector), FALSE);

    GstElement *videoconvert2 = gst_element_factory_make ("videoconvert", NULL);
    g_return_val_if_fail (GST_IS_ELEMENT (videoconvert2), FALSE);

    GstElement *motioncells = gst_element_factory_make ("motioncells", NULL);
    g_return_val_if_fail (GST_IS_ELEMENT (motioncells), FALSE);
    g_object_set (G_OBJECT (motioncells), "display", FALSE, NULL);
    self->motion = motioncells;

    GstElement *videoconvert3 = gst_element_factory_make ("videoconvert", NULL);
    g_return_val_if_fail (GST_IS_ELEMENT (videoconvert3), FALSE);

    GstElement *encoder_detector = gst_element_factory_make ("x264enc", NULL);
    g_return_val_if_fail (GST_IS_ELEMENT (encoder_detector), FALSE);
    // g_object_set (G_OBJECT (encoder_detector), "tune", "zerolatency", "speed-preset", 1, NULL);

    GstElement *parser_detector = gst_element_factory_make ("h264parse", NULL);
    g_return_val_if_fail (GST_IS_ELEMENT (parser_detector), FALSE);

    GstElement *writer_detector = gst_element_factory_make ("splitmuxsink", NULL);
    g_return_val_if_fail (GST_IS_ELEMENT (writer_detector), FALSE);
    g_object_set (G_OBJECT (writer_detector), "max-size-time", 3600000000000, "location", "detector%02d.mov", NULL);


    GstElement *queue_uploader = gst_element_factory_make ("queue", NULL);
    g_return_val_if_fail (GST_IS_ELEMENT (queue_uploader), FALSE);

    GstElement *valve_uploader = gst_element_factory_make ("valve", NULL);
    g_return_val_if_fail (GST_IS_ELEMENT (valve_uploader), FALSE);
    g_object_set (G_OBJECT (valve_uploader), "drop", TRUE, NULL);
    self->valve = valve_uploader;

    GstElement *encoder_uploader = gst_element_factory_make ("x264enc", NULL);
    g_return_val_if_fail (GST_IS_ELEMENT (encoder_uploader), FALSE);
    // g_object_set (G_OBJECT (encoder_uploader), "tune", "zerolatency", "speed-preset", 1, NULL);

    GstElement *parser_uploader = gst_element_factory_make ("h264parse", NULL);
    g_return_val_if_fail (GST_IS_ELEMENT (parser_uploader), FALSE);

    GstElement *writer_uploader = gst_element_factory_make ("splitmuxsink", NULL);
    g_return_val_if_fail (GST_IS_ELEMENT (writer_uploader), FALSE);
    g_object_set (G_OBJECT (writer_uploader), "max-size-time", 30000000000, "location", "uploader%02d.mov", NULL);
    g_signal_connect (writer_uploader, "format-location", G_CALLBACK (file_written_handler), self);

    gst_bin_add_many (GST_BIN (self->pipeline), videosource, videoconvert, tee,
                                                queue_detector, videoconvert2, motioncells, videoconvert3, encoder_detector, parser_detector, writer_detector,
                                                queue_uploader, valve_uploader, encoder_uploader, parser_uploader, writer_uploader,
                                                NULL);

    if (clockoverlay)
    {
        clockoverlay_detector = gst_element_factory_make ("clockoverlay", NULL);
        g_return_val_if_fail (GST_IS_ELEMENT (clockoverlay_detector), FALSE);
        clockoverlay_uploader = gst_element_factory_make ("clockoverlay", NULL);
        g_return_val_if_fail (GST_IS_ELEMENT (clockoverlay_uploader), FALSE);
        gst_bin_add_many (GST_BIN (self->pipeline), clockoverlay_detector, clockoverlay_uploader, NULL);
    }

    if (!gst_element_link_many (videoconvert, tee, NULL))
    {
        g_critical ("Couldn't link common pipeline!");
        return FALSE;
    }

    if (!gst_element_link_many (tee, queue_detector, videoconvert2, NULL))
    {
        g_critical ("Couldn't link all detector elements!(1)");
        return FALSE;
    }
    GstCaps *detector_caps = gst_caps_from_string("video/x-raw format=RGB");
    if (!gst_element_link_filtered (videoconvert2, motioncells, detector_caps))
    {
        g_critical ("couldn't link videoconvert2 and motioncells");
    }
    if (clockoverlay)
    {
        if (!gst_element_link_many (motioncells, clockoverlay_detector, videoconvert3, encoder_detector, parser_detector, writer_detector, NULL))
        {
            g_critical ("Couldn't link all detector elements!(2)");
            return FALSE;
        }

        if (!gst_element_link_many (tee, queue_uploader, valve_uploader, clockoverlay_uploader, encoder_uploader, parser_uploader, writer_uploader, NULL))
        {
            g_critical ("Couldn't link all uploader elements!");
            return FALSE;
        }
    }
    else
    {
        if (!gst_element_link_many (motioncells, videoconvert3, encoder_detector, parser_detector, writer_detector, NULL))
        {
            g_critical ("Couldn't link all detector elements!(2)");
            return FALSE;
        }

        if (!gst_element_link_many (tee, queue_uploader, valve_uploader, encoder_uploader, parser_uploader, writer_uploader, NULL))
        {
            g_critical ("Couldn't link all uploader elements!");
            return FALSE;
        }
    }

    return TRUE;
}