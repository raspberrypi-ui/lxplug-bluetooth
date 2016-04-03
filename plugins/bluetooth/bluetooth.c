#include <errno.h>
#include <locale.h>
#include <stdlib.h>
#include <string.h>
#include <glib/gi18n.h>

#include <gio/gio.h>

#include "config.h"
#include "plugin.h"

#define ICON_BUTTON_TRIM 4

#define DEBUG_ON
#ifdef DEBUG_ON
#define DEBUG(fmt,args...) printf(fmt,##args)
#else
#define DEBUG
#endif

/* Plug-in global data */

typedef struct {

    GtkWidget *plugin;              /* Back pointer to the widget */
    LXPanel *panel;                 /* Back pointer to panel */
    GtkWidget *tray_icon;           /* Displayed image */
    config_setting_t *settings;     /* Plugin settings */
    GtkWidget *menu;                /* Popup menu */
    GtkListStore *pair_list;
    GtkListStore *unpair_list;
    GDBusObjectManager *objmanager;
    GDBusProxy *agentmanager;
    GDBusProxy *adapter;
    GDBusConnection *gbc;
    gchar *pairing_object;
    GtkWidget *list_dialog, *list;
    GtkWidget *pair_dialog, *pair_label, *pair_entry, *pair_ok, *pair_cancel;
    GtkEntryBuffer *pinbuf;
    GDBusMethodInvocation *invocation;
    gulong ok_instance;
    gulong cancel_instance;
} BluetoothPlugin;

typedef enum {
    DIALOG_PAIR,
    DIALOG_REMOVE
} DIALOG_TYPE;

typedef enum {
    STATE_PAIR_INIT,
    STATE_REQUEST_PIN,
    STATE_REQUEST_PASS,
    STATE_DISPLAY_PIN,
    STATE_CONFIRM_PIN,
    STATE_PAIR_ERROR,
    STATE_PAIRED,
    STATE_CONNECTED,
    STATE_CONNECT_FAIL
} PAIR_STATE;

/* Agent data */

static void handle_method_call (GDBusConnection *connection, const gchar *sender, const gchar *object_path,
    const gchar *interface_name, const gchar *method_name, GVariant *parameters, GDBusMethodInvocation *invocation,
    gpointer user_data);

static const GDBusInterfaceVTable interface_vtable =
{
  handle_method_call,
  NULL,
  NULL
};

static const gchar introspection_xml[] =
"<!DOCTYPE node PUBLIC \"-//freedesktop//DTD D-BUS Object Introspection 1.0//EN\"\n"
"\"http://www.freedesktop.org/standards/dbus/1.0/introspect.dtd\";>\n"
"<node>\n"
"   <interface name=\"org.bluez.Agent1\">\n"
"       <signal name=\"pinRequested\">\n"
"           <arg name=\"arg_0\" type=\"s\"/>\n"
"       </signal>\n"
"       <signal name=\"agentReleased\">\n"
"       </signal>\n"
"       <method name=\"Release\">\n"
"       </method>\n"
"       <method name=\"AuthorizeService\">\n"
"           <arg name=\"device\" type=\"o\" direction=\"in\"/>\n"
"           <arg name=\"uuid\" type=\"s\" direction=\"in\"/>\n"
"       </method>\n"
"       <method name=\"RequestPinCode\">\n"
"           <arg name=\"device\" type=\"o\" direction=\"in\"/>\n"
"           <arg type=\"s\" direction=\"out\"/>\n"
"       </method>\n"
"       <method name=\"RequestPasskey\">\n"
"           <arg name=\"device\" type=\"o\" direction=\"in\"/>\n"
"           <arg type=\"u\" direction=\"out\"/>\n"
"       </method>\n"
"       <method name=\"DisplayPinCode\">\n"
"           <arg name=\"device\" type=\"o\" direction=\"in\"/>\n"
"           <arg name=\"pincode\" type=\"s\" direction=\"in\"/>\n"
"       </method>\n"
"       <method name=\"DisplayPasskey\">\n"
"           <arg name=\"device\" type=\"o\" direction=\"in\"/>\n"
"           <arg name=\"passkey\" type=\"u\" direction=\"in\"/>\n"
"           <arg name=\"entered\" type=\"q\" direction=\"in\"/>\n"
"       </method>\n"
"       <method name=\"RequestConfirmation\">\n"
"           <arg name=\"device\" type=\"o\" direction=\"in\"/>\n"
"           <arg name=\"passkey\" type=\"u\" direction=\"in\"/>\n"
"       </method>\n"
"       <method name=\"Cancel\">\n"
"       </method>\n"
"   </interface>\n"
"   <interface name=\"org.freedesktop.DBus.Introspectable\">\n"
"       <method name=\"Introspect\">\n"
"           <arg name=\"data\" direction=\"out\" type=\"s\"/>\n"
"       </method>\n"
"   </interface>\n"
"</node>\n";

/*---------------------------------------------------------------------------*/
/* Prototypes */
/*---------------------------------------------------------------------------*/

static void initialise (BluetoothPlugin *bt);
static void clear (BluetoothPlugin *bt);
static void find_hardware (BluetoothPlugin *bt);
static void cb_name_owned (GDBusConnection *connection, const gchar *name, const gchar *owner, gpointer user_data);
static void cb_name_unowned (GDBusConnection *connection, const gchar *name, gpointer user_data);
static void cb_interface_added (GDBusObjectManager *manager, GDBusObject *object, GDBusInterface *interface, gpointer user_data);
static void cb_interface_removed (GDBusObjectManager *manager, GDBusObject *object, GDBusInterface *interface, gpointer user_data);
static void cb_object_added (GDBusObjectManager *manager, GDBusObject *object, gpointer user_data);
static void cb_object_removed (GDBusObjectManager *manager, GDBusObject *object, gpointer user_data);
static void cb_interface_signal (GDBusObjectManagerClient *manager, GDBusObjectProxy *object_proxy, GDBusProxy *proxy, gchar *sender, gchar *signal, GVariant *parameters, gpointer user_data);
static void cb_interface_properties (GDBusObjectManagerClient *manager, GDBusObjectProxy *object_proxy, GDBusProxy *proxy, GVariant *parameters, GStrv inval, gpointer user_data);
static gboolean is_searching (BluetoothPlugin *bt);
static void set_search (BluetoothPlugin *bt, gboolean state);
static void cb_search_start (GObject *source, GAsyncResult *res, gpointer user_data);
static void cb_search_end (GObject *source, GAsyncResult *res, gpointer user_data);
static gboolean is_discoverable (BluetoothPlugin *bt);
static void set_discoverable (BluetoothPlugin *bt, gboolean state);
static void cb_discover_start (GObject *source, GAsyncResult *res, gpointer user_data);
static void cb_discover_end (GObject *source, GAsyncResult *res, gpointer user_data);
static gboolean is_paired (BluetoothPlugin *bt, const gchar *path);
static void pair_device (BluetoothPlugin *bt, const gchar *path, gboolean state);
static void cb_paired (GObject *source, GAsyncResult *res, gpointer user_data);
static void cb_cancelled (GObject *source, GAsyncResult *res, gpointer user_data);
static gboolean is_trusted (BluetoothPlugin *bt, const gchar *path);
static void trust_device (BluetoothPlugin *bt, const gchar *path, gboolean state);
static void cb_trusted (GObject *source, GAsyncResult *res, gpointer user_data);
static gboolean is_connected (BluetoothPlugin *bt, const gchar *path);
static void connect_device (BluetoothPlugin *bt, const gchar *path, gboolean state);
static void cb_connected (GObject *source, GAsyncResult *res, gpointer user_data);
static void cb_disconnected (GObject *source, GAsyncResult *res, gpointer user_data);
static void remove_device (BluetoothPlugin *bt, const gchar *path);
static void cb_removed (GObject *source, GAsyncResult *res, gpointer user_data);

