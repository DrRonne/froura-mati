#include "mati-communicator.h"

struct _MatiCommunicator
{
   MatiDbusSkeleton parent_instance;

   guint dbus_owner_id;
   MatiApplication *app;
};

G_DEFINE_TYPE (MatiCommunicator, mati_communicator, MATI_DBUS_TYPE__SKELETON);

void
mati_communicator_emit_motion_event (MatiCommunicator *self,
                                     gboolean          moving)
{
    mati_dbus__emit_motion (MATI_DBUS_ (self), moving);
}

void
mati_communicator_emit_state_changed (MatiCommunicator *self,
                                      enum MatiState    state)
{
    mati_dbus__emit_state_changed (MATI_DBUS_ (self), state);
}

void
mati_communicator_emit_peer_id (MatiCommunicator *self,
                                char             *peer_id)
{
    mati_dbus__emit_peer_id (MATI_DBUS_ (self), peer_id);
}

static gboolean
handle_get_diagnostics (MatiDbus              *obj,
                        GDBusMethodInvocation *invoc,
                        gpointer               user_data)
{
    MatiCommunicator *self = MATI_COMMUNICATOR (user_data);
    g_autofree const char *diag_str = mati_application_get_diagnostics (self->app);

    mati_dbus__complete_get_diagnostics (obj, invoc, diag_str);
}

static void
mati_communicator_finalize (GObject *object)
{
    MatiCommunicator *self = MATI_COMMUNICATOR (object);

    g_bus_unown_name (self->dbus_owner_id);

    G_OBJECT_CLASS (mati_communicator_parent_class)->finalize (object);
}

static void
mati_communicator_init (MatiCommunicator *self)
{
    self->dbus_owner_id = 0;
    self->app = NULL;
}

static void
mati_communicator_class_init (MatiCommunicatorClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS (klass);

    object_class->finalize = mati_communicator_finalize;
    // object_class->constructed = mati_communicator_constructed;
}

static void
on_name_lost (GDBusConnection *connection, const char *name, gpointer user_data)
{
    if (connection)
        g_critical ("Couldn't acquire DBus name %s", name);
    else
        g_critical ("Failed to connect to bus: %s", name);
}

static void
on_name_acquired (GDBusConnection *connection, const char *name, gpointer user_data)
{
    g_message ("acquired DBus name %s", name);
}

static void
on_connection_acquired (GDBusConnection *connection, const char *name, gpointer user_data)
{
    MatiCommunicator *self = MATI_COMMUNICATOR (user_data);
    g_autoptr (GError) error = NULL;

    if (!g_dbus_interface_skeleton_export (G_DBUS_INTERFACE_SKELETON (self), connection, "/com/froura/mati/app", &error))
        g_critical ("Failed to export mati to dbus: %s", error->message);

    g_signal_connect_object (MATI_DBUS_ (self), "handle-get-diagnostics", G_CALLBACK (handle_get_diagnostics), self, 0);
}

MatiCommunicator *
mati_communicator_new (const char      *mati_id,
                       MatiApplication *app)
{
    MatiCommunicator *self;
    g_autofree char *dbus_name = NULL;

    self = g_object_new (MATI_TYPE_COMMUNICATOR, NULL);

    dbus_name = g_strdup_printf ("com.froura.mati.app_%s", mati_id);
    self->dbus_owner_id = g_bus_own_name (G_BUS_TYPE_SESSION, dbus_name, 0, on_connection_acquired, on_name_acquired, on_name_lost, self, NULL);
    self->app = app;

    return self;
}