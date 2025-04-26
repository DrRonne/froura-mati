#include "mati-detector.h"
#include <gst/video/video.h>

#define TCP_BIN_SUBNAME "tcpbin_"
#define TCP_BIN_SUBNAME_LENGTH 7
#define TCP_CLIENT_NAME "tcpclientsink"
#define TCP_SUBBIN_NAME "tcpsinkbin"
#define ENCODER_ELEMENT_NAME "encoder"
#define RTSPSRC_NAME "rtspsource"
#define WEBRTCSINK_NAME "webrtcsink"
#define FILESINK_NAME "filesink"
#define RECORDING_BUFFER_NAME "recording-buffer"
#define DECODE_FRAME_TIMEOUT 10000
#define PAUSED 3
#define PLAYING 4
#define RECORDING_BUFFER ((guint64)10 * 1000000000) // 10 seconds in nanoseconds
#define THUMBNAIL_REFRESH_INTERVAL ((guint64)10 * 1000000000) // 10 seconds in nanoseconds

GST_DEBUG_CATEGORY_STATIC (mati_detector_debug);

struct _MatiDetector
{
    GObject parent_instance;

    MatiCommunicator *communicator;

    gboolean is_in_motion;

    GstElement *pipeline;

    GstElement *tee;
    GstElement *file_sink_bin;

    GstElement *queue_connect;
    GstElement *mux_queue;
    GstElement *mux;
    GstElement *motion;

    GstElement *recording_tee;

    gint64 last_frame_buffer;
    gdouble framerate;
    guint frame_timeout;

    gulong block_pad_id;

    char *peer_id;

    char *source_id;
    char *turnserver;

    guint motion_stopped_timeout;

    guint thumbnail_timeout;
};

G_DEFINE_TYPE (MatiDetector, mati_detector, G_TYPE_OBJECT);

enum MatiDetectorSignals
{
    EOS_MESSAGE,
    PIPELINE_ERROR,
    MATI_PENDING,
    MATI_PLAYING,
    LAST
};

static guint signals[LAST];

GstStateChangeReturn mati_detector_stop (MatiDetector *self);

static GstElement* build_filesink (MatiDetector *self);
// static gboolean decode_frame_timeout (MatiDetector *self);

static void
mati_detector_init (MatiDetector *self)
{
    self->is_in_motion = FALSE;
    self->file_sink_bin = NULL;
    self->framerate = 0;
    self->last_frame_buffer = 0;
    self->motion_stopped_timeout = 0;
    self->thumbnail_timeout = 0;
    // self->frame_timeout = g_timeout_add (DECODE_FRAME_TIMEOUT, decode_frame_timeout, self);
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

    g_free (self->peer_id);

    G_OBJECT_CLASS (mati_detector_parent_class)->finalize (object);
}

static void
mati_detector_class_init (MatiDetectorClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS (klass);

    object_class->finalize = mati_detector_finalize;

    signals[EOS_MESSAGE] = g_signal_new ("eos", MATI_TYPE_DETECTOR, G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL, G_TYPE_NONE, 0);
    signals[PIPELINE_ERROR] = g_signal_new ("error", MATI_TYPE_DETECTOR, G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL, G_TYPE_NONE, 2, GST_TYPE_ELEMENT, G_TYPE_STRING);
    signals[MATI_PENDING] = g_signal_new ("pending", MATI_TYPE_DETECTOR, G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL, G_TYPE_NONE, 0);
    signals[MATI_PLAYING] = g_signal_new ("playing", MATI_TYPE_DETECTOR, G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL, G_TYPE_NONE, 0);
}

static void
mati_detector_setup_filesink_pipeline (MatiDetector *self)
{
    g_message ("Setting up filesink pipeline");

    self->file_sink_bin = build_filesink (self);
    gst_bin_add (GST_BIN (self->pipeline), self->file_sink_bin);
    if (!gst_element_link (self->recording_tee, self->file_sink_bin))
        g_critical ("Couldn't link filesink pipeline!");

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
async_destroy_filesink (gpointer user_data)
{
    MatiDetector *self = MATI_DETECTOR (user_data);
    sync_state_change (self->file_sink_bin, GST_STATE_NULL, "filesink bin");
    gst_element_unlink (self->recording_tee, self->file_sink_bin);
    if (!gst_bin_remove (GST_BIN (self->pipeline), self->file_sink_bin))
        g_critical ("Couldn't remove filesink bin from pipeline!");
    self->file_sink_bin = NULL;
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
                gst_pad_send_event (sink_pad, eos_event);
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
        g_idle_add (async_destroy_filesink, self);

        return TRUE;
    }
}