static guint request_authorization (BluetoothPlugin *bt, const gchar *device);
static void handle_pin_entered (GtkButton *button, gpointer user_data);
static void handle_pass_entered (GtkButton *button, gpointer user_data);
static void handle_pin_confirmed (GtkButton *button, gpointer user_data);
static void handle_pin_rejected (GtkButton *button, gpointer user_data);
static void handle_cancel_pair (GtkButton *button, gpointer user_data);
static void handle_close_pair_dialog (GtkButton *button, gpointer user_data);
static void connect_ok (BluetoothPlugin *bt, void (*cb) (void));
static void connect_cancel (BluetoothPlugin *bt, void (*cb) (void));
static void show_pairing_dialog (BluetoothPlugin *bt, PAIR_STATE state, const gchar *device, const gchar *param);
static gboolean selected_path (BluetoothPlugin *bt, char **path, char **name);
static void handle_pair (GtkButton *button, gpointer user_data);
static void handle_remove (GtkButton *button, gpointer user_data);
static void handle_close_list_dialog (GtkButton *button, gpointer user_data);
static void show_list_dialog (BluetoothPlugin * bt, DIALOG_TYPE type);
static void handle_menu_add (GtkWidget *widget, gpointer user_data);
static void handle_menu_remove (GtkWidget *widget, gpointer user_data);
static void handle_menu_connect (GtkWidget *widget, gpointer user_data);
static void handle_menu_discover (GtkWidget *widget, gpointer user_data);
static gboolean add_to_menu (GtkTreeModel *model, GtkTreePath *tpath, GtkTreeIter *iter, gpointer user_data);
static void add_device (BluetoothPlugin *bt, GDBusObject *object, GtkListStore *lst);
static void update_device_list (BluetoothPlugin *bt);
static void set_icon (LXPanel *p, GtkWidget *image, const char *icon, int size);
static void menu_popup_set_position (GtkMenu *menu, gint *px, gint *py, gboolean *push_in, gpointer data);
static void show_menu (BluetoothPlugin *bt);

/*---------------------------------------------------------------------------*/
/* Function Definitions */
/*---------------------------------------------------------------------------*/

/* Find an object manager and set up the callbacks to monitor the DBus for BlueZ objects.
   Also create the DBus agent which handles pairing for use later. */

static void initialise (BluetoothPlugin *bt)
{
    GError *error;
    GDBusNodeInfo *introspection_data;

    // make sure everything is reset - should be unnecessary...
    clear (bt);

    // get an object manager for BlueZ
    error = NULL;
    bt->objmanager = g_dbus_object_manager_client_new_for_bus_sync (G_BUS_TYPE_SYSTEM, 0, "org.bluez", "/", NULL, NULL, NULL, NULL, &error);
    if (error) DEBUG ("Error getting object manager - %s\n", error->message);

    // register callbacks on object manager
    g_signal_connect (bt->objmanager, "interface-added", G_CALLBACK (cb_interface_added), bt);
    g_signal_connect (bt->objmanager, "interface-removed", G_CALLBACK (cb_interface_removed), bt);
    g_signal_connect (bt->objmanager, "object-added", G_CALLBACK (cb_object_added), bt);
    g_signal_connect (bt->objmanager, "object-removed", G_CALLBACK (cb_object_removed), bt);
    g_signal_connect (bt->objmanager, "interface-proxy-signal", G_CALLBACK (cb_interface_signal), bt);
    g_signal_connect (bt->objmanager, "interface-proxy-properties-changed", G_CALLBACK (cb_interface_properties), bt);

    // get a connection to the system DBus
    error = NULL;
    bt->gbc = g_bus_get_sync (G_BUS_TYPE_SYSTEM, NULL, &error);
    if (error) DEBUG ("Error getting system bus - %s\n", error->message);

    // create the agent from XML spec
    error = NULL;
    introspection_data = g_dbus_node_info_new_for_xml (introspection_xml, &error);
    if (error) DEBUG ("Error creating agent node - %s\n", error->message);

    // register the agent on the system bus
    error = NULL;
    g_dbus_connection_register_object (bt->gbc, "/btagent", introspection_data->interfaces[0], &interface_vtable, bt, NULL, &error);
    if (error) DEBUG ("Error registering agent on bus - %s\n", error->message);

    // query the DBus for an agent manager and a Bluetooth adapter
    find_hardware (bt);
}

/* Clear all the BlueZ data if the DBus connection is lost */

static void clear (BluetoothPlugin *bt)
{
    bt->objmanager = NULL;
    bt->gbc = NULL;
    bt->agentmanager = NULL;
    bt->adapter = NULL;
}

/* Scan for BlueZ objects on DBus, identifying the agent manager
   (of which there should be only one per BlueZ instance) and an adapter (the object 
   which corresponds to the Bluetooth hardware on the host computer). */ 

static void find_hardware (BluetoothPlugin *bt)
{
    GDBusInterface *interface;
    GDBusObject *object;
    GList *objects, *interfaces;
    GDBusProxy *newagentmanager = NULL, *newadapter = NULL;
    GError *error;

    objects = g_dbus_object_manager_get_objects (bt->objmanager);

    while (objects != NULL)
    {
        object = (GDBusObject*) objects->data;

        interfaces = g_dbus_object_get_interfaces (object);
        while (interfaces != NULL)
        {
            interface = G_DBUS_INTERFACE (interfaces->data);
            if (g_strcmp0 (g_dbus_proxy_get_interface_name (G_DBUS_PROXY (interface)), "org.bluez.Adapter1") == 0)
            {
                if (newadapter == NULL) newadapter = G_DBUS_PROXY (interface);
                else DEBUG ("Multiple adapters found\n");
            }
            else if (g_strcmp0 (g_dbus_proxy_get_interface_name (G_DBUS_PROXY (interface)), "org.bluez.AgentManager1") == 0)
            {
                if (newagentmanager == NULL) newagentmanager = G_DBUS_PROXY (interface);
                else DEBUG ("Multiple agent managers found\n");
            }
            interfaces = interfaces->next;
        }
        objects = objects->next;
    }

    if (!newagentmanager)
    {
        DEBUG ("No agent manager found\n");
        bt->agentmanager = NULL;
    }
    else if (newagentmanager != bt->agentmanager)
    {
        DEBUG ("New agent manager found\n");
        // if there is already an agent manager, unregister the agent from it
        if (bt->agentmanager)
        {
            error = NULL;
            g_dbus_proxy_call_sync (bt->agentmanager, "UnregisterAgent", g_variant_new ("(o)", "/btagent"), G_DBUS_CALL_FLAGS_NONE, -1, NULL, &error);
            if (error) DEBUG ("Error unregistering agent with manager - %s\n", error->message);
        }

        bt->agentmanager = newagentmanager;

        // register the agent with the new agent manager
        if (bt->agentmanager)
        {
            error = NULL;
            //g_dbus_proxy_call_sync (bt->agentmanager, "RegisterAgent", g_variant_new ("(os)", "/btagent", "DisplayYesNo"), G_DBUS_CALL_FLAGS_NONE, -1, NULL, &error);
            g_dbus_proxy_call_sync (bt->agentmanager, "RegisterAgent", g_variant_new ("(os)", "/btagent", "KeyboardDisplay"), G_DBUS_CALL_FLAGS_NONE, -1, NULL, &error);
            if (error) DEBUG ("Error registering agent with manager - %s\n", error->message);
        }
    }

    if (!newadapter)
    {
        DEBUG ("No adapter found\n");
        bt->adapter = NULL;
    }
    else if (newadapter != bt->adapter)
    {
        DEBUG ("New adapter found\n");
        bt->adapter = newadapter;
    }

   // initialise lists with current state of object proxy
   update_device_list (bt);
}

/* Handler for method calls on the agent */

