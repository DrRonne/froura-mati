#pragma once

#include <glib.h>
#include <glib-object.h>

#include "mati-application.h"
#include "froura-mati-dbus-proxy.h"

G_BEGIN_DECLS

enum MatiState
{
    MATI_STATE_STOPPED,
    MATI_STATE_PENDING,
    MATI_STATE_PAUSED,
    MATI_STATE_PLAYING
};

#define MATI_TYPE_COMMUNICATOR (mati_communicator_get_type ())
G_DECLARE_FINAL_TYPE (MatiCommunicator, mati_communicator, MATI, COMMUNICATOR, MatiDbusSkeleton);

void
mati_communicator_emit_motion_event (MatiCommunicator *self,
                                     gboolean          moving);

void
mati_communicator_emit_state_changed (MatiCommunicator *self,
                                      enum MatiState    state);

void
mati_communicator_emit_peer_id (MatiCommunicator *self,
                                char             *peer_id);

MatiCommunicator *
mati_communicator_new (const char      *mati_id,
                       MatiApplication *app);

G_END_DECLS