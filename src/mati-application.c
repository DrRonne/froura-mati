#include "mati-application.h"
#include "mati-communicator.h"

#include "mati-detector.h"
#include <gst/gst.h>

struct _MatiApplication
{
    GApplication parent_instance;

    MatiDetector *detector;
    MatiCommunicator *communicator;
};

G_DEFINE_TYPE (MatiApplication, mati_application, G_TYPE_APPLICATION);


static void
mati_application_init (MatiApplication *self)
{

}

static void
mati_application_finalize (GObject *object)
{
    MatiApplication *self = MATI_APPLICATION (object);

    // g_clear_object (self->pipeline);

    G_OBJECT_CLASS (mati_application_parent_class)->finalize (object);
}

static void
mati_application_activate (GApplication *app)
{
    MatiApplication *self = MATI_APPLICATION (app);

    self->communicator = mati_communicator_new ("test_app");

    self->detector = mati_detector_new (self->communicator);
    if (self->detector == NULL)
        return;
    
    if (!mati_detector_build (self->detector))
    {
        g_critical ("Could not build detector pipeline!");
        return;
    }

    mati_detector_start (self->detector);

    g_application_hold (app);

    g_message ("Mati is running");
}

static void
mati_application_shutdown (GApplication *app)
{
    g_message ("shutting down Mati...");
    g_application_release (app);
    G_APPLICATION_CLASS (mati_application_parent_class)->shutdown (app);
}

static void
mati_application_class_init (MatiApplicationClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS (klass);
    GApplicationClass *gapplication_class = G_APPLICATION_CLASS (klass);

    object_class->finalize = mati_application_finalize;

    gapplication_class->activate = mati_application_activate;
    gapplication_class->shutdown = mati_application_shutdown;
}

MatiApplication *
mati_application_new (int argc, char *argv[])
{
    g_autoptr (MatiApplication) self = NULL;
    g_autoptr (GError) error = NULL;

    self = g_object_new (MATI_TYPE_APPLICATION, "application-id", NULL, "flags", G_APPLICATION_NON_UNIQUE, NULL);

    return g_steal_pointer (&self);
}