static void handle_method_call (GDBusConnection *connection, const gchar *sender, const gchar *object_path,
    const gchar *interface_name, const gchar *method_name, GVariant *parameters, GDBusMethodInvocation *invocation,
    gpointer user_data)
{
    BluetoothPlugin * bt = (BluetoothPlugin *) user_data;
    GDBusInterface *interface;
    GVariant *var;
    gchar buffer[16];

    DEBUG ("Agent method %s called\n", method_name);
    if (parameters) DEBUG ("with parameters %s\n", g_variant_print (parameters, TRUE));

    if (g_strcmp0 (method_name, "Cancel") == 0) return;

    bt->invocation = invocation;
    interface = g_dbus_object_manager_get_interface (bt->objmanager, g_variant_get_string (g_variant_get_child_value (parameters, 0), NULL), "org.bluez.Device1");
    var = g_dbus_proxy_get_cached_property (G_DBUS_PROXY (interface), "Alias");

    if (g_strcmp0 (method_name, "RequestConfirmation") == 0)
    {
        sprintf (buffer, "%lu", g_variant_get_uint32 (g_variant_get_child_value (parameters, 1)));
        show_pairing_dialog (bt, STATE_CONFIRM_PIN, g_variant_get_string (var, NULL), buffer);
    }
    else if (g_strcmp0 (method_name, "DisplayPinCode") == 0)
    {
        g_dbus_method_invocation_return_value (invocation, NULL);
        show_pairing_dialog (bt, STATE_DISPLAY_PIN, g_variant_get_string (var, NULL), g_variant_get_string (g_variant_get_child_value (parameters, 1), NULL));
    }
    else if (g_strcmp0 (method_name, "DisplayPasskey") == 0)   // !!!! do we need to do something with "entered" parameter here?
    {
        g_dbus_method_invocation_return_value (invocation, NULL);
        sprintf (buffer, "%lu", g_variant_get_uint32 (g_variant_get_child_value (parameters, 1)));
        show_pairing_dialog (bt, STATE_DISPLAY_PIN, g_variant_get_string (var, NULL), buffer);
    }
    else if (g_strcmp0 (method_name, "RequestPinCode") == 0)
    {
        show_pairing_dialog (bt, STATE_REQUEST_PIN, g_variant_get_string (var, NULL), NULL);
    }
    else if (g_strcmp0 (method_name, "RequestPasskey") == 0)
    {
        show_pairing_dialog (bt, STATE_REQUEST_PASS, g_variant_get_string (var, NULL), NULL);
    }
    else if (g_strcmp0 (method_name, "RequestAuthorization") == 0)
    {
        if (request_authorization (bt, g_variant_get_string (var, NULL)))
            g_dbus_method_invocation_return_value (invocation, NULL);
        else
            g_dbus_method_invocation_return_dbus_error (invocation, "org.bluez.Error.Rejected", "Pairing rejected by user");
    }
}

/* Bus watcher callbacks - information only at present; may want to link the general initialisation to these */

static void cb_name_owned (GDBusConnection *connection, const gchar *name, const gchar *owner, gpointer user_data)
{
    BluetoothPlugin * bt = (BluetoothPlugin *) user_data;
    printf ("Name %s owned on DBus\n", name);
    initialise (bt);
}

static void cb_name_unowned (GDBusConnection *connection, const gchar *name, gpointer user_data)
{
    BluetoothPlugin * bt = (BluetoothPlugin *) user_data;
    printf ("Name %s unowned on DBus\n", name);
    clear (bt);
}

/* Object manager callbacks - we only really care about objects added, removed or with changed properties */

static void cb_interface_added (GDBusObjectManager *manager, GDBusObject *object, GDBusInterface *interface, gpointer user_data)
{
    DEBUG ("Object manager - interface added\n");
}

static void cb_interface_removed (GDBusObjectManager *manager, GDBusObject *object, GDBusInterface *interface, gpointer user_data)
{
    DEBUG ("Object manager - interface removed\n");
}

static void cb_object_added (GDBusObjectManager *manager, GDBusObject *object, gpointer user_data)
{
    BluetoothPlugin * bt = (BluetoothPlugin *) user_data;
    DEBUG ("Object manager - object added at path %s\n", g_dbus_object_get_object_path (object));
    find_hardware (bt);
}

static void cb_object_removed (GDBusObjectManager *manager, GDBusObject *object, gpointer user_data)
{
    BluetoothPlugin * bt = (BluetoothPlugin *) user_data;
    DEBUG ("Object manager - object removed at path %s\n", g_dbus_object_get_object_path (object));
    find_hardware (bt);
}

static void cb_interface_signal (GDBusObjectManagerClient *manager, GDBusObjectProxy *object_proxy, GDBusProxy *proxy, gchar *sender, gchar *signal, GVariant *parameters, gpointer user_data)
{
    DEBUG ("Object manager - object at %s interface signal %s %s %s\n", g_dbus_proxy_get_object_path (proxy), sender, signal, g_variant_print (parameters, TRUE));
}

static void cb_interface_properties (GDBusObjectManagerClient *manager, GDBusObjectProxy *object_proxy, GDBusProxy *proxy, GVariant *parameters, GStrv inval, gpointer user_data)
{
    BluetoothPlugin * bt = (BluetoothPlugin *) user_data;
    GVariant *var;

    DEBUG ("Object manager - object at %s property changed - %s %s\n",  g_dbus_proxy_get_object_path (proxy), g_dbus_proxy_get_interface_name (proxy), g_variant_print (parameters, TRUE));
    update_device_list (bt);

    // hack to reconnect after successful pairing
    var = g_variant_lookup_value (parameters, "Connected", NULL);
    if (var && g_variant_get_boolean (var) == FALSE)
    {
        if (g_strcmp0 (g_dbus_proxy_get_object_path (proxy), bt->pairing_object) == 0)
        {
            DEBUG ("Paired object disconnected - reconnecting\n");
            connect_device (bt, bt->pairing_object, TRUE);
            g_free (bt->pairing_object);
            bt->pairing_object = NULL;
        }
    }
}

/* Searching */

static gboolean is_searching (BluetoothPlugin *bt)
{
    GVariant *var = g_dbus_proxy_get_cached_property (G_DBUS_PROXY (bt->adapter), "Discovering");
    return g_variant_get_boolean (var);
}

static void set_search (BluetoothPlugin *bt, gboolean state)
{
    if (state)
    {
        DEBUG ("Starting search\n");
        g_dbus_proxy_call (bt->adapter, "StartDiscovery", NULL, G_DBUS_CALL_FLAGS_NONE, -1, NULL, cb_search_start, NULL);
    }
    else
    {
        DEBUG ("Ending search\n");
        g_dbus_proxy_call (bt->adapter, "StopDiscovery", NULL, G_DBUS_CALL_FLAGS_NONE, -1, NULL, cb_search_end, NULL);
    }
}

static void cb_search_start (GObject *source, GAsyncResult *res, gpointer user_data)
{
    GError *error = NULL;
    GVariant *var = g_dbus_proxy_call_finish (G_DBUS_PROXY (source), res, &error);

    if (error) DEBUG ("Search start - error %s\n", error->message);
    else DEBUG ("Search start - result %s\n", g_variant_print (var, TRUE));
}

static void cb_search_end (GObject *source, GAsyncResult *res, gpointer user_data)
{
    GError *error = NULL;
    GVariant *var= g_dbus_proxy_call_finish (G_DBUS_PROXY (source), res, &error);

    if (error) DEBUG ("Search end - error %s\n", error->message);
    else DEBUG ("Search end - result %s\n", g_variant_print (var, TRUE));
}

/* Discoverable */

static gboolean is_discoverable (BluetoothPlugin *bt)
{
    GVariant *var = g_dbus_proxy_get_cached_property (G_DBUS_PROXY (bt->adapter), "Discoverable");
    return g_variant_get_boolean (var);
}

