#pragma once

#include <glib.h>
#include <gio/gio.h>
#include <gst/gst.h>

G_BEGIN_DECLS

#define MATI_TYPE_DETECTOR (mati_detector_get_type ())
G_DECLARE_FINAL_TYPE (MatiDetector, mati_detector, MATI, DETECTOR, GObject)

MatiDetector* mati_detector_new ();

void mati_detector_start (MatiDetector *self);

GstStateChangeReturn mati_detector_stop (MatiDetector *self);

gboolean mati_detector_build (MatiDetector *self);

G_END_DECLS