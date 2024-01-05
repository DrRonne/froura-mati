#include "mati-detector.h"

#define TCP_BIN_SUBNAME "tcpbin_"
#define TCP_BIN_SUBNAME_LENGTH 7
#define TCP_CLIENT_NAME "tcpclientsink"
#define TCP_SUBBIN_NAME "tcpsinkbin"
#define ENCODER_ELEMENT_NAME "encoder"
#define DECODEBIN_NAME "uridecodebin"
#define FILESINK_NAME "filesink"

GST_DEBUG_CATEGORY_STATIC (mati_detector_debug);

struct _MatiDetector
{
    GObject parent_instance;

    MatiCommunicator *communicator;

    gboolean is_in_motion;

    GstElement *pipeline;

    GstElement *tee;
    GstElement *encoder_bin;
    GstElement *file_sink_bin;

    GstElement *queue_connect;
    GstElement *encoder_queue;
    GstElement *mux_queue;
    GstElement *mux;
    GstElement *motion;

    gulong block_pad_id;
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

static GstElement* build_encoder_pipeline ();
static GstElement* build_filesink ();
static GstElement* build_tcpsink (int tcp_port);

static void
mati_detector_init (MatiDetector *self)
{
    self->is_in_motion = FALSE;
    self->file_sink_bin = NULL;
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
mati_detector_setup_filesink_pipeline (MatiDetector *self)
{
    g_message ("Setting up filesink pipeline");

    self->encoder_bin = build_encoder_pipeline (self);
    self->file_sink_bin = build_filesink ();
    gst_bin_add_many (GST_BIN (self->pipeline), self->encoder_bin, self->file_sink_bin, NULL);
    if (!gst_element_link_many (self->tee, self->encoder_bin, self->file_sink_bin, NULL))
        g_critical ("Couldn't link filesink pipeline!");

    gst_element_sync_state_with_parent (self->encoder_bin);
    gst_element_sync_state_with_parent (self->file_sink_bin);
}

static gboolean
sync_state_change (GstElement *element,
                   GstState    state,
                   char       *element_name)
{
    switch (gst_element_set_state (element, state))
    {
        case GST_STATE_CHANGE_SUCCESS:
        {
            GST_INFO ("%s state succesfully changed to playing!", element_name);
            return TRUE;
        }
        case GST_STATE_CHANGE_ASYNC:
        {
            GST_INFO ("The state change of %s will happen asynchronously, waiting for state change.", element_name);
            GstState current_state, pending;
            GstStateChangeReturn res = gst_element_get_state (element, &current_state, &pending, GST_CLOCK_TIME_NONE);
            if (res == GST_STATE_CHANGE_SUCCESS && state == current_state)
            {
                g_message ("state change successful!");
                return TRUE;
            }
            return FALSE;
        }
        case GST_STATE_CHANGE_FAILURE:
        {
            GST_INFO ("Failed to change state of %s!!", element_name);
            return FALSE;
        }
        case GST_STATE_CHANGE_NO_PREROLL:
        {
            GST_INFO ("State change of %s succeeded, but element cannot produce data while paused", element_name);
            return FALSE;
        }
        default:
            return FALSE;
    }
}

static gboolean
async_destroy_encoder_bin (gpointer user_data)
{
    MatiDetector *self = MATI_DETECTOR (user_data);
    g_autoptr (GstIterator) pad_iter = NULL;

    sync_state_change (self->encoder_bin, GST_STATE_NULL, "encoder bin");
    gst_element_unlink (self->tee, self->encoder_bin);
    pad_iter = gst_element_iterate_src_pads (self->tee);
    GValue item = G_VALUE_INIT;
    gboolean done = FALSE;
    while (!done) {
        switch (gst_iterator_next (pad_iter, &item)) {
        case GST_ITERATOR_OK:
            GstPad *src_pad = g_value_get_object (&item);
            if (!gst_pad_is_linked (src_pad))
                gst_element_remove_pad (self->tee, src_pad);
            g_value_reset (&item);
            break;
        case GST_ITERATOR_RESYNC:
            g_warning ("iterator resync");
            gst_iterator_resync (pad_iter);
            break;
        case GST_ITERATOR_ERROR:
            g_warning ("iterator error");
            done = TRUE;
            break;
        case GST_ITERATOR_DONE:
            done = TRUE;
            break;
        }
    }
    g_value_unset (&item);
    if (!gst_bin_remove (GST_BIN (self->pipeline), self->encoder_bin))
        g_critical ("Couldn't remove encoder bin from pipeline!");
    self->encoder_bin = NULL;
    return FALSE;
}

static void
on_mux_queue_blocked (GstPad          *pad,
                      GstPadProbeInfo *info,
                      gpointer         user_data)
{
    MatiDetector *self = MATI_DETECTOR (user_data);

    if (gst_pad_is_blocked (pad))
    {
        g_autoptr (GstIterator) pad_iter = gst_element_iterate_sink_pads (self->mux);
        GValue item = G_VALUE_INIT;
        gboolean done = FALSE;
        while (!done) {
            switch (gst_iterator_next (pad_iter, &item)) {
            case GST_ITERATOR_OK:
                GstPad *sink_pad = g_value_get_object (&item);
                GstEvent *eos_event = gst_event_new_eos ();
                gboolean handled = gst_pad_send_event (sink_pad, eos_event);
                g_value_reset (&item);
                break;
            case GST_ITERATOR_RESYNC:
                g_warning ("iterator resync");
                gst_iterator_resync (pad_iter);
                break;
            case GST_ITERATOR_ERROR:
                g_warning ("iterator error");
                done = TRUE;
                break;
            case GST_ITERATOR_DONE:
                done = TRUE;
                break;
            }
        }
        g_value_unset (&item);
        sync_state_change (self->file_sink_bin, GST_STATE_NULL, "filesink bin");
        gst_element_unlink (self->encoder_bin, self->file_sink_bin);
        if (!gst_bin_remove (GST_BIN (self->pipeline), self->file_sink_bin))
            g_critical ("Couldn't remove filesink bin from pipeline!");
        self->file_sink_bin = NULL;
        g_idle_add (async_destroy_encoder_bin, self);

        return TRUE;
    }
}

static gboolean
mati_detector_destroy_filesink_pipeline (MatiDetector *self)
{
    g_message ("Destroying filesink pipeline");
    g_autoptr (GstPad) pad = gst_element_get_static_pad (self->mux_queue, "src");
    self->block_pad_id = gst_pad_add_probe (pad,
                                            GST_PAD_PROBE_TYPE_BLOCK_DOWNSTREAM,
                                            on_mux_queue_blocked,
                                            self, NULL);
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
                g_message ("motion %s!", self->is_in_motion ? "stopped" : "started");
                self->is_in_motion = !self->is_in_motion;

                mati_communicator_emit_motion_event (self->communicator, self->is_in_motion);

                if (self->is_in_motion)
                    mati_detector_setup_filesink_pipeline (self);
                else
                    mati_detector_destroy_filesink_pipeline (self);
            }
        }
        default:
            break;
    }
    return TRUE;
}

