#include <gio/gio.h>
#include <gst/gst.h>
#include <stdlib.h>
#include "mati-application.h"

int
main (int argc, char *argv[])
{
    g_autoptr (MatiApplication) application = NULL;
    gint exit_status;

    // Init gstreamer
    gst_init (&argc, &argv);

    application = mati_application_new (argc, argv);
    if (application == NULL)
        return EXIT_FAILURE;
    
    exit_status = g_application_run (G_APPLICATION (application), argc, argv);

    g_message ("Mati exiting...");

    return exit_status;
}