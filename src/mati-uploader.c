#include "mati-uploader.h"

GST_DEBUG_CATEGORY_STATIC (mati_uploader_debug);

struct _MatiUploader
{
    GObject parent_instance;

    gboolean is_in_motion;

    GstElement *pipeline;

    GstElement *convert;
    GstElement *motion;
};

G_DEFINE_TYPE (MatiUploader, mati_uploader, G_TYPE_OBJECT);

enum MatiUploaderSignals
{
    EOS_MESSAGE,
    PIPELINE_ERROR,
    PENDING,
    PLAYING,
    LAST
};

static guint signals[LAST];

GstStateChangeReturn mati_uploader_stop (MatiUploader *self);

static void
mati_uploader_init (MatiUploader *self)
{
    self->is_in_motion = FALSE;
}

static void
mati_uploader_finalize (GObject *object)
{
    MatiUploader *self = MATI_UPLOADER (object);

    g_return_if_fail (GST_IS_ELEMENT (self->pipeline));

    if (mati_uploader_stop (self) != GST_STATE_CHANGE_SUCCESS)
    {
        g_critical ("Couldn't stop the pipeline, exiting...");
        return;
    }
    else
    {
        GST_DEBUG ("Deleting pipeline");
        gst_clear_object (&self->pipeline);
    }

    G_OBJECT_CLASS (mati_uploader_parent_class)->finalize (object);
}

static void
mati_uploader_class_init (MatiUploaderClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS (klass);

    object_class->finalize = mati_uploader_finalize;

    signals[EOS_MESSAGE] = g_signal_new ("eos", MATI_TYPE_UPLOADER, G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL, G_TYPE_NONE, 0);
    signals[PIPELINE_ERROR] = g_signal_new ("eos", MATI_TYPE_UPLOADER, G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL, G_TYPE_NONE, 2, GST_TYPE_ELEMENT, G_TYPE_STRING);
    signals[PENDING] = g_signal_new ("eos", MATI_TYPE_UPLOADER, G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL, G_TYPE_NONE, 0);
    signals[PLAYING] = g_signal_new ("eos", MATI_TYPE_UPLOADER, G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL, G_TYPE_NONE, 0);
}

static gboolean
on_pipeline_message (GstBus *bus, GstMessage *message, gpointer user_data)
{
    MatiUploader *self = MATI_UPLOADER (user_data);

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
                self->is_in_motion = !self->is_in_motion;
            }
        }
        default:
            break;
    }
    return TRUE;
}

MatiUploader *
mati_uploader_new ()
{
    g_autoptr (MatiUploader) self = g_object_new (MATI_TYPE_UPLOADER, NULL);
    g_autoptr (GstBus) pipeline_bus = NULL;

    self->pipeline = gst_pipeline_new ("mati");
    if (self->pipeline == NULL)
        return NULL;
    
    pipeline_bus = gst_element_get_bus (self->pipeline);
    gst_bus_add_watch (pipeline_bus, on_pipeline_message, self);

    return g_steal_pointer (&self);
}

void
mati_uploader_start (MatiUploader *self)
{
    gboolean change_success = FALSE;

    g_return_if_fail (MATI_IS_UPLOADER (self));
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
mati_uploader_stop (MatiUploader *self)
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
pad_added_handler (GstElement *src, GstPad *new_pad, MatiUploader *self)
{
    GstPad *sink_pad = gst_element_get_static_pad (self->convert, "sink");
    GstPadLinkReturn ret;
    const gchar *new_pad_type = NULL;

    g_message ("Received new pad '%s' from '%s':\n", GST_PAD_NAME (new_pad), GST_ELEMENT_NAME (src));

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
        g_critical ("Type is '%s' but link failed.\n", new_pad_type);
    } 
    else
    {
        g_message ("Link succeeded (type '%s').\n", new_pad_type);
    }

exit:
    gst_object_unref (sink_pad);
}

gboolean
mati_uploader_build (MatiUploader *self)
{
    g_return_val_if_fail (MATI_IS_UPLOADER (self), FALSE);

    GstElement *videosource = gst_element_factory_make ("uridecodebin", NULL);
    g_return_val_if_fail (GST_IS_ELEMENT (videosource), FALSE);
    g_object_set (G_OBJECT (videosource), "uri", "file:///mnt/c/Users/amest/videolol00.mov", NULL);
    g_signal_connect (videosource, "pad-added", G_CALLBACK (pad_added_handler), self);

    GstElement *videoconvert = gst_element_factory_make ("videoconvert", NULL);
    g_return_val_if_fail (GST_IS_ELEMENT (videoconvert), FALSE);
    self->convert = videoconvert;

    GstElement *motioncells = gst_element_factory_make ("motioncells", NULL);
    g_return_val_if_fail (GST_IS_ELEMENT (motioncells), FALSE);
    g_object_set (G_OBJECT (motioncells), "display", FALSE, NULL);
    self->motion = motioncells;

    GstElement *videoconvert2 = gst_element_factory_make ("videoconvert", NULL);
    g_return_val_if_fail (GST_IS_ELEMENT (videoconvert2), FALSE);

    GstElement *clockoverlay = gst_element_factory_make ("clockoverlay", NULL);
    g_return_val_if_fail (GST_IS_ELEMENT (clockoverlay), FALSE);

    GstElement *autovideosink = gst_element_factory_make ("autovideosink", NULL);
    g_return_val_if_fail (GST_IS_ELEMENT (autovideosink), FALSE);

    gst_bin_add_many (GST_BIN (self->pipeline), videosource, videoconvert, motioncells, videoconvert2, clockoverlay, autovideosink, NULL);

    if (!gst_element_link_many (videoconvert, motioncells, videoconvert2, clockoverlay, autovideosink, NULL))
    {
        g_critical ("Couldn't link all elements!");
        return FALSE;
    }

    return TRUE;
}