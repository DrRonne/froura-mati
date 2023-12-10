#pragma once

#include <glib.h>
#include <glib-object.h>

#include "froura-mati-dbus-proxy.h"

G_BEGIN_DECLS

#define MATI_TYPE_COMMUNICATOR (mati_communicator_get_type ())
G_DECLARE_FINAL_TYPE (MatiCommunicator, mati_communicator, MATI, COMMUNICATOR, MatiDbusSkeleton);

void
mati_communicator_emit_motion_event (MatiCommunicator *self,
                                     gboolean          moving);

MatiCommunicator *
mati_communicator_new (const char *mati_id);

G_END_DECLS