static void set_discoverable (BluetoothPlugin *bt, gboolean state)
{
    GVariant *var = g_variant_new ("(ssv)", g_dbus_proxy_get_interface_name (G_DBUS_PROXY (bt->adapter)), "Discoverable", g_variant_new_boolean (state));

    if (state)
    {
        DEBUG ("Making discoverable\n");
        g_dbus_proxy_call (bt->adapter, "org.freedesktop.DBus.Properties.Set", var, G_DBUS_CALL_FLAGS_NONE, -1, NULL, cb_discover_start, NULL);
    }
    else
    {
        DEBUG ("Stopping discoverable\n");
        g_dbus_proxy_call (bt->adapter, "org.freedesktop.DBus.Properties.Set", var, G_DBUS_CALL_FLAGS_NONE, -1, NULL, cb_discover_end, NULL);
    }
}

static void cb_discover_start (GObject *source, GAsyncResult *res, gpointer user_data)
{
    GError *error = NULL;
    GVariant *var = g_dbus_proxy_call_finish (G_DBUS_PROXY (source), res, &error);
    
    if (error) DEBUG ("Discoverable start - error %s\n", error->message);
    else DEBUG ("Discoverable start - result %s\n", g_variant_print (var, TRUE));
}

static void cb_discover_end (GObject *source, GAsyncResult *res, gpointer user_data)
{
    GError *error = NULL;
    GVariant *var = g_dbus_proxy_call_finish (G_DBUS_PROXY (source), res, &error);
    
    if (error) DEBUG ("Discoverable end - error %s\n", error->message);
    else DEBUG ("Discoverable end - result %s\n", g_variant_print (var, TRUE));
}

/* Pair device / cancel pairing */

static gboolean is_paired (BluetoothPlugin *bt, const gchar *path)
{
    GDBusInterface *interface = g_dbus_object_manager_get_interface (bt->objmanager, path, "org.bluez.Device1");
    GVariant *var = g_dbus_proxy_get_cached_property (G_DBUS_PROXY (interface), "Paired");
    return g_variant_get_boolean (var);
}

static void pair_device (BluetoothPlugin *bt, const gchar *path, gboolean state)
{
    GDBusInterface *interface = g_dbus_object_manager_get_interface (bt->objmanager, path, "org.bluez.Device1");

    if (state)
    {
        DEBUG ("Pairing with %s\n", path);
        g_dbus_proxy_call (G_DBUS_PROXY (interface), "Pair", NULL, G_DBUS_CALL_FLAGS_NONE, -1, NULL, cb_paired, bt);
    }
    else
    {
        DEBUG ("CancelPairing with %s\n", path);
        g_dbus_proxy_call (G_DBUS_PROXY (interface), "CancelPairing", NULL, G_DBUS_CALL_FLAGS_NONE, -1, NULL, cb_cancelled, NULL);
    }
}

static void cb_paired (GObject *source, GAsyncResult *res, gpointer user_data)
{
    BluetoothPlugin * bt = (BluetoothPlugin *) user_data;
    GError *error = NULL;
    GVariant *var = g_dbus_proxy_call_finish (G_DBUS_PROXY (source), res, &error);

    if (error)
    {
        DEBUG ("Pairing error %s\n", error->message);
        show_pairing_dialog (bt, STATE_PAIR_ERROR, NULL, error->message);
        if (bt->pairing_object)
        {
            g_free (bt->pairing_object);
            bt->pairing_object = NULL;
        }
    }
    else
    {
        DEBUG ("Pairing result %s\n", g_variant_print (var, TRUE));
        show_pairing_dialog (bt, STATE_PAIRED, NULL, NULL);
    }
}

static void cb_cancelled (GObject *source, GAsyncResult *res, gpointer user_data)
{
    GError *error = NULL;
    GVariant *var = g_dbus_proxy_call_finish (G_DBUS_PROXY (source), res, &error);

    if (error) DEBUG ("Cancelling error %s\n", error->message);
    else DEBUG ("Cancelling result %s\n", g_variant_print (var, TRUE));
 }

/* Trust / distrust device */

static gboolean is_trusted (BluetoothPlugin *bt, const gchar *path)
{
    GDBusInterface *interface = g_dbus_object_manager_get_interface (bt->objmanager, path, "org.bluez.Device1");
    GVariant *var = g_dbus_proxy_get_cached_property (G_DBUS_PROXY (interface), "Trusted");
    return g_variant_get_boolean (var);
}

static void trust_device (BluetoothPlugin *bt, const gchar *path, gboolean state)
{
    GDBusInterface *interface = g_dbus_object_manager_get_interface (bt->objmanager, path, "org.bluez.Device1");
    GVariant *var = g_variant_new ("(ssv)", g_dbus_proxy_get_interface_name (G_DBUS_PROXY (interface)), "Trusted", g_variant_new_boolean (state));

    if (state) DEBUG ("Trusting %s\n", path);
    else DEBUG ("Distrusting %s\n", path);
    g_dbus_proxy_call (G_DBUS_PROXY (interface), "org.freedesktop.DBus.Properties.Set", var, G_DBUS_CALL_FLAGS_NONE, -1, NULL, cb_trusted, NULL);
}

static void cb_trusted (GObject *source, GAsyncResult *res, gpointer user_data)
{
    GError *error = NULL;
    GVariant *var = g_dbus_proxy_call_finish (G_DBUS_PROXY (source), res, &error);

    if (error) DEBUG ("Trusting error %s\n", error->message);
    else DEBUG ("Trusting result %s\n", g_variant_print (var, TRUE));
}

/* Connect / disconnect device */

static gboolean is_connected (BluetoothPlugin *bt, const gchar *path)
{
    GDBusInterface *interface = g_dbus_object_manager_get_interface (bt->objmanager, path, "org.bluez.Device1");
    GVariant *var = g_dbus_proxy_get_cached_property (G_DBUS_PROXY (interface), "Connected");
    return g_variant_get_boolean (var);
}

static void connect_device (BluetoothPlugin *bt, const gchar *path, gboolean state)
{
    GDBusInterface *interface = g_dbus_object_manager_get_interface (bt->objmanager, path, "org.bluez.Device1");

    if (state)
    {
        // if trying to connect, make sure device is trusted first
        if (!is_trusted (bt, path)) trust_device (bt, path, TRUE);

        DEBUG ("Connecting to %s\n", path);
        g_dbus_proxy_call (G_DBUS_PROXY (interface), "Connect", NULL, G_DBUS_CALL_FLAGS_NONE, -1, NULL, cb_connected, bt);
    }
    else
    {
        DEBUG ("Disconnecting from %s\n", path); 
        g_dbus_proxy_call (G_DBUS_PROXY (interface), "Disconnect", NULL, G_DBUS_CALL_FLAGS_NONE, -1, NULL, cb_disconnected, NULL);
    }
}

static void cb_connected (GObject *source, GAsyncResult *res, gpointer user_data)
{
    BluetoothPlugin *bt = (BluetoothPlugin *) user_data;
    GError *error = NULL;
    GVariant *var = g_dbus_proxy_call_finish (G_DBUS_PROXY (source), res, &error);

    if (error)
    {
        DEBUG ("Connect error %s\n", error->message);
        if (bt->pair_dialog) show_pairing_dialog (bt, STATE_CONNECT_FAIL, NULL, error->message);
    }
    else
    {
        DEBUG ("Connect result %s\n", g_variant_print (var, TRUE));
        if (bt->pair_dialog) show_pairing_dialog (bt, STATE_CONNECTED, NULL, NULL);
    }
}

static void cb_disconnected (GObject *source, GAsyncResult *res, gpointer user_data)
{
    GError *error = NULL;
    GVariant *var = g_dbus_proxy_call_finish (G_DBUS_PROXY (source), res, &error);

    if (error) DEBUG ("Disconnect error %s\n", error->message);
    else DEBUG ("Disconnect result %s\n", g_variant_print (var, TRUE));
}