static void
mati_detector_activate_tcp_client (GObject *obj,
                                   int      port,
                                   gpointer user_data)
{
    MatiDetector *self = MATI_DETECTOR (user_data);
    GstElement *encoder_bin;
    GstElement *tcp_bin;
    GstElement *complete_bin;
    g_autofree char *bin_name = NULL;
    GstGhostPad *video_sink_pad;

    g_message ("Adding tcpclient with port %d to the pipeline!", port);

    encoder_bin = build_encoder_pipeline (self);
    tcp_bin = build_tcpsink (port);
    bin_name = g_strdup_printf ("%s%d", TCP_BIN_SUBNAME, port);
    complete_bin = GST_ELEMENT (gst_bin_new (bin_name));
    gst_bin_add_many (GST_BIN (complete_bin), encoder_bin, tcp_bin, NULL);

    g_autoptr (GstIterator) pad_iter = gst_element_iterate_sink_pads (encoder_bin);
    GValue item = G_VALUE_INIT;
    gboolean done = FALSE;
    while (!done) {
        switch (gst_iterator_next (pad_iter, &item)) {
        case GST_ITERATOR_OK:
            GstPad *sink_pad = g_value_get_object (&item);
            video_sink_pad = gst_ghost_pad_new ("sink", sink_pad);
            if (!gst_element_add_pad (complete_bin, video_sink_pad))
                g_critical ("Failed to set videosink pad in complete tcp bin!");
            g_value_reset (&item);
            break;
        case GST_ITERATOR_RESYNC:
            g_warning ("iterator resync");
            gst_iterator_resync (pad_iter);
            break;
        case GST_ITERATOR_ERROR:
            g_warning ("iterator error");
            done = TRUE;
            break;
        case GST_ITERATOR_DONE:
            done = TRUE;
            break;
        }
    }
    g_value_unset (&item);

    if (!gst_element_link (encoder_bin, tcp_bin))
        g_critical ("Couldn't link encoder bin to tcp bin!");
    gst_bin_add (GST_BIN (self->pipeline), complete_bin);
    if (!gst_element_link (self->tee, complete_bin))
        g_critical ("Couldn't link common pipeline to complete tcp bin!");

    gst_element_sync_state_with_parent (complete_bin);
}

