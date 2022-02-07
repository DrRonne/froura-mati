#pragma once

#include <glib.h>
#include <gio/gio.h>
#include <gst/gst.h>

G_BEGIN_DECLS

#define MATI_TYPE_UPLOADER (mati_uploader_get_type ())
G_DECLARE_FINAL_TYPE (MatiUploader, mati_uploader, MATI, UPLOADER, GObject)

MatiUploader* mati_uploader_new ();

void mati_uploader_start (MatiUploader *self);

GstStateChangeReturn mati_uploader_stop (MatiUploader *self);

gboolean mati_uploader_build (MatiUploader *self);

G_END_DECLS