static gboolean
mati_detector_destroy_filesink_pipeline (gpointer user_data)
{
    MatiDetector *self = MATI_DETECTOR (user_data);

    g_autoptr (GstPad) pad = gst_element_get_static_pad (self->mux_queue, "src");
    self->block_pad_id = gst_pad_add_probe (pad,
                                            GST_PAD_PROBE_TYPE_BLOCK_DOWNSTREAM,
                                            on_mux_queue_blocked,
                                            self, NULL);
    self->motion_stopped_timeout = 0;
    return G_SOURCE_REMOVE;
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
            if ((GObject *) GST_MESSAGE_SRC (message) == (GObject *) self->pipeline)
            {
                GstState old_state, new_state;

                gst_message_parse_state_changed (message, &old_state, &new_state, NULL);
                GST_INFO ("Pipeline went from state %s to state %s", gst_element_state_get_name (old_state), gst_element_state_get_name (new_state));
                switch (new_state)
                {
                    case PAUSED:
                    {
                        mati_communicator_emit_state_changed (self->communicator, MATI_STATE_PAUSED);
                        ;
                    }
                    case PLAYING:
                    {
                        mati_communicator_emit_state_changed (self->communicator, MATI_STATE_PLAYING);
                        break;
                    }
                    default:
                    {
                        mati_communicator_emit_state_changed (self->communicator, MATI_STATE_STOPPED);
                        break;
                    }
                }
                GST_DEBUG_BIN_TO_DOT_FILE (GST_BIN (self->pipeline), GST_DEBUG_GRAPH_SHOW_ALL, "/tmp/pipeline");
            }
            break;
        }
        case GST_MESSAGE_ELEMENT:
        {
            if ((GObject *) GST_MESSAGE_SRC (message) == (GObject *) self->motion)
            {
                g_message ("motion %s!", self->is_in_motion ? "stopped" : "started");
                self->is_in_motion = !self->is_in_motion;

                mati_communicator_emit_motion_event (self->communicator, self->is_in_motion);

                if (self->is_in_motion)
                {
                    if (self->motion_stopped_timeout != 0)
                    {
                        g_message ("Removing motion stopped timeout");
                        g_source_remove (self->motion_stopped_timeout);
                        self->motion_stopped_timeout = 0;
                    }
                    else
                    {
                        g_message ("setting new filesink");
                    mati_detector_setup_filesink_pipeline (self);
                    }
                }
                else
                {
                    if (self->motion_stopped_timeout != 0)
                    {
                        g_message ("Removing previous motion stopped timeout");
                        g_source_remove (self->motion_stopped_timeout);
                        self->motion_stopped_timeout = 0;
                    }
                    g_message ("setting new motion stopped timeout");
                    self->motion_stopped_timeout = g_timeout_add_seconds (RECORDING_BUFFER / 1000000000, (GSourceFunc) mati_detector_destroy_filesink_pipeline, self);
                }
            }
        }
        default:
            break;
    }
    return TRUE;
}

MatiDetector *
mati_detector_new (MatiCommunicator *communicator,
                   char             *source_id,
                   char             *turnserver)
{
    g_autoptr (MatiDetector) self = g_object_new (MATI_TYPE_DETECTOR, NULL);
    g_autoptr (GstBus) pipeline_bus = NULL;

    self->source_id = source_id;
    self->turnserver = turnserver;
    self->pipeline = gst_pipeline_new ("mati");
    if (self->pipeline == NULL)
        return NULL;
    
    pipeline_bus = gst_element_get_bus (self->pipeline);
    gst_bus_add_watch (pipeline_bus, on_pipeline_message, self);

    self->communicator = communicator;
    self->peer_id = g_strdup ("no peer id yet");

    return g_steal_pointer (&self);
}