static void
mati_detector_deactivate_tcp_client (GObject *obj,
                                     int port,
                                     gpointer user_data)
{
    MatiDetector *self = MATI_DETECTOR (user_data);
    GstElement *complete_bin;
    g_autofree char *bin_name = NULL;

    g_message ("Removing tcp client with port %d from the pipeline!", port);

    bin_name = g_strdup_printf ("%s%d", TCP_BIN_SUBNAME, port);
    complete_bin = gst_bin_get_by_name (GST_BIN (self->pipeline), bin_name);

    sync_state_change (complete_bin, GST_STATE_NULL, "complete tcp bin");
    gst_element_unlink (self->tee, complete_bin);
    if (!gst_bin_remove (GST_BIN (self->pipeline), complete_bin))
        g_critical ("Couldn't remove complete tcp bin from pipeline!");
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

    g_signal_connect_object (self->communicator, "activate-tcp-client", G_CALLBACK (mati_detector_activate_tcp_client), self, 0);
    g_signal_connect_object (self->communicator, "deactivate-tcp-client", G_CALLBACK (mati_detector_deactivate_tcp_client), self, 0);

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
    g_message ("using port %d", tcp_port);
    GstElement *bin, *queue_streamer, *streamer_clientsink;
    GstGhostPad *video_sink_pad;

    queue_streamer = gst_element_factory_make ("queue", NULL);
    g_return_val_if_fail (GST_IS_ELEMENT (queue_streamer), FALSE);

    streamer_clientsink = gst_element_factory_make ("tcpclientsink", NULL);
    g_return_val_if_fail (GST_IS_ELEMENT (streamer_clientsink), FALSE);
    g_object_set (G_OBJECT (streamer_clientsink), "host", "localhost", "port", tcp_port, NULL);
    gst_element_set_name (streamer_clientsink, TCP_CLIENT_NAME);

    bin = gst_bin_new (TCP_SUBBIN_NAME);
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
    g_autoptr (GTimeZone) time_zone = NULL;
    g_autoptr (GDateTime) date_time = NULL;
    g_autofree char *date_time_str = NULL;
    g_autofree char *file_name = NULL;

    queue_detector = gst_element_factory_make ("queue", NULL);
    g_return_val_if_fail (GST_IS_ELEMENT (queue_detector), FALSE);

    writer_detector = gst_element_factory_make ("filesink", NULL);
    g_return_val_if_fail (GST_IS_ELEMENT (writer_detector), FALSE);
    time_zone = g_time_zone_new_local ();
    date_time = g_date_time_new_now (time_zone);
    date_time_str =  g_date_time_format (date_time, "%H-%M-%S---%d-%m-%Y");
    file_name = g_strconcat ("/tmp/", date_time_str, "-test.ogg", NULL);
    g_message ("saving to %s", file_name);
    g_object_set (G_OBJECT (writer_detector), "location", file_name, NULL);
    gst_element_set_name (writer_detector, FILESINK_NAME);

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
build_encoder_pipeline (MatiDetector *self)
{
    GstElement *bin;
    GstElement *videoconvert;
    GstElement *streamer_encoder;

    GstGhostPad *video_sink_pad;
    GstGhostPad *video_src_pad;

    self->encoder_queue = gst_element_factory_make ("queue", NULL);
    g_return_val_if_fail (GST_IS_ELEMENT (self->encoder_queue), FALSE);

    videoconvert = gst_element_factory_make ("videoconvert", NULL);
    g_return_val_if_fail (GST_IS_ELEMENT (videoconvert), FALSE);

    streamer_encoder = gst_element_factory_make ("theoraenc", NULL);
    g_return_val_if_fail (GST_IS_ELEMENT (streamer_encoder), FALSE);
    gst_element_set_name (streamer_encoder, ENCODER_ELEMENT_NAME);

    self->mux_queue = gst_element_factory_make ("queue", NULL);
    g_return_val_if_fail (GST_IS_ELEMENT (self->mux_queue), FALSE);

    self->mux = gst_element_factory_make ("oggmux", NULL);
    g_return_val_if_fail (GST_IS_ELEMENT (self->mux), FALSE);

    bin = gst_bin_new ("encoderbin");
    gst_bin_add_many (GST_BIN (bin),
                      self->encoder_queue, videoconvert, streamer_encoder,
                      self->mux_queue, self->mux, NULL);

    if (!gst_element_link_many (self->encoder_queue, videoconvert, streamer_encoder,
                                self->mux_queue, self->mux, NULL))
        g_critical ("Couldn't link all encoder elements!");

    video_sink_pad = gst_ghost_pad_new ("videosink", gst_element_get_static_pad (self->encoder_queue, "sink"));
    if (!gst_element_add_pad (bin, video_sink_pad))
        g_critical ("Failed to set videosink pad in encoder bin!");
    video_src_pad = gst_ghost_pad_new ("videosrc", gst_element_get_static_pad (self->mux, "src"));
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
    gst_element_set_name (videosource, DECODEBIN_NAME);
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
    GstElement *fake_sink_bin;

    common_pipeline = build_common_pipeline (self, uri);
    self->tee = gst_element_factory_make ("tee", NULL);
    g_return_val_if_fail (GST_IS_ELEMENT (self->tee), FALSE);
    fake_sink_bin = build_fakesink ();

    gst_bin_add_many (GST_BIN (self->pipeline), common_pipeline, self->tee, fake_sink_bin, NULL);
    g_message ("about to dump dot");
    dump_dot (self);

    if (!gst_element_link_many (common_pipeline, self->tee, fake_sink_bin, NULL))
    {
        g_critical ("Couldn't link common pipeline to fakesinkbin!");
        return FALSE;
    }

    return TRUE;
}

JsonNode *
mati_detector_get_diagnostics (MatiDetector *self)
{
    JsonNode *json_node = json_node_alloc ();
    JsonObject *diagnostics_object = json_object_new ();
    JsonObject *decoder_object = json_object_new ();
    JsonArray *tcp_bins_active = json_array_new ();
    g_autoptr (GstElement) decodebin = gst_bin_get_by_name (self->pipeline, DECODEBIN_NAME);
    int tcp_bins = 0;
    gint64 bufferduration, connectionspeed, ringbuffermaxsize;
    int buffersize;
    g_autofree char *uri;
    gboolean forceswdecoders, usebuffering;

    g_autoptr (GstIterator) iter = gst_bin_iterate_elements (GST_BIN (self->pipeline));
    GValue item = G_VALUE_INIT;
    gboolean done = FALSE;
    while (!done) {
        switch (gst_iterator_next (iter, &item)) {
        case GST_ITERATOR_OK:
            GstElement *item_element = g_value_get_object (&item);
            if (GST_IS_BIN (item_element))
            {
                GstBin *item_bin = GST_BIN (item_element);
                g_autofree char *name = gst_element_get_name (item_element);
                g_autofree char *sub_name = g_strndup (name, TCP_BIN_SUBNAME_LENGTH);
                tcp_bins += 1;

                if (g_strcmp0 (sub_name, TCP_BIN_SUBNAME) == 0)
                {
                    g_autoptr (GstElement) tcpclient = gst_bin_get_by_name (item_bin, TCP_CLIENT_NAME);
                    g_autoptr (GstElement) encoder = gst_bin_get_by_name (item_bin, ENCODER_ELEMENT_NAME);
                    int port, bitrate, quality, ratebuffer, speedlevel;
                    g_autofree char *host;
                    gboolean dropframes;
                    JsonObject* json_object = json_object_new ();

                    g_object_get (tcpclient,
                                  "port", &port,
                                  "host", &host, NULL);
                    g_object_get (encoder,
                                  "bitrate", &bitrate,
                                  "drop-frames", &dropframes,
                                  "quality", &quality,
                                  "rate-buffer", &ratebuffer,
                                  "speed-level", &speedlevel, NULL);
                    json_object_set_int_member (json_object, "port", port);
                    json_object_set_int_member (json_object, "quality", quality);
                    json_object_set_int_member (json_object, "ratebuffer", ratebuffer);
                    json_object_set_int_member (json_object, "bitrate", bitrate);
                    json_object_set_int_member (json_object, "speedlevel", speedlevel);
                    json_object_set_string_member (json_object, "host", host);
                    json_object_set_boolean_member (json_object, "drop-frames", dropframes);
                    json_array_add_object_element (tcp_bins_active, json_object);
                }
            }
            g_value_reset (&item);
            break;
        case GST_ITERATOR_RESYNC:
            g_warning ("iterator resync");
            gst_iterator_resync (iter);
            break;
        case GST_ITERATOR_ERROR:
            g_warning ("iterator error");
            done = TRUE;
            break;
        case GST_ITERATOR_DONE:
            done = TRUE;
            break;
        }
    }
    g_value_unset (&item);

    g_object_get (decodebin,
                  "buffer-duration", &bufferduration,
                  "buffer-size", &buffersize,
                  "connection-speed", &connectionspeed,
                  "force-sw-decoders", &forceswdecoders,
                  "ring-buffer-max-size", &ringbuffermaxsize,
                  "uri", &uri, 
                  "use-buffering", &usebuffering, NULL);
    json_object_set_int_member (decoder_object, "buffer-duration", bufferduration);
    json_object_set_int_member (decoder_object, "buffer-size", buffersize);
    json_object_set_int_member (decoder_object, "connection-speed", connectionspeed);
    json_object_set_int_member (decoder_object, "ring-buffer-max-size", ringbuffermaxsize);
    json_object_set_string_member (decoder_object, "uri", uri);
    json_object_set_boolean_member (decoder_object, "use-buffering", usebuffering);
    json_object_set_boolean_member (decoder_object, "force-sw-decoders", forceswdecoders);

    json_object_set_boolean_member (diagnostics_object, "is-in-motion", self->is_in_motion);
    json_object_set_object_member (diagnostics_object, "decoder", decoder_object);
    json_object_set_array_member (diagnostics_object, "active-tcp-bins", tcp_bins_active);

    if (self->is_in_motion)
    {
        JsonObject *filesink_object = json_object_new ();
        g_autoptr (GstElement) encoder = gst_bin_get_by_name (self->encoder_bin, ENCODER_ELEMENT_NAME);
        g_autoptr (GstElement) filesink = gst_bin_get_by_name (self->file_sink_bin, FILESINK_NAME);
        int bitrate, quality, ratebuffer, speedlevel, filesink_buffersize;
        g_autofree *filelocation;
        gboolean dropframes;
        JsonObject* json_object = json_object_new ();

        g_object_get (encoder,
                      "bitrate", &bitrate,
                      "drop-frames", &dropframes,
                      "quality", &quality,
                      "rate-buffer", &ratebuffer,
                      "speed-level", &speedlevel, NULL);

        g_object_get (filesink,
                      "buffer-size", &filesink_buffersize,
                      "location", &filelocation, NULL);

        json_object_set_int_member (filesink_object, "quality", quality);
        json_object_set_int_member (filesink_object, "ratebuffer", ratebuffer);
        json_object_set_int_member (filesink_object, "bitrate", bitrate);
        json_object_set_int_member (filesink_object, "speedlevel", speedlevel);
        json_object_set_boolean_member (filesink_object, "drop-frames", dropframes);
        json_object_set_int_member (filesink_object, "filesink-buffer-size", filesink_buffersize);
        json_object_set_string_member (filesink_object, "file-location", filelocation);

        json_object_set_object_member (diagnostics_object, "active-file-bin", filesink_object);
    }

    return json_node_init_object (json_node, diagnostics_object);
}