/* Remove device */

static void remove_device (BluetoothPlugin *bt, const gchar *path)
{
    DEBUG ("Removing\n");
    g_dbus_proxy_call (bt->adapter, "RemoveDevice", g_variant_new ("(o)", path), G_DBUS_CALL_FLAGS_NONE, -1, NULL, cb_removed, NULL);
}

static void cb_removed (GObject *source, GAsyncResult *res, gpointer user_data)
{
    GError *error = NULL;
    GVariant *var = g_dbus_proxy_call_finish (G_DBUS_PROXY (source), res, &error);

    if (error) DEBUG ("Remove error %s\n", error->message);
    else DEBUG ("Remove result %s\n", g_variant_print (var, TRUE));
}


/* GUI... */

/* Functions to respond to method calls on the agent */

static guint request_authorization (BluetoothPlugin *bt, const gchar *device)
{
    char buffer[256];
    guint res;
    GtkWidget *lbl;

    // create the dialog, asking user to confirm the displayed PIN
    sprintf (buffer, "Do you accept pairing from device %s?", device);
    bt->list_dialog = gtk_dialog_new_with_buttons ("Pairing Request", NULL, GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT, GTK_STOCK_CANCEL, 0, GTK_STOCK_OK, 1, NULL);
    lbl = gtk_label_new (buffer);
    gtk_label_set_line_wrap (GTK_LABEL (lbl), TRUE);
    gtk_box_pack_start (GTK_BOX (gtk_dialog_get_content_area (GTK_DIALOG (bt->list_dialog))), lbl, TRUE, TRUE, 0);
    gtk_widget_show_all (bt->list_dialog);

    // block while waiting for user response
    res = gtk_dialog_run (GTK_DIALOG (bt->list_dialog));
    gtk_widget_destroy (bt->list_dialog);
    bt->list_dialog = NULL;
    return res;
}

static void handle_pin_entered (GtkButton *button, gpointer user_data)
{
    BluetoothPlugin *bt = (BluetoothPlugin *) user_data;
    DEBUG ("PIN entered by user\n");
    GVariant *retvar = g_variant_new ("(s)", gtk_entry_buffer_get_text (bt->pinbuf));
    g_dbus_method_invocation_return_value (bt->invocation, retvar);
}

static void handle_pass_entered (GtkButton *button, gpointer user_data)
{
    BluetoothPlugin *bt = (BluetoothPlugin *) user_data;
    guint32 passkey;
    DEBUG ("Passkey entered by user\n");
    sscanf (gtk_entry_buffer_get_text (bt->pinbuf), "%lu", &passkey);
    GVariant *retvar = g_variant_new ("(u)", passkey);
    g_dbus_method_invocation_return_value (bt->invocation, retvar);
}

static void handle_pin_confirmed (GtkButton *button, gpointer user_data)
{
    BluetoothPlugin *bt = (BluetoothPlugin *) user_data;
    DEBUG ("PIN confirmed by user\n");
    g_dbus_method_invocation_return_value (bt->invocation, NULL);
}

static void handle_pin_rejected (GtkButton *button, gpointer user_data)
{
    BluetoothPlugin *bt = (BluetoothPlugin *) user_data;
    DEBUG ("PIN rejected by user\n");
    g_dbus_method_invocation_return_dbus_error (bt->invocation, "org.bluez.Error.Rejected", "Confirmation rejected by user");
    if (bt->pairing_object)
    {
        g_free (bt->pairing_object);
        bt->pairing_object = NULL;
    }
}

static void handle_cancel_pair (GtkButton *button, gpointer user_data)
{
    BluetoothPlugin *bt = (BluetoothPlugin *) user_data;
    pair_device (bt, bt->pairing_object, FALSE);
    gtk_widget_destroy (bt->pair_dialog);
    bt->pair_dialog = NULL;
    if (bt->pairing_object)
    {
        g_free (bt->pairing_object);
        bt->pairing_object = NULL;
    }
}

static void handle_close_pair_dialog (GtkButton *button, gpointer user_data)
{
    BluetoothPlugin *bt = (BluetoothPlugin *) user_data;
    gtk_widget_destroy (bt->pair_dialog);
    bt->pair_dialog = NULL;
    if (bt->pairing_object)
    {
        g_free (bt->pairing_object);
        bt->pairing_object = NULL;
    }
}

static void connect_ok (BluetoothPlugin *bt, void (*cb) (void))
{
    if (bt->ok_instance) g_signal_handler_disconnect (bt->pair_ok, bt->ok_instance);
    bt->ok_instance = g_signal_connect (bt->pair_ok, "clicked", cb, bt);
}

static void connect_cancel (BluetoothPlugin *bt, void (*cb) (void))
{
    if (bt->cancel_instance) g_signal_handler_disconnect (bt->pair_cancel, bt->cancel_instance);
    bt->cancel_instance = g_signal_connect (bt->pair_cancel, "clicked", cb, bt);
}

static void show_pairing_dialog (BluetoothPlugin *bt, PAIR_STATE state, const gchar *device, const gchar *param)
{
    char buffer[256];

    switch (state)
    {
        case STATE_PAIR_INIT:
            sprintf (buffer, "Pairing with %s", device);
            bt->pinbuf = gtk_entry_buffer_new (NULL, -1);
            bt->pair_dialog = gtk_dialog_new_with_buttons (buffer, NULL, GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT, NULL);
            gtk_window_set_icon (GTK_WINDOW (bt->pair_dialog), gtk_icon_theme_load_icon (panel_get_icon_theme (bt->panel), "preferences-system-bluetooth", 24, 0, NULL));
            gtk_window_set_position (GTK_WINDOW (bt->pair_dialog), GTK_WIN_POS_CENTER_ALWAYS);
            gtk_container_set_border_width (GTK_CONTAINER (bt->pair_dialog), 10);
            bt->pair_label = gtk_label_new ("Pairing request sent to device - waiting for response....");
            gtk_label_set_line_wrap (GTK_LABEL (bt->pair_label), TRUE);
            gtk_label_set_justify (GTK_LABEL (bt->pair_label), GTK_JUSTIFY_LEFT);
            gtk_misc_set_alignment (GTK_MISC (bt->pair_label), 0.0, 0.0);
            gtk_box_pack_start (GTK_BOX (gtk_dialog_get_content_area (GTK_DIALOG (bt->pair_dialog))), bt->pair_label, TRUE, TRUE, 0);
            bt->pair_entry = gtk_entry_new_with_buffer (bt->pinbuf);
            gtk_box_pack_start (GTK_BOX (gtk_dialog_get_content_area (GTK_DIALOG (bt->pair_dialog))), bt->pair_entry, TRUE, TRUE, 0);
            bt->pair_cancel = gtk_dialog_add_button (GTK_DIALOG (bt->pair_dialog), "Cancel", 0);
            bt->pair_ok = gtk_dialog_add_button (GTK_DIALOG (bt->pair_dialog), "OK", 1);
            connect_cancel (bt, G_CALLBACK (handle_cancel_pair));
            gtk_widget_show_all (bt->pair_dialog);
            gtk_widget_hide (bt->pair_entry);
            gtk_widget_hide (bt->pair_ok);
            gtk_widget_show (bt->pair_cancel);
            break;

        case STATE_PAIR_ERROR:
            if (!bt->pair_dialog) return;
            sprintf (buffer, "Pairing failed - %s", param);
            gtk_label_set_text (GTK_LABEL (bt->pair_label), buffer);
            gtk_widget_hide (bt->pair_entry);
            connect_ok (bt, G_CALLBACK (handle_close_pair_dialog));
            gtk_widget_show (bt->pair_ok);
            gtk_widget_hide (bt->pair_cancel);
            break;

        case STATE_PAIRED:
            gtk_label_set_text (GTK_LABEL (bt->pair_label), "Pairing successful - creating connection...");
            gtk_widget_hide (bt->pair_entry);
            gtk_widget_hide (bt->pair_ok);
            gtk_widget_hide (bt->pair_cancel);
            break;

        case STATE_CONNECTED:
            gtk_label_set_text (GTK_LABEL (bt->pair_label), "Connected successfully");
            connect_ok (bt, G_CALLBACK (handle_close_pair_dialog));
            gtk_widget_show (bt->pair_ok);
            gtk_widget_hide (bt->pair_cancel);
            break;

        case STATE_CONNECT_FAIL:
            sprintf (buffer, "Connection failed - %s. Try to connect manually.", param);
            gtk_label_set_text (GTK_LABEL (bt->pair_label), buffer);
            connect_ok (bt, G_CALLBACK (handle_close_pair_dialog));
            gtk_widget_show (bt->pair_ok);
            gtk_widget_hide (bt->pair_cancel);
            break;

        case STATE_DISPLAY_PIN:
            sprintf (buffer, "Please enter code %s on device %s", param, device);
            gtk_label_set_text (GTK_LABEL (bt->pair_label), buffer);
            connect_cancel (bt, G_CALLBACK (handle_cancel_pair));
            gtk_widget_hide (bt->pair_ok);
            gtk_widget_show (bt->pair_cancel);
            break;

        case STATE_CONFIRM_PIN:
            sprintf (buffer, "Please confirm that device %s is showing the code %s to connect", device, param);
            gtk_label_set_text (GTK_LABEL (bt->pair_label), buffer);
            connect_cancel (bt, G_CALLBACK (handle_pin_rejected));
            connect_ok (bt, G_CALLBACK (handle_pin_confirmed));
            gtk_widget_show (bt->pair_ok);
            gtk_widget_show (bt->pair_cancel);
            break;

        case STATE_REQUEST_PIN:
        case STATE_REQUEST_PASS:
            sprintf (buffer, "Please enter PIN code for device %s", device);
            gtk_label_set_text (GTK_LABEL (bt->pair_label), buffer);
            gtk_widget_show (bt->pair_entry);
            connect_cancel (bt, G_CALLBACK (handle_cancel_pair));
            if (state == STATE_REQUEST_PIN)
                connect_ok (bt, G_CALLBACK (handle_pin_entered));
            else
                connect_ok (bt, G_CALLBACK (handle_pass_entered));
            gtk_widget_show (bt->pair_ok);
            gtk_widget_show (bt->pair_cancel);
            break;
    }
}

