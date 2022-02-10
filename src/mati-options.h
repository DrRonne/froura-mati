#pragma once

#include <gst/gst.h>

G_BEGIN_DECLS

#define MATI_TYPE_OPTIONS (mati_options_get_type ())
G_DECLARE_FINAL_TYPE (MatiOptions, mati_options, MATI, OPTIONS, GObject)

MatiOptions *mati_options_new ();

gboolean mati_options_read (MatiOptions *self, int *argc, char **argv[], GError **error);

gchar *mati_options_get_uri (MatiOptions *self);
gchar *mati_options_get_id (MatiOptions *self);
gboolean mati_options_get_clockoverlay (MatiOptions *self);

G_END_DECLS