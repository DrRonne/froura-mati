#include "mati-options.h"

struct _MatiOptions
{
    GObject parent_instance;

    gchar *id;
    gchar *uri;
    gboolean clockoverlay;
};

G_DEFINE_TYPE (MatiOptions, mati_options, G_TYPE_OBJECT);


static void
mati_options_init (MatiOptions *self)
{

}

static void
mati_options_finalize (GObject *object)
{
    MatiOptions *self = MATI_OPTIONS (object);

    G_OBJECT_CLASS (mati_options_parent_class)->finalize (object);
}

static void
mati_options_class_init (MatiOptionsClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS (klass);

    object_class->finalize = mati_options_finalize;
}

gboolean
mati_options_read (MatiOptions *self, int *argc, char **argv[], GError **err)
{
    g_autoptr (GOptionContext) ctx = NULL;
    GOptionGroup *main_group;
    GError *error = NULL;

    GOptionEntry entries[] = {
        {
            "uri", 0, 0, G_OPTION_ARG_STRING, &self->uri, "Input URI of the video stream", "rtsp://username:password@ipaddress/path"
        },
        {
            "id", 0, 0, G_OPTION_ARG_STRING, &self->id, "ID of the stream", "camera_livingroom"
        },
        {
            "clockoverlay", 0, 0, G_OPTION_ARG_NONE, &self->clockoverlay, "This flag will add a clockoverlay to the recorded video", NULL
        },
        { NULL }
    };

    main_group = g_option_group_new (NULL, NULL, NULL, self, NULL);
    g_option_group_add_entries (main_group, entries);

    ctx = g_option_context_new ("- Mati");
    g_option_context_set_main_group (ctx, main_group);
    g_option_context_add_group (ctx, gst_init_get_option_group());

    if (!g_option_context_parse (ctx, argc, argv, &error))
    {
        g_propagate_error (err, error);
        return FALSE;
    }
    return TRUE;
}

gchar *
mati_options_get_uri (MatiOptions *self)
{
    return self->uri;
}

gchar *
mati_options_get_id (MatiOptions *self)
{
    return self->id;
}

gboolean
mati_options_get_clockoverlay (MatiOptions *self)
{
    return self->clockoverlay;
}

MatiOptions *
mati_options_new ()
{
    MatiOptions *self = g_object_new (MATI_TYPE_OPTIONS, NULL);

    self->id = NULL;
    self->uri = NULL;
    self->clockoverlay = FALSE;
}