/* Functions to manage pair and remove dialogs */

static gboolean selected_path (BluetoothPlugin *bt, char **path, char **name)
{
    GtkTreeSelection *sel; 
    GtkTreeModel *model;
    GtkTreeIter iter;

    if (!bt->list)
    {
        *path = NULL;
        return FALSE;
    }
    sel = gtk_tree_view_get_selection (GTK_TREE_VIEW (bt->list));
    if (!gtk_tree_selection_get_selected (sel, &model, &iter))
    {
        *path = NULL;
        return FALSE;
    }    
    gtk_tree_model_get (model, &iter, 0, path, 1, name, -1);
    return TRUE;
}

static void handle_pair (GtkButton *button, gpointer user_data)
{
    BluetoothPlugin *bt = (BluetoothPlugin *) user_data;
    gchar *path, *name;

    if (selected_path (bt, &path, &name))
    {
        gtk_widget_destroy (bt->list);
        bt->list = NULL;
        gtk_widget_destroy (bt->list_dialog);
        bt->list_dialog = NULL;
        set_search (bt, FALSE);

        if (!is_paired (bt, path))
        {
            bt->pairing_object = path;
            show_pairing_dialog (bt, STATE_PAIR_INIT, name, NULL);
            pair_device (bt, path, TRUE);
        }
    }
}

static void handle_remove (GtkButton *button, gpointer user_data)
{
    BluetoothPlugin * bt = (BluetoothPlugin *) user_data;
    gchar *path, *name;

    if (selected_path (bt, &path, &name))
    {
        gtk_widget_destroy (bt->list);
        bt->list = NULL;
        gtk_widget_destroy (bt->list_dialog);
        bt->list_dialog = NULL;
        remove_device (bt, path);
        g_free (path);
    }
}

static void handle_close_list_dialog (GtkButton *button, gpointer user_data)
{
    BluetoothPlugin * bt = (BluetoothPlugin *) user_data;
    if (bt->list_dialog)
    {
        gtk_widget_destroy (bt->list);
        bt->list = NULL;
        gtk_widget_destroy (bt->list_dialog);
        bt->list_dialog = NULL;
        if (is_searching (bt)) set_search (bt, FALSE);
    }
}

static void show_list_dialog (BluetoothPlugin * bt, DIALOG_TYPE type)
{
    GtkWidget *btn_cancel, *btn_act, *frm, *lbl, *scrl;
    GtkCellRenderer *rend;

    // create the window
    bt->list_dialog = gtk_dialog_new_with_buttons (type == DIALOG_PAIR ? "Add New Device" : "Remove Device", NULL, GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT, NULL);
    gtk_window_set_position (GTK_WINDOW (bt->list_dialog), GTK_WIN_POS_CENTER_ALWAYS);
    gtk_window_set_icon (GTK_WINDOW (bt->list_dialog), gtk_icon_theme_load_icon (panel_get_icon_theme (bt->panel), "preferences-system-bluetooth", 24, 0, NULL));
    gtk_container_set_border_width (GTK_CONTAINER (bt->list_dialog), 5);
    gtk_box_set_spacing (GTK_BOX (gtk_dialog_get_content_area (GTK_DIALOG (bt->list_dialog))), 10);

    // add the buttons
    btn_cancel = gtk_dialog_add_button (GTK_DIALOG (bt->list_dialog), "_Cancel", 0);
    btn_act = gtk_dialog_add_button (GTK_DIALOG (bt->list_dialog), type == DIALOG_PAIR ? "_Pair" : "_Remove", 1);
    g_signal_connect (btn_act, "clicked", type == DIALOG_PAIR ? G_CALLBACK (handle_pair) : G_CALLBACK (handle_remove), bt);
    g_signal_connect (btn_cancel, "clicked", G_CALLBACK (handle_close_list_dialog), bt);
    //g_signal_connect (GTK_OBJECT (bt->list_dialog), "delete_event", G_CALLBACK (handle_close_list_dialog), bt);

    // add a label
    lbl = gtk_label_new (type == DIALOG_PAIR ? "Searching for Bluetooth devices..." : "Paired Bluetooth devices");
    gtk_misc_set_alignment (GTK_MISC (lbl), 0.0, 0.5);
    gtk_box_pack_start (GTK_BOX (gtk_dialog_get_content_area (GTK_DIALOG (bt->list_dialog))), lbl, TRUE, TRUE, 0);

    // add a scrolled window
    scrl = gtk_scrolled_window_new (NULL, NULL);
    gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (scrl), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_shadow_type (GTK_SCROLLED_WINDOW (scrl), GTK_SHADOW_IN);
    gtk_box_pack_start (GTK_BOX (gtk_dialog_get_content_area (GTK_DIALOG (bt->list_dialog))), scrl, TRUE, TRUE, 0);

    // create the list view and add to scrolled window
    bt->list = gtk_tree_view_new ();
    gtk_tree_view_set_fixed_height_mode (GTK_TREE_VIEW (bt->list), TRUE);
    gtk_tree_view_set_headers_visible (GTK_TREE_VIEW (bt->list), FALSE);
    gtk_widget_set_size_request (bt->list, -1, 150);
    rend = gtk_cell_renderer_pixbuf_new ();
    gtk_tree_view_insert_column_with_attributes (GTK_TREE_VIEW (bt->list), -1, "Icon", rend, "pixbuf", 5, NULL);
    gtk_tree_view_column_set_fixed_width (gtk_tree_view_get_column (GTK_TREE_VIEW (bt->list), 0), 50);
    rend = gtk_cell_renderer_text_new ();
    gtk_tree_view_insert_column_with_attributes (GTK_TREE_VIEW (bt->list), -1, "Name", rend, "text", 1, NULL);
    gtk_tree_view_column_set_fixed_width (gtk_tree_view_get_column (GTK_TREE_VIEW (bt->list), 1), 300);
    gtk_tree_view_set_model (GTK_TREE_VIEW (bt->list), type == DIALOG_PAIR ? GTK_TREE_MODEL (bt->unpair_list) : GTK_TREE_MODEL (bt->pair_list));
    gtk_container_add (GTK_CONTAINER (scrl), bt->list);

    // window ready
    gtk_widget_show_all (bt->list_dialog);
}

