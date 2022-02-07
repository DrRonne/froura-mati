#pragma once

#include <glib.h>
#include <gio/gio.h>

G_BEGIN_DECLS

#define MATI_TYPE_APPLICATION (mati_application_get_type ())

G_DECLARE_FINAL_TYPE (MatiApplication, mati_application, MATI, APPLICATION, GApplication);

MatiApplication* mati_application_new (int argc, char* argv[]);

G_END_DECLS