void
mati_detector_start (MatiDetector *self)
{
    g_message ("emiting state %d", MATI_STATE_PENDING);
    mati_communicator_emit_state_changed (self->communicator, MATI_STATE_PENDING);
    gboolean change_success = FALSE;

    g_return_if_fail (MATI_IS_DETECTOR (self));
    g_return_if_fail (GST_IS_ELEMENT (self->pipeline));

    GST_INFO ("Setting pipeline to playing state...");

    switch (gst_element_set_state (self->pipeline, GST_STATE_PLAYING))
    {
        case GST_STATE_CHANGE_SUCCESS:
        {
            mati_communicator_emit_state_changed (self->communicator, MATI_STATE_PLAYING);
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
        g_signal_emit (self, signals[MATI_PENDING], 0);
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
    return TRUE;
}

static gboolean
decode_frame_timeout (MatiDetector *self)
{
    g_critical ("Frame timeout reached!");
    mati_communicator_emit_state_changed (self->communicator, MATI_STATE_STOPPED);
    return FALSE;
}

static GstPadProbeReturn
decode_frame_probe_cb (GstPad          *pad,
                       GstPadProbeInfo *info,
                       gpointer         user_data)
{
    MatiDetector *self = MATI_DETECTOR (user_data);

    gint64 previous_frame_buffer = self->last_frame_buffer;
    self->last_frame_buffer = g_get_monotonic_time ();
    self->framerate = 1 / ((double) (self->last_frame_buffer - previous_frame_buffer) / 1000000);

    if (self->frame_timeout > 0)
    g_source_remove (self->frame_timeout);
    self->frame_timeout = g_timeout_add (DECODE_FRAME_TIMEOUT, decode_frame_timeout, self);

    return GST_PAD_PROBE_OK;
}

static void
pad_added_handler (GstElement *src, GstPad *new_pad, MatiDetector *self)
{
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
        GST_DEBUG_BIN_TO_DOT_FILE(GST_BIN (self->pipeline), GST_DEBUG_GRAPH_SHOW_ALL, "/tmp/dotfile");
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
peer_id_handler (GstElement   *signaller,
                 gchararray    peer_id,
                 MatiDetector *self)
{
    mati_communicator_emit_peer_id (self->communicator, peer_id);
    g_free (self->peer_id);
    self->peer_id = g_strdup (peer_id);
}

static void
consumer_added_handler (GstElement *consumer_id, char *webrtcbin, GstElement *arg1, MatiDetector *self)
{
    gboolean ret;
    char *ts;
    g_object_get (arg1, "turn-server", &ts, NULL);
    g_signal_emit_by_name (arg1, "add-turn-server", self->turnserver, &ret);
}


static GstElement*
build_streamer (MatiDetector *self)
{
    GstElement *bin, *streamer_queue, *webrtcsink;
    GstPad *video_sink_pad;

    streamer_queue = gst_element_factory_make ("queue", NULL);
    g_return_val_if_fail (GST_IS_ELEMENT (streamer_queue), FALSE);

    webrtcsink = gst_element_factory_make ("webrtcsink", NULL);
    g_return_val_if_fail (GST_IS_ELEMENT (webrtcsink), FALSE);
    GObject *signaller;
    GstValueArray *turnservers;
    GValue turnserver_array = G_VALUE_INIT;
    GValue deserialized_turnserver = G_VALUE_INIT;;
    char *uri;
    g_object_get (webrtcsink, "signaller", &signaller, "turn-servers", &turnservers, NULL);
    g_object_get (signaller, "uri", &uri, NULL);
    g_object_set (signaller, "uri", "ws://synthima-service:8443", NULL);
    g_object_get (signaller, "uri", &uri, NULL);
    g_signal_connect (signaller, "peer-id-ready", G_CALLBACK (peer_id_handler), self);
    g_signal_connect(webrtcsink, "consumer-added", G_CALLBACK (consumer_added_handler), self);
    g_value_init (&turnserver_array, GST_TYPE_ARRAY);
    g_value_init (&deserialized_turnserver, G_TYPE_STRING);
    gboolean ret = gst_value_deserialize (&deserialized_turnserver, self->turnserver);
    gst_value_array_append_value (&turnserver_array, &deserialized_turnserver);
    g_object_set_property(G_OBJECT(webrtcsink), "turn-servers", &turnserver_array);
    gst_element_set_name (webrtcsink, WEBRTCSINK_NAME);

    bin = gst_bin_new ("streamerbin");
    gst_bin_add_many (GST_BIN (bin), streamer_queue, webrtcsink, NULL);
    if (!gst_element_link (streamer_queue, webrtcsink))
        g_critical ("Failed to link streamer elements!");

    video_sink_pad = gst_ghost_pad_new ("videosink", gst_element_get_static_pad (streamer_queue, "sink"));
    if (!gst_element_add_pad (bin, video_sink_pad))
        g_critical ("Failed to set videosink pad in streamer bin!");

    return bin;
}

static GstPadProbeReturn
iframe_probe_cb (GstPad *pad, GstPadProbeInfo *info, gpointer user_data)
{
    GstBuffer *buffer = GST_PAD_PROBE_INFO_BUFFER (info);
    GstEvent *event = GST_PAD_PROBE_INFO_EVENT (info);

    if (buffer) {
        if (!(gst_buffer_get_flags (buffer) & GST_BUFFER_FLAG_DELTA_UNIT) && gst_buffer_get_flags (buffer) != 0) {
            g_message ("I-frame detected, starting recording");
            return GST_PAD_PROBE_REMOVE;
        } else {
            return GST_PAD_PROBE_DROP;
        }
    }

    if (event) {
        if (GST_EVENT_TYPE (event) == GST_EVENT_EOS) {
            g_message ("EOS event received, stopping probe");
            return GST_PAD_PROBE_REMOVE;
        }
    }

    return GST_PAD_PROBE_OK;
}

static GstElement*
build_filesink (MatiDetector *self)
{
    GstElement *bin, *queue_detector, *mux_detector, *writer_detector;
    GstPad *video_sink_pad;
    g_autoptr (GTimeZone) time_zone = NULL;
    g_autoptr (GDateTime) date_time = NULL;
    g_autofree char *date_time_str = NULL;
    g_autofree char *file_name = NULL;

    queue_detector = gst_element_factory_make ("queue", NULL);
    g_return_val_if_fail (GST_IS_ELEMENT (queue_detector), FALSE);
    self->mux_queue = queue_detector;

    mux_detector = gst_element_factory_make ("matroskamux", NULL);
    g_object_set (G_OBJECT (mux_detector), "offset-to-zero", TRUE, NULL);
    g_return_val_if_fail (GST_IS_ELEMENT (mux_detector), FALSE);
    self->mux = mux_detector;

    writer_detector = gst_element_factory_make ("filesink", NULL);
    g_return_val_if_fail (GST_IS_ELEMENT (writer_detector), FALSE);
    time_zone = g_time_zone_new_local ();
    date_time = g_date_time_new_now (time_zone);
    date_time_str =  g_date_time_format (date_time, "%H-%M-%S---%d-%m-%Y");
    file_name = g_strconcat ("/etc/videos/", self->source_id, "/", date_time_str, ".mp4", NULL);
    g_message ("saving to %s", file_name);
    g_object_set (G_OBJECT (writer_detector),
                  "location", file_name,
                  "sync", TRUE,
                  NULL);
    gst_element_set_name (writer_detector, FILESINK_NAME);

    /* Add probe that drops frames until a keyframe is received to ensure we
     * only have a useful, fully readable recording. */
    GstPad *mux_queue_src_pad = gst_element_get_static_pad (self->mux_queue, "src");
    gst_pad_add_probe (mux_queue_src_pad, GST_PAD_PROBE_TYPE_BUFFER | GST_PAD_PROBE_TYPE_EVENT_DOWNSTREAM, iframe_probe_cb, self, NULL);
    gst_object_unref (mux_queue_src_pad);

    bin = gst_bin_new ("filesinkbin");
    gst_bin_add_many (GST_BIN (bin), queue_detector, mux_detector, writer_detector, NULL);
    if (!gst_element_link_many (queue_detector, mux_detector, writer_detector, NULL))
        g_critical ("Failed to link filesink elements!");

    video_sink_pad = gst_ghost_pad_new ("videosink", gst_element_get_static_pad (queue_detector, "sink"));
    if (!gst_element_add_pad (bin, video_sink_pad))
        g_critical ("Failed to set videosink pad in filesink bin!");

    return bin;
}

static GstElement*
build_thumbnailsink (MatiDetector *self)
{
    GstElement *bin, *queue_fakesink, *decoder, *videoconvert, *motioncells, *videorate, *capsfilter, *jpegenc, *multifilesink;
    GstPad *video_sink_pad;
    g_autofree char *file_name = NULL;

    queue_fakesink = gst_element_factory_make ("queue", NULL);
    g_return_val_if_fail (GST_IS_ELEMENT (queue_fakesink), FALSE);

    jpegenc = gst_element_factory_make ("jpegenc", NULL);
    g_return_val_if_fail (GST_IS_ELEMENT (jpegenc), FALSE);

    multifilesink = gst_element_factory_make ("multifilesink", NULL);
    g_return_val_if_fail (GST_IS_ELEMENT (multifilesink), FALSE);
    file_name = g_strconcat ("/etc/thumbnails/", self->source_id, ".jpg", NULL);
    g_object_set (G_OBJECT (multifilesink),
                  "location", file_name,
                  "post-messages", TRUE,
                  "next-file", 0, // every buffer is a new image
                  "sync", FALSE,
                  NULL);

    decoder = gst_element_factory_make ("avdec_h264", NULL);
    g_return_val_if_fail (GST_IS_ELEMENT (decoder), FALSE);
    gst_pad_add_probe(gst_element_get_static_pad (decoder, "src"),
                      GST_PAD_PROBE_TYPE_BUFFER,
                      decode_frame_probe_cb,
                      self,
                      NULL);

    videoconvert = gst_element_factory_make ("videoconvert", NULL);
    g_return_val_if_fail (GST_IS_ELEMENT (videoconvert), FALSE);

    motioncells = gst_element_factory_make ("motioncells", NULL);
    g_return_val_if_fail (GST_IS_ELEMENT (motioncells), FALSE);
    g_object_set (G_OBJECT (motioncells), "display", FALSE, NULL);
    self->motion = motioncells;

    videorate = gst_element_factory_make ("videorate", NULL);
    g_return_val_if_fail (GST_IS_ELEMENT (videorate), FALSE);
    g_object_set (G_OBJECT (videorate),
                  "max-rate", 1,
                  "drop-only", TRUE,
                  "skip-to-first", TRUE,
                  NULL);

    capsfilter = gst_element_factory_make ("capsfilter", NULL);
    g_return_val_if_fail (GST_IS_ELEMENT (capsfilter), FALSE);
    GstCaps *caps = gst_caps_new_simple ("video/x-raw",
                                        "framerate", GST_TYPE_FRACTION, 1, 10,
                                        NULL);
    g_object_set (G_OBJECT (capsfilter), "caps", caps, NULL);
    gst_caps_unref (caps);

    bin = gst_bin_new ("thumbnailsinkbin");
    gst_bin_add_many (GST_BIN (bin), queue_fakesink, decoder, videoconvert, motioncells, videorate, capsfilter, jpegenc, multifilesink, NULL);
    if (!gst_element_link_many (queue_fakesink, decoder, videoconvert, motioncells, videorate, capsfilter, jpegenc, multifilesink, NULL))
        g_critical ("Failed to link thumbnailsink elements!");

    video_sink_pad = gst_ghost_pad_new ("videosink", gst_element_get_static_pad (queue_fakesink, "sink"));
    if (!gst_element_add_pad (bin, video_sink_pad))
        g_critical ("Failed to set videosink pad in fakesink bin!");

    return bin;
}

static GstElement*
build_common_pipeline (MatiDetector *self,
                       gchar        *uri)
{
    GstElement *bin;

    GstElement *videosource;
    GstElement *h264_depay;
    GstElement *h264_parse;

    GstPad *video_src_pad;

    videosource = gst_element_factory_make ("rtspsrc", NULL);
    g_return_val_if_fail (GST_IS_ELEMENT (videosource), FALSE);
    g_object_set (G_OBJECT (videosource), "location", uri, NULL);
    gst_element_set_name (videosource, RTSPSRC_NAME);
    g_signal_connect (videosource, "pad-added", G_CALLBACK (pad_added_handler), self);

    self->queue_connect = gst_element_factory_make ("queue", NULL);
    g_return_val_if_fail (GST_IS_ELEMENT (self->queue_connect), FALSE);

    h264_depay = gst_element_factory_make ("rtph264depay", NULL);
    g_return_val_if_fail (GST_IS_ELEMENT (h264_depay), FALSE);

    h264_parse = gst_element_factory_make ("h264parse", NULL);
    g_return_val_if_fail (GST_IS_ELEMENT (h264_parse), FALSE);

    bin = gst_bin_new ("commonbin");
    gst_bin_add_many (GST_BIN (bin), videosource, self->queue_connect,
                      h264_depay, h264_parse,
                      NULL);

    if (!gst_element_link_many (self->queue_connect, h264_depay, h264_parse,
                                NULL))
        g_critical ("Couldn't link all common pipeline elements!");

    video_src_pad = gst_ghost_pad_new ("videosrc", gst_element_get_static_pad (h264_parse, "src"));
    if (!gst_element_add_pad (bin, video_src_pad))
        g_critical ("Failed to set videosrc pad in common pipeline bin!");

    return bin;
}

gboolean
mati_detector_build (MatiDetector *self,
                     gchar        *uri)
{
    g_return_val_if_fail (MATI_IS_DETECTOR (self), FALSE);

    GstElement *common_pipeline;
    GstElement *thumbnail_sink_bin, *streamer_bin;
    GstElement *recording_buffer;
    GstElement *recording_fakesink_queue;
    GstElement *recording_fakesink;

    common_pipeline = build_common_pipeline (self, uri);
    streamer_bin = build_streamer (self);
    self->tee = gst_element_factory_make ("tee", NULL);
    g_return_val_if_fail (GST_IS_ELEMENT (self->tee), FALSE);
    self->recording_tee = gst_element_factory_make ("tee", NULL);
    g_return_val_if_fail (GST_IS_ELEMENT (self->recording_tee), FALSE);
    /* Add a buffer that is at least RECORDING_BUFFER long.
     * This makes sure that the recording pipeline is running RECORDING_BUFFER
     * behind so we can "record in the past". */
    recording_buffer = gst_element_factory_make ("queue", NULL);
    g_return_val_if_fail (GST_IS_ELEMENT (recording_buffer), FALSE);
    gst_element_set_name (recording_buffer, RECORDING_BUFFER_NAME);
    g_object_set (recording_buffer,
                  "max-size-time", RECORDING_BUFFER * 2,
                  "max-size-bytes", 0,
                  "max-size-buffers", 0,
                  NULL);
    GstPad *recording_buffer_src_pad = gst_element_get_static_pad (recording_buffer, "src");
    gst_pad_set_offset (recording_buffer_src_pad, RECORDING_BUFFER);
    gst_object_unref (recording_buffer_src_pad);
    recording_fakesink_queue = gst_element_factory_make ("queue", NULL);
    g_return_val_if_fail (GST_IS_ELEMENT (recording_fakesink_queue), FALSE);
    recording_fakesink = gst_element_factory_make ("fakesink", NULL);
    g_return_val_if_fail (GST_IS_ELEMENT (recording_fakesink), FALSE);
    g_object_set (G_OBJECT (recording_fakesink),
                  "sync", TRUE,
                  NULL);
    thumbnail_sink_bin = build_thumbnailsink (self);

    gst_bin_add_many (GST_BIN (self->pipeline), common_pipeline, self->tee, thumbnail_sink_bin, streamer_bin, 
                               recording_buffer, self->recording_tee, recording_fakesink_queue, recording_fakesink,
                               NULL);

    if (!gst_element_link_many (common_pipeline, self->tee, thumbnail_sink_bin, NULL))
    {
        g_critical ("Couldn't link common pipeline to fakesinkbin!");
        return FALSE;
    }

    if (!gst_element_link (self->tee, streamer_bin))
    {
        g_critical ("Couldn't link common pipeline to streamer bin!");
        return FALSE;
    }

    if (!gst_element_link_many (self->tee, recording_buffer, self->recording_tee, recording_fakesink_queue, recording_fakesink, NULL))
    {
        g_critical ("Couldn't link common pipeline to recording buffer!");
        return FALSE;
    }

    return TRUE;
}

JsonNode *
mati_detector_get_diagnostics (MatiDetector *self)
{
    JsonNode *json_node = json_node_alloc ();
    JsonObject *diagnostics_object = json_object_new ();
    JsonObject *input_object = json_object_new ();
    JsonObject *webrtc_object = json_object_new ();
    JsonObject *decoder_object = json_object_new ();
    g_autoptr (GstElement) rtspsrc = gst_bin_get_by_name (GST_BIN (self->pipeline), RTSPSRC_NAME);
    g_autoptr (GstElement) webrtcsink = gst_bin_get_by_name (GST_BIN (self->pipeline), WEBRTCSINK_NAME);

    if (!GST_IS_BIN (rtspsrc) || !GST_IS_BIN (webrtcsink))
    {
        g_critical ("Couldn't get rtspsrc or webrtcsink elements!");
        return json_node_init_object (json_node, diagnostics_object);
    }

    gint64 bufferduration, connectionspeed, latency;
    int buffersize;
    g_autofree char *uri;

    g_object_get (rtspsrc,
                  "udp-buffer-size", &buffersize,
                  "connection-speed", &connectionspeed,
                  "location", &uri, 
                  "latency", &latency, NULL);
    json_object_set_int_member (input_object, "udp-buffer-size", buffersize);
    json_object_set_int_member (input_object, "connection-speed", connectionspeed);
    json_object_set_string_member (input_object, "uri", uri);
    json_object_set_int_member (input_object, "latency", latency);

    json_object_set_boolean_member (diagnostics_object, "is-in-motion", self->is_in_motion);
    json_object_set_object_member (diagnostics_object, "input", input_object);

    GStrv *webrtc_sessions;
    guint min_bitrate, max_bitrate;
    char *stun_server;
    GstValueArray *turn_servers;
    // JsonArray *turn_server_array = json_array_new ();
    GstCaps *video_caps;
    g_signal_emit_by_name (webrtcsink, "get-sessions", &webrtc_sessions);
    g_object_get (webrtcsink,
                  "min-bitrate", &min_bitrate,
                  "max-bitrate", &max_bitrate,
                  "stun-server", &stun_server,
                  "turn-servers", &turn_servers,
                  "video-caps", &video_caps, NULL);
    json_object_set_int_member (webrtc_object, "min-bitrate", min_bitrate);
    json_object_set_int_member (webrtc_object, "max-bitrate", max_bitrate);
    json_object_set_string_member (webrtc_object, "stun-server", stun_server);
    // for (int i = 0; i < gst_value_array_get_size (turn_servers); i++)
    // {
    //     json_array_add_string_element (turn_server_array, g_value_get_string (gst_value_array_get_value (turn_servers, i)));
    // }
    // json_object_set_array_member (webrtcsink, "turn-servers", turn_server_array);
    json_object_set_string_member (webrtc_object, "video-caps", gst_caps_to_string (video_caps));
    json_object_set_int_member (webrtc_object, "consumers", g_strv_length (webrtc_sessions));
    json_object_set_string_member (webrtc_object, "peer-id", self->peer_id);
    json_object_set_object_member (diagnostics_object, "webrtc", webrtc_object);

    json_object_set_double_member (decoder_object, "framerate", self->framerate);
    json_object_set_int_member (decoder_object, "last-frame-buffer", self->last_frame_buffer);
    json_object_set_object_member (diagnostics_object, "decoder", decoder_object);

    if (self->is_in_motion)
    {
        JsonObject *filesink_object = json_object_new ();
        g_autoptr (GstElement) filesink = gst_bin_get_by_name (self->file_sink_bin, FILESINK_NAME);
        int filesink_buffersize;
        g_autofree *filelocation;
        gboolean dropframes;
        JsonObject* json_object = json_object_new ();

        g_object_get (filesink,
                      "buffer-size", &filesink_buffersize,
                      "location", &filelocation, NULL);

        json_object_set_int_member (filesink_object, "filesink-buffer-size", filesink_buffersize);
        json_object_set_string_member (filesink_object, "file-location", filelocation);

        json_object_set_object_member (diagnostics_object, "active-file-bin", filesink_object);
    }

    return json_node_init_object (json_node, diagnostics_object);
}
