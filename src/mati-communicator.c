#include "mati-communicator.h"

struct _MatiCommunicator
{
   MatiDbusSkeleton parent_instance;

   guint dbus_owner_id;
};

G_DEFINE_TYPE (MatiCommunicator, mati_communicator, MATI_DBUS_TYPE__SKELETON);

void mati_communicator_emit_next_file_signal (MatiCommunicator *self, guint next_number)
{
    g_message ("Emit next file signal with number %d", next_number);
    mati_dbus__emit_next_upload_file (MATI_DBUS_ (self), next_number);
}

static void
mati_communicator_finalize (GObject *object)
{
    MatiCommunicator *self = MATI_COMMUNICATOR (object);

    g_bus_unown_name (self->dbus_owner_id);

    G_OBJECT_CLASS (mati_communicator_parent_class)->finalize (object);
}

static void
mati_communicator_constructed (GObject *object)
{
    // Set initial values to dbus properties, connect method callbacks etc
    // g_signal_connect (object, "handle-method-x", G_CALLBACK (handle_method_x), NULL);
}

static void
mati_communicator_class_init (MatiCommunicatorClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS (klass);

    object_class->finalize = mati_communicator_finalize;
    object_class->constructed = mati_communicator_constructed;
}

static void
mati_communicator_init (MatiCommunicator *self)
{

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
}

MatiCommunicator *
mati_communicator_new (const char *mati_id)
{
    MatiCommunicator *self;
    g_autofree char *dbus_name = NULL;

    self = g_object_new (MATI_TYPE_COMMUNICATOR, NULL);

    dbus_name = g_strdup_printf ("com.froura.mati.app_%s", mati_id);
    self->dbus_owner_id = g_bus_own_name (G_BUS_TYPE_SESSION, dbus_name, 0, on_connection_acquired, on_name_acquired, on_name_lost, self, NULL);

    return self;
}