/* Functions to manage main menu */

static void handle_menu_add (GtkWidget *widget, gpointer user_data)
{
    BluetoothPlugin *bt = (BluetoothPlugin *) user_data;
    set_search (bt, TRUE);
    show_list_dialog (bt, DIALOG_PAIR);
}

static void handle_menu_remove (GtkWidget *widget, gpointer user_data)
{
    BluetoothPlugin *bt = (BluetoothPlugin *) user_data;
    show_list_dialog (bt, DIALOG_REMOVE);
}

static void handle_menu_connect (GtkWidget *widget, gpointer user_data)
{
    BluetoothPlugin *bt = (BluetoothPlugin *) user_data;
    gchar *name, *path;
    gboolean valid;

    GtkTreeIter iter;
    valid = gtk_tree_model_get_iter_first (GTK_TREE_MODEL (bt->pair_list), &iter);
    while (valid)
    {
        gtk_tree_model_get (GTK_TREE_MODEL (bt->pair_list), &iter, 0, &path, 1, &name, -1);
        if (!g_strcmp0 (gtk_widget_get_name (widget), path))
        {
            if (!is_connected (bt, path))
                connect_device (bt, path, TRUE);
            else
                connect_device (bt, path, FALSE);
            break;
        }
        g_free (name);
        g_free (path);
        valid = gtk_tree_model_iter_next (GTK_TREE_MODEL (bt->pair_list), &iter);    
    }
}

static void handle_menu_discover (GtkWidget *widget, gpointer user_data)
{
    BluetoothPlugin *bt = (BluetoothPlugin *) user_data;

    if (!is_discoverable (bt)) set_discoverable (bt, TRUE);
    else set_discoverable (bt, FALSE);
}

static gboolean add_to_menu (GtkTreeModel *model, GtkTreePath *tpath, GtkTreeIter *iter, gpointer user_data)
{
    BluetoothPlugin *bt = (BluetoothPlugin *) user_data;
    gchar *name, *path;
    GtkWidget *item, *submenu, *smi;

    gtk_tree_model_get (model, iter, 0, &path, 1, &name, -1);
    item = gtk_image_menu_item_new_with_label (name);

    // create a submenu for each paired device
    submenu = gtk_menu_new ();
    gtk_menu_item_set_submenu (GTK_MENU_ITEM (item), submenu);

    // create a single item for the submenu
    if (is_connected (bt, path))
    {
        GtkWidget *sel = gtk_image_new ();
        set_icon (bt->panel, sel, "dialog-ok-apply", 16);
        gtk_image_menu_item_set_image (GTK_IMAGE_MENU_ITEM (item), sel);
        smi = gtk_menu_item_new_with_label (_("Disconnect..."));
    }
    else smi = gtk_menu_item_new_with_label (_("Connect..."));

    // use the widget name of the menu item to store the unique path of the paired device
    gtk_widget_set_name (smi, path);

    // connect the connect toggle function and add the submenu item to the submenu
    g_signal_connect (smi, "activate", G_CALLBACK (handle_menu_connect), bt);
    gtk_menu_shell_append (GTK_MENU_SHELL (submenu), smi);

    // append the new item with submenu to the main menu
    gtk_menu_shell_append (GTK_MENU_SHELL (bt->menu), item);

    return FALSE;
}

/* Functions to manage device lists */

static void add_device (BluetoothPlugin *bt, GDBusObject *object, GtkListStore *lst)
{
    GVariant *var1, *var2, *var3, *var4, *var5;
    GtkTreeIter iter;
    GDBusInterface *interface;
    GdkPixbuf *icon;

    // add a new structure to the list store...
    gtk_list_store_append (lst, &iter);

    // ... and fill it with data about the object
    interface = g_dbus_object_get_interface (object, "org.bluez.Device1");
    var1 = g_dbus_proxy_get_cached_property (G_DBUS_PROXY (interface), "Alias");
    var2 = g_dbus_proxy_get_cached_property (G_DBUS_PROXY (interface), "Paired");
    var3 = g_dbus_proxy_get_cached_property (G_DBUS_PROXY (interface), "Connected");
    var4 = g_dbus_proxy_get_cached_property (G_DBUS_PROXY (interface), "Trusted");
    var5 = g_dbus_proxy_get_cached_property (G_DBUS_PROXY (interface), "Icon");
    icon = NULL;
    if (var5)
    {
        if (gtk_icon_theme_has_icon (panel_get_icon_theme (bt->panel), g_variant_get_string (var5, NULL)))
        {
            icon = gtk_icon_theme_load_icon (panel_get_icon_theme (bt->panel), g_variant_get_string (var5, NULL), 32, 0, NULL);
        }
    }
    if (!icon) icon = gtk_icon_theme_load_icon (panel_get_icon_theme (bt->panel), "dialog-question", 32, 0, NULL);
 
    gtk_list_store_set (lst, &iter, 0, g_dbus_object_get_object_path (object),
        1, var1 ? g_variant_get_string (var1, NULL) : "Unnamed device", 2, g_variant_get_boolean (var2),
        3, g_variant_get_boolean (var3), 4, g_variant_get_boolean (var4), 
        5, icon, -1);
}

static void update_device_list (BluetoothPlugin *bt)
{
    GDBusInterface *interface;
    GDBusObject *object;
    GList *objects, *interfaces;
    GVariant *var;
    gchar *path = NULL, *name;
    int item = 0, sel_item = -1;

    // save the current highlight if there is one
    if (bt->list) selected_path (bt, &path, &name);

    // clear out the list store
    gtk_list_store_clear (bt->pair_list);
    gtk_list_store_clear (bt->unpair_list);

    // iterate all the objects the manager knows about
    objects = g_dbus_object_manager_get_objects (bt->objmanager);
    while (objects != NULL)
    {
        object = (GDBusObject *) objects->data;
        interfaces = g_dbus_object_get_interfaces (object);
        while (interfaces != NULL)
        {
            // if an object has a Device1 interface, it is a Bluetooth device - add it to the list
            interface = G_DBUS_INTERFACE (interfaces->data);
            if (g_strcmp0 (g_dbus_proxy_get_interface_name (G_DBUS_PROXY (interface)), "org.bluez.Device1") == 0)
            {
                // ignore any devices which have no class
                if (!g_dbus_proxy_get_cached_property (G_DBUS_PROXY (interface), "Class")) break;
                var = g_dbus_proxy_get_cached_property (G_DBUS_PROXY (interface), "Paired");
                if (g_variant_get_boolean (var)) add_device (bt, object, bt->pair_list);
                else
                {
                    add_device (bt, object, bt->unpair_list);

                    // find the new location of the selected item
                    if (path && !g_strcmp0 (g_dbus_object_get_object_path (object), path)) sel_item = item;
                    item++;
                }
                break;
            }
            interfaces = interfaces->next;
        }
        objects = objects->next;
    }

    // replace the selection
    if (bt->list && sel_item != -1)
    {
        GtkTreePath *selpath = gtk_tree_path_new_from_indices (sel_item, -1);
        GtkTreeSelection *sel = gtk_tree_view_get_selection (GTK_TREE_VIEW (bt->list));
        gtk_tree_selection_select_path (sel, selpath);
        gtk_tree_path_free (selpath);
    }
}

static void set_icon (LXPanel *p, GtkWidget *image, const char *icon, int size)
{
    GdkPixbuf *pixbuf;
    if (size == 0) size = panel_get_icon_size (p) - ICON_BUTTON_TRIM;
    if (gtk_icon_theme_has_icon (panel_get_icon_theme (p), icon))
    {
        pixbuf = gtk_icon_theme_load_icon (panel_get_icon_theme (p), icon, size, 0, NULL);
        if (pixbuf != NULL)
        {
            gtk_image_set_from_pixbuf (GTK_IMAGE (image), pixbuf);
            g_object_unref (pixbuf);
            return;
        }
    }
    else
    {
        char path[256];
        sprintf (path, "%s/images/%s.png", PACKAGE_DATA_DIR, icon);
        pixbuf = gdk_pixbuf_new_from_file_at_scale (path, size, size, TRUE, NULL);
        if (pixbuf != NULL)
        {
            gtk_image_set_from_pixbuf (GTK_IMAGE (image), pixbuf);
            g_object_unref (pixbuf);
        }
    }
}

static void menu_popup_set_position (GtkMenu *menu, gint *px, gint *py, gboolean *push_in, gpointer data)
{
    BluetoothPlugin *bt = (BluetoothPlugin *) data;

    /* Determine the coordinates */
    lxpanel_plugin_popup_set_position_helper (bt->panel, bt->plugin, GTK_WIDGET(menu), px, py);
    *push_in = TRUE;
}

static void show_menu (BluetoothPlugin *bt)
{
    GtkWidget *item, *sel = gtk_image_new ();
    GtkTreeIter iter;
    
    bt->menu = gtk_menu_new ();

    if (bt->adapter == NULL)
    {
        // warn if no BT hardware detected
        item = gtk_image_menu_item_new_with_label (_("No Bluetooth adapter found"));
        gtk_widget_set_sensitive (item, FALSE);
        gtk_menu_shell_append (GTK_MENU_SHELL (bt->menu), item);
    }
    else
    {
        // discoverable toggle
        item = gtk_image_menu_item_new_with_label (_("Discoverable"));
        if (is_discoverable (bt))
        {
            set_icon (bt->panel, sel, "dialog-ok-apply", 16);
            gtk_image_menu_item_set_image (GTK_IMAGE_MENU_ITEM(item), sel);
        }
        g_signal_connect (item, "activate", G_CALLBACK (handle_menu_discover), bt);
        gtk_menu_shell_append (GTK_MENU_SHELL (bt->menu), item);
        item = gtk_separator_menu_item_new ();
        gtk_menu_shell_append (GTK_MENU_SHELL (bt->menu), item);

        // add and remove dialogs
        item = gtk_image_menu_item_new_with_label (_("Add Device..."));
        g_signal_connect (item, "activate", G_CALLBACK (handle_menu_add), bt);
        gtk_menu_shell_append (GTK_MENU_SHELL (bt->menu), item);
        item = gtk_image_menu_item_new_with_label (_("Remove Device..."));
        g_signal_connect (item, "activate", G_CALLBACK (handle_menu_remove), bt);
        gtk_menu_shell_append (GTK_MENU_SHELL (bt->menu), item);

        // paired devices
        if (gtk_tree_model_get_iter_first (GTK_TREE_MODEL (bt->pair_list), &iter))
        {
            item = gtk_separator_menu_item_new ();
            gtk_menu_shell_append (GTK_MENU_SHELL (bt->menu), item);

            gtk_tree_model_foreach (GTK_TREE_MODEL (bt->pair_list), add_to_menu, bt);
        }
    }

    gtk_widget_show_all (bt->menu);
    gtk_menu_popup (GTK_MENU (bt->menu), NULL, NULL, menu_popup_set_position, bt, 1, gtk_get_current_event_time ());
}

/* Handler for menu button click */
static gboolean bluetooth_button_press_event (GtkWidget *widget, GdkEventButton *event, LXPanel *panel)
{
    BluetoothPlugin *bt = lxpanel_plugin_get_data (widget);

    /* Show or hide the popup menu on left-click */
    if (event->button == 1)
    {
        show_menu (bt);
        return TRUE;
    }
    else return FALSE;
}

/* Handler for system config changed message from panel */
static void bluetooth_configuration_changed (LXPanel *panel, GtkWidget *widget)
{
    BluetoothPlugin *bt = lxpanel_plugin_get_data (widget);

    set_icon (panel, bt->tray_icon, "preferences-system-bluetooth", 0);
}

/* Plugin destructor. */
static void bluetooth_destructor (gpointer user_data)
{
    BluetoothPlugin * bt = (BluetoothPlugin *) user_data;

    /* Deallocate memory */
    g_free (bt);
}

/* Plugin constructor. */
static GtkWidget *bluetooth_constructor (LXPanel *panel, config_setting_t *settings)
{
    /* Allocate and initialize plugin context */
    BluetoothPlugin *bt = g_new0 (BluetoothPlugin, 1);

    setlocale (LC_ALL, "");
    bindtextdomain (PACKAGE, NULL);
    bind_textdomain_codeset (PACKAGE, "UTF-8");
    textdomain (PACKAGE);

    bt->tray_icon = gtk_image_new ();
    //set_icon (panel, bt->tray_icon, "preferences-desktop-accessibility", 0);
    set_icon (panel, bt->tray_icon, "preferences-system-bluetooth", 0);
    gtk_widget_set_tooltip_text (bt->tray_icon, _("Manage Bluetooth devices"));
    gtk_widget_set_visible (bt->tray_icon, TRUE);

    /* Allocate top level widget and set into Plugin widget pointer. */
    bt->panel = panel;
    bt->plugin = gtk_button_new ();
    gtk_button_set_relief (GTK_BUTTON (bt->plugin), GTK_RELIEF_NONE);
    g_signal_connect (bt->plugin, "button-press-event", G_CALLBACK(bluetooth_button_press_event), NULL);
    bt->settings = settings;
    lxpanel_plugin_set_data (bt->plugin, bt, bluetooth_destructor);
    gtk_widget_add_events (bt->plugin, GDK_BUTTON_PRESS_MASK);

    /* Allocate icon as a child of top level */
    gtk_container_add (GTK_CONTAINER (bt->plugin), bt->tray_icon);
    
    /* Show the widget */
    gtk_widget_show_all (bt->plugin);
    
    /* Initialise plugin data */
    bt->pair_list = gtk_list_store_new (6, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_UINT, G_TYPE_UINT, G_TYPE_UINT, GDK_TYPE_PIXBUF);
    bt->unpair_list = gtk_list_store_new (6, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_UINT, G_TYPE_UINT, G_TYPE_UINT, GDK_TYPE_PIXBUF);
    bt->ok_instance = 0;
    bt->cancel_instance = 0;
    clear (bt);
    
    // set up callbacks to see if BlueZ is on DBus
    g_bus_watch_name (G_BUS_TYPE_SYSTEM, "org.bluez", 0, cb_name_owned, cb_name_unowned, bt, NULL);

    return bt->plugin;
}

FM_DEFINE_MODULE(lxpanel_gtk, bluetooth)

/* Plugin descriptor. */
LXPanelPluginInit fm_module_init_lxpanel_gtk = {
    .name = N_("Bluetooth"),
    .description = N_("Manages Bluetooth devices"),
    .new_instance = bluetooth_constructor,
    .reconfigure = bluetooth_configuration_changed,
    .button_press_event = bluetooth_button_press_event
};
