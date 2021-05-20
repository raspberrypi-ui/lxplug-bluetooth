/*
Copyright (c) 2018 Raspberry Pi (Trading) Ltd.
All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:
    * Redistributions of source code must retain the above copyright
      notice, this list of conditions and the following disclaimer.
    * Redistributions in binary form must reproduce the above copyright
      notice, this list of conditions and the following disclaimer in the
      documentation and/or other materials provided with the distribution.
    * Neither the name of the copyright holder nor the
      names of its contributors may be used to endorse or promote products
      derived from this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY
DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
(INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif
#include <errno.h>
#include <locale.h>
#include <stdlib.h>
#include <string.h>
#include <glib/gi18n.h>

#include <gio/gio.h>

#include "plugin.h"

#define DEBUG_ON
#ifdef DEBUG_ON
#define DEBUG(fmt,args...) if(getenv("DEBUG_BT"))g_message("bt: " fmt,##args)
#define DEBUG_VAR(fmt,var,args...) if(getenv("DEBUG_BT")){gchar*vp=g_variant_print(var,TRUE);g_message("bt: " fmt,##args,vp);g_free(vp);}
#else
#define DEBUG
#define DEBUG_VAR
#endif

/* Name table for cached icons */

#define ICON_CACHE_SIZE 13

const gchar *icon_names[ICON_CACHE_SIZE] =
{
    "audio-card",
    "computer",
    "gnome-dev-computer",
    "gnome-fs-client",
    "input-keyboard",
    "input-mouse",
    "keyboard",
    "media-removable",
    "mouse",
    "phone",
    "stock_cell-phone",
    "system",
    "dialog-question"
};

/* Plug-in global data */

typedef struct {
    GtkWidget *plugin;              /* Back pointer to the widget */
    LXPanel *panel;                 /* Back pointer to panel */
    GtkWidget *tray_icon;           /* Displayed image */
    config_setting_t *settings;     /* Plugin settings */
    GtkWidget *menu;                /* Popup menu */
    GtkListStore *pair_list;
    GtkListStore *unpair_list;
    GtkTreeModelFilter *filter_list; /* Filtered device list used in connect dialog */
    GtkTreeModelSort *sorted_list;  /* Sorted device list used in connect dialog */
    gchar *selection;               /* Connect dialog selected item */
    GDBusConnection *busconnection;
    GDBusObjectManager *objmanager;
    GDBusProxy *agentmanager;
    GDBusProxy *adapter;
    guint agentobj;
    gchar *pairing_object;
    gchar *device_name;
    gchar *device_path;
    const gchar *incoming_object;
    GtkWidget *list_dialog, *list, *list_ok;
    GtkWidget *pair_dialog, *pair_label, *pair_entry, *pair_ok, *pair_cancel;
    GtkWidget *conn_dialog, *conn_label, *conn_ok, *conn_cancel;
    GtkEntryBuffer *pinbuf;
    GDBusMethodInvocation *invocation;
    gulong ok_instance;
    gulong cancel_instance;
    guint flash_timer;
    guint flash_state;
    GdkPixbuf *icon_ref[ICON_CACHE_SIZE];
} BluetoothPlugin;

typedef enum {
    DIALOG_PAIR,
    DIALOG_REMOVE,
    DIALOG_CONNECT,
    DIALOG_DISCONNECT
} DIALOG_TYPE;

typedef enum {
    STATE_PAIR_INIT,
    STATE_PAIR_REQUEST,
    STATE_REQUEST_PIN,
    STATE_REQUEST_PASS,
    STATE_DISPLAY_PIN,
    STATE_CONFIRM_PIN,
    STATE_WAITING,
    STATE_PAIRED,
    STATE_PAIR_FAIL,
    STATE_CONNECTED,
    STATE_CONNECT_FAIL,
    STATE_REMOVING,
    STATE_REMOVE_FAIL,
    STATE_PAIRED_AUDIO,
    STATE_PAIRED_UNUSABLE
} PAIR_STATE;

typedef enum {
    STATE_CONFIRM,
    STATE_CONFIRMED,
    STATE_INIT,
    STATE_FAIL
} CONN_STATE;

typedef enum {
    DEV_HID,
    DEV_HID_LE,
    DEV_AUDIO_SINK,
    DEV_OTHER
} DEVICE_TYPE;

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
"       <method name=\"RequestAuthorization\">\n"
"           <arg name=\"device\" type=\"o\" direction=\"in\"/>\n"
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
static DEVICE_TYPE check_uuids (BluetoothPlugin *bt, const gchar *path);

static void handle_pin_entered (GtkButton *button, gpointer user_data);
static void handle_pass_entered (GtkButton *button, gpointer user_data);
static void handle_pin_confirmed (GtkButton *button, gpointer user_data);
static void handle_pin_rejected (GtkButton *button, gpointer user_data);
static void handle_authorize_yes (GtkButton *button, gpointer user_data);
static void handle_authorize_no (GtkButton *button, gpointer user_data);
static void handle_cancel_pair (GtkButton *button, gpointer user_data);
static void handle_close_pair_dialog (GtkButton *button, gpointer user_data);
static gint delete_pair (GtkWidget *widget, GdkEvent *event, gpointer user_data);
static void handle_reject_pair (GtkButton *button, gpointer user_data);
static void handle_accept_pair (GtkButton *button, gpointer user_data);
static void connect_ok (BluetoothPlugin *bt, void (*cb) (void));
static void connect_cancel (BluetoothPlugin *bt, void (*cb) (void));
static void show_pairing_dialog (BluetoothPlugin *bt, PAIR_STATE state, const gchar *device, const gchar *param);
static gboolean selected_path (BluetoothPlugin *bt, char **path, char **name);
static void handle_pair (GtkButton *button, gpointer user_data);
static void handle_remove (GtkButton *button, gpointer user_data);
static void handle_close_list_dialog (GtkButton *button, gpointer user_data);
static gint delete_list (GtkWidget *widget, GdkEvent *event, gpointer user_data);
static gboolean filter_unknowns (GtkTreeModel *model, GtkTreeIter *iter, gpointer data);
static void sel_changed (GtkTreeSelection *sel, gpointer user_data);
static void show_list_dialog (BluetoothPlugin * bt, DIALOG_TYPE type);
static void show_connect_dialog (BluetoothPlugin *bt, DIALOG_TYPE type, CONN_STATE state, const gchar *param);
static void handle_close_connect_dialog (GtkButton *button, gpointer user_data);
static gint delete_conn (GtkWidget *widget, GdkEvent *event, gpointer user_data);
static void handle_menu_add (GtkWidget *widget, gpointer user_data);
static void handle_menu_remove (GtkWidget *widget, gpointer user_data);
static void handle_menu_connect (GtkWidget *widget, gpointer user_data);
static gboolean flash_icon (gpointer user_data);
static void handle_menu_discover (GtkWidget *widget, gpointer user_data);
static gboolean add_to_menu (GtkTreeModel *model, GtkTreePath *tpath, GtkTreeIter *iter, gpointer user_data);
static void add_device (BluetoothPlugin *bt, GDBusObject *object, GtkListStore *lst);
static void update_device_list (BluetoothPlugin *bt);
static void init_icon_cache (BluetoothPlugin *bt);
static GdkPixbuf *icon_from_cache (BluetoothPlugin *bt, const gchar *icon_name);
static void menu_popup_set_position (GtkMenu *menu, gint *px, gint *py, gboolean *push_in, gpointer data);
static void show_menu (BluetoothPlugin *bt);
static void update_icon (BluetoothPlugin *bt);

/*---------------------------------------------------------------------------*/
/* Function Definitions */
/*---------------------------------------------------------------------------*/

static int bt_enabled (void)
{
    FILE *fp;

    // is rfkill installed?
    fp = popen ("test -e /usr/sbin/rfkill", "r");
    if (pclose (fp)) return -2;

    // is there BT hardware that rfkill can see?
    fp = popen ("/usr/sbin/rfkill list bluetooth | grep -q blocked", "r");
    if (pclose (fp)) return -1;

    // is rfkill blocking BT?
    fp = popen ("/usr/sbin/rfkill list bluetooth | grep -q 'Soft blocked: no'", "r");
    if (!pclose (fp)) return 1;
    return 0;
}

static void toggle_bt (GtkWidget *widget, gpointer user_data)
{
    BluetoothPlugin *bt = (BluetoothPlugin *) user_data;

    if (bt_enabled ())
    {
        system ("/usr/sbin/rfkill block bluetooth");
        if (bt->flash_timer) g_source_remove (bt->flash_timer);
        bt->flash_timer = 0;
        lxpanel_plugin_set_taskbar_icon (bt->panel, bt->tray_icon, "preferences-system-bluetooth-inactive");
    }
    else
    {
        system ("/usr/sbin/rfkill unblock bluetooth");
        lxpanel_plugin_set_taskbar_icon (bt->panel, bt->tray_icon, "preferences-system-bluetooth");
    }
}

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
    if (error)
    {
        DEBUG ("Error getting object manager - %s", error->message);
        g_error_free (error);
    }
    else
    {
        // register callbacks on object manager
        g_signal_connect (bt->objmanager, "interface-added", G_CALLBACK (cb_interface_added), bt);
        g_signal_connect (bt->objmanager, "interface-removed", G_CALLBACK (cb_interface_removed), bt);
        g_signal_connect (bt->objmanager, "object-added", G_CALLBACK (cb_object_added), bt);
        g_signal_connect (bt->objmanager, "object-removed", G_CALLBACK (cb_object_removed), bt);
        g_signal_connect (bt->objmanager, "interface-proxy-signal", G_CALLBACK (cb_interface_signal), bt);
        g_signal_connect (bt->objmanager, "interface-proxy-properties-changed", G_CALLBACK (cb_interface_properties), bt);
    }

    // get a connection to the system DBus
    error = NULL;
    bt->busconnection = g_bus_get_sync (G_BUS_TYPE_SYSTEM, NULL, &error);
    if (error)
    {
        DEBUG ("Error getting system bus - %s", error->message);
        g_error_free (error);
    }

    // create the agent from XML spec
    error = NULL;
    introspection_data = g_dbus_node_info_new_for_xml (introspection_xml, &error);
    if (error)
    {
        DEBUG ("Error creating agent node - %s", error->message);
        g_error_free (error);
    }

    // register the agent on the system bus
    error = NULL;
    bt->agentobj = g_dbus_connection_register_object (bt->busconnection, "/btagent", introspection_data->interfaces[0], &interface_vtable, bt, NULL, &error);
    if (error)
    {
        DEBUG ("Error registering agent on bus - %s", error->message);
        g_error_free (error);
    }

    // query the DBus for an agent manager and a Bluetooth adapter
    find_hardware (bt);

    // clean up
    g_dbus_node_info_unref (introspection_data);
}

/* Clear all the BlueZ data if the DBus connection is lost */

static void clear (BluetoothPlugin *bt)
{
    if (bt->objmanager) g_object_unref (bt->objmanager);
    bt->objmanager = NULL;
    if (bt->busconnection)
    {
        if (bt->agentobj) g_dbus_connection_unregister_object (bt->busconnection, bt->agentobj);
        g_object_unref (bt->busconnection);
    }
    bt->busconnection = NULL;
    bt->agentobj = 0;
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
    GList *objects, *interfaces, *obj_elem, *if_elem;
    GDBusProxy *newagentmanager = NULL, *newadapter = NULL;
    GError *error;
    GVariant *res, *arg;
    int bt_state;

    // if there's no object manager, you won't find anything, and it'll crash...
    if (!bt->objmanager) return;

    objects = g_dbus_object_manager_get_objects (bt->objmanager);

    for (obj_elem = objects; obj_elem != NULL; obj_elem = obj_elem->next)
    {
        object = (GDBusObject*) obj_elem->data;

        interfaces = g_dbus_object_get_interfaces (object);
        for (if_elem = interfaces; if_elem != NULL; if_elem = if_elem->next)
        {
            interface = G_DBUS_INTERFACE (if_elem->data);
            if (g_strcmp0 (g_dbus_proxy_get_interface_name (G_DBUS_PROXY (interface)), "org.bluez.Adapter1") == 0)
            {
                if (newadapter == NULL) newadapter = G_DBUS_PROXY (interface);
                else DEBUG ("Multiple adapters found");
            }
            else if (g_strcmp0 (g_dbus_proxy_get_interface_name (G_DBUS_PROXY (interface)), "org.bluez.AgentManager1") == 0)
            {
                if (newagentmanager == NULL) newagentmanager = G_DBUS_PROXY (interface);
                else DEBUG ("Multiple agent managers found");
            }
        }
        g_list_free_full (interfaces, g_object_unref);
    }
    g_list_free_full (objects, g_object_unref);

    if (!newagentmanager)
    {
        DEBUG ("No agent manager found");
        if (bt->agentmanager) g_object_unref (bt->agentmanager);
        bt->agentmanager = NULL;
    }
    else if (newagentmanager != bt->agentmanager)
    {
        DEBUG ("New agent manager found");
        // if there is already an agent manager, unregister the agent from it
        if (bt->agentmanager)
        {
            error = NULL;
            arg = g_variant_new ("(o)", "/btagent");
            g_variant_ref_sink (arg);
            res = g_dbus_proxy_call_sync (bt->agentmanager, "UnregisterAgent", arg, G_DBUS_CALL_FLAGS_NONE, -1, NULL, &error);
            if (error)
            {
                DEBUG ("Error unregistering agent with manager - %s", error->message);
                g_error_free (error);
            }
            if (res) g_variant_unref (res);
            g_variant_unref (arg);
            g_object_unref (bt->agentmanager);
        }

        bt->agentmanager = newagentmanager;

        // register the agent with the new agent manager
        if (bt->agentmanager)
        {
            error = NULL;
            arg = g_variant_new ("(os)", "/btagent", "DisplayYesNo");
            g_variant_ref_sink (arg);
            res = g_dbus_proxy_call_sync (bt->agentmanager, "RegisterAgent", arg, G_DBUS_CALL_FLAGS_NONE, -1, NULL, &error);
            if (error)
            {
                DEBUG ("Error registering agent with manager - %s", error->message);
                g_error_free (error);
            }
            g_variant_unref (arg);
            if (res) g_variant_unref (res);
        }
    }

    if (!newadapter)
    {
        DEBUG ("No adapter found");
        if (bt->adapter) g_object_unref (bt->adapter);
        bt->adapter = NULL;
    }
    else if (newadapter != bt->adapter)
    {
        DEBUG ("New adapter found");
        if (bt->adapter) g_object_unref (bt->adapter);
        bt->adapter = newadapter;
    }

    // update the plugin icon
    update_icon (bt);

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
    GVariant *var, *varc0, *varc1;
    gchar buffer[16];

    DEBUG ("Agent method %s called", method_name);
    if (parameters) DEBUG_VAR ("with parameters %s", parameters);

    if (g_strcmp0 (method_name, "Cancel") == 0) return;

    bt->invocation = invocation;
    varc0 = g_variant_get_child_value (parameters, 0);
    interface = g_dbus_object_manager_get_interface (bt->objmanager, g_variant_get_string (varc0, NULL), "org.bluez.Device1");
    var = g_dbus_proxy_get_cached_property (G_DBUS_PROXY (interface), "Alias");

    if (g_strcmp0 (method_name, "RequestConfirmation") == 0)
    {
        varc1 = g_variant_get_child_value (parameters, 1);
        sprintf (buffer, "%06lu", g_variant_get_uint32 (varc1));
        show_pairing_dialog (bt, STATE_CONFIRM_PIN, g_variant_get_string (var, NULL), buffer);
        g_variant_unref (varc1);
    }
    else if (g_strcmp0 (method_name, "DisplayPinCode") == 0)
    {
        varc1 = g_variant_get_child_value (parameters, 1);
        show_pairing_dialog (bt, STATE_DISPLAY_PIN, g_variant_get_string (var, NULL), g_variant_get_string (varc1, NULL));
        g_dbus_method_invocation_return_value (invocation, NULL);
        g_variant_unref (varc1);
    }
    else if (g_strcmp0 (method_name, "DisplayPasskey") == 0)   // !!!! do we need to do something with "entered" parameter here?
    {
        varc1 = g_variant_get_child_value (parameters, 1);
        sprintf (buffer, "%lu", g_variant_get_uint32 (varc1));
        show_pairing_dialog (bt, STATE_DISPLAY_PIN, g_variant_get_string (var, NULL), buffer);
        g_dbus_method_invocation_return_value (invocation, NULL);
        g_variant_unref (varc1);
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
        show_pairing_dialog (bt, STATE_PAIR_REQUEST, g_variant_get_string (var, NULL), NULL);
    }
    g_variant_unref (varc0);
    g_variant_unref (var);
    g_object_unref (interface);
}

/* Bus watcher callbacks - information only at present; may want to link the general initialisation to these */

static void cb_name_owned (GDBusConnection *connection, const gchar *name, const gchar *owner, gpointer user_data)
{
    BluetoothPlugin * bt = (BluetoothPlugin *) user_data;
    DEBUG ("Name %s owned on DBus", name);
    initialise (bt);
}

static void cb_name_unowned (GDBusConnection *connection, const gchar *name, gpointer user_data)
{
    BluetoothPlugin * bt = (BluetoothPlugin *) user_data;
    DEBUG ("Name %s unowned on DBus", name);
    clear (bt);
}

/* Object manager callbacks - we only really care about objects added, removed or with changed properties */

static void cb_interface_added (GDBusObjectManager *manager, GDBusObject *object, GDBusInterface *interface, gpointer user_data)
{
    DEBUG ("Object manager - interface %s added", g_dbus_proxy_get_interface_name (G_DBUS_PROXY (interface)));
}

static void cb_interface_removed (GDBusObjectManager *manager, GDBusObject *object, GDBusInterface *interface, gpointer user_data)
{
    DEBUG ("Object manager - interface %s removed", g_dbus_proxy_get_interface_name (G_DBUS_PROXY (interface)));
}

static void cb_object_added (GDBusObjectManager *manager, GDBusObject *object, gpointer user_data)
{
    BluetoothPlugin * bt = (BluetoothPlugin *) user_data;
    DEBUG ("Object manager - object added at path %s", g_dbus_object_get_object_path (object));
    find_hardware (bt);
}

static void cb_object_removed (GDBusObjectManager *manager, GDBusObject *object, gpointer user_data)
{
    BluetoothPlugin * bt = (BluetoothPlugin *) user_data;
    DEBUG ("Object manager - object removed at path %s", g_dbus_object_get_object_path (object));
    find_hardware (bt);
}

static void cb_interface_signal (GDBusObjectManagerClient *manager, GDBusObjectProxy *object_proxy, GDBusProxy *proxy, gchar *sender, gchar *signal, GVariant *parameters, gpointer user_data)
{
    DEBUG_VAR ("Object manager - object at %s interface signal %s %s %s", parameters, g_dbus_proxy_get_object_path (proxy), sender, signal);
}

static void cb_interface_properties (GDBusObjectManagerClient *manager, GDBusObjectProxy *object_proxy, GDBusProxy *proxy, GVariant *parameters, GStrv inval, gpointer user_data)
{
    BluetoothPlugin * bt = (BluetoothPlugin *) user_data;
    GVariant *var, *var_m, *var_u, *var_a;

    DEBUG_VAR ("Object manager - object at %s property changed - %s %s", parameters, g_dbus_proxy_get_object_path (proxy), g_dbus_proxy_get_interface_name (proxy));
    update_device_list (bt);

    // hack to reconnect after successful pairing
    var = g_variant_lookup_value (parameters, "Connected", NULL);
    if (var)
    {
        if (g_variant_get_boolean (var) == FALSE)
        {
            if (g_strcmp0 (g_dbus_proxy_get_object_path (proxy), bt->pairing_object) == 0)
            {
                DEBUG ("Paired object disconnected - reconnecting");

                // if this is not an audio device, connect to it
                if (check_uuids (bt, bt->pairing_object) == DEV_HID)
                    connect_device (bt, bt->pairing_object, TRUE);
                g_free (bt->pairing_object);
                bt->pairing_object = NULL;
            }
        }
        g_variant_unref (var);
    }
    // hack to accept incoming pairing
    var = g_variant_lookup_value (parameters, "Paired", NULL);
    if (var)
    {
        if (g_variant_get_boolean (var) == TRUE)
        {
            var_m = g_variant_lookup_value (parameters, "Modalias", NULL);
            var_u = g_variant_lookup_value (parameters, "UUIDs", NULL);
            if (var_m && var_u && bt->pairing_object == NULL)
            {
                DEBUG ("New pairing detected");
                var_a = g_dbus_proxy_get_cached_property (proxy, "Alias");
                bt->incoming_object = g_dbus_proxy_get_object_path (proxy);
                show_pairing_dialog (bt, STATE_PAIR_REQUEST, g_variant_get_string (var_a, NULL), NULL);
                if (var_a) g_variant_unref (var_a);
            }
            if (var_m) g_variant_unref (var_m);
            if (var_u) g_variant_unref (var_u);
        }
        g_variant_unref (var);
    }
}

/* Searching */

static gboolean is_searching (BluetoothPlugin *bt)
{
    GVariant *var = g_dbus_proxy_get_cached_property (G_DBUS_PROXY (bt->adapter), "Discovering");
    gboolean res = g_variant_get_boolean (var);
    g_variant_unref (var);
    return res;
}

static void set_search (BluetoothPlugin *bt, gboolean state)
{
    if (state)
    {
        DEBUG ("Starting search");
        g_dbus_proxy_call (bt->adapter, "StartDiscovery", NULL, G_DBUS_CALL_FLAGS_NONE, -1, NULL, cb_search_start, bt);
    }
    else
    {
        DEBUG ("Ending search");
        g_dbus_proxy_call (bt->adapter, "StopDiscovery", NULL, G_DBUS_CALL_FLAGS_NONE, -1, NULL, cb_search_end, bt);
    }
}

static void cb_search_start (GObject *source, GAsyncResult *res, gpointer user_data)
{
    GError *error = NULL;
    GVariant *var = g_dbus_proxy_call_finish (G_DBUS_PROXY (source), res, &error);

    if (error)
    {
        DEBUG ("Search start - error %s", error->message);
        g_error_free (error);
    }
    else DEBUG_VAR ("Search start - result %s", var);
    if (var) g_variant_unref (var);
}

static void cb_search_end (GObject *source, GAsyncResult *res, gpointer user_data)
{
    GError *error = NULL;
    GVariant *var= g_dbus_proxy_call_finish (G_DBUS_PROXY (source), res, &error);

    if (error)
    {
        DEBUG ("Search end - error %s", error->message);
        g_error_free (error);
    }
    else DEBUG_VAR ("Search end - result %s", var);
    if (var) g_variant_unref (var);
}

/* Discoverable */

static gboolean is_discoverable (BluetoothPlugin *bt)
{
    GVariant *var = g_dbus_proxy_get_cached_property (G_DBUS_PROXY (bt->adapter), "Discoverable");
    gboolean res = g_variant_get_boolean (var);
    g_variant_unref (var);
    return res;
}

static void set_discoverable (BluetoothPlugin *bt, gboolean state)
{
    GVariant *vbool = g_variant_new_boolean (state);
    g_variant_ref_sink (vbool);
    GVariant *var = g_variant_new ("(ssv)", g_dbus_proxy_get_interface_name (G_DBUS_PROXY (bt->adapter)), "Discoverable", vbool);
    g_variant_ref_sink (var);

    if (state)
    {
        DEBUG ("Making discoverable");
        g_dbus_proxy_call (bt->adapter, "org.freedesktop.DBus.Properties.Set", var, G_DBUS_CALL_FLAGS_NONE, -1, NULL, cb_discover_start, bt);
    }
    else
    {
        DEBUG ("Stopping discoverable");
        g_dbus_proxy_call (bt->adapter, "org.freedesktop.DBus.Properties.Set", var, G_DBUS_CALL_FLAGS_NONE, -1, NULL, cb_discover_end, bt);
    }
    g_variant_unref (vbool);
    g_variant_unref (var);
}

static void cb_discover_start (GObject *source, GAsyncResult *res, gpointer user_data)
{
    BluetoothPlugin * bt = (BluetoothPlugin *) user_data;
    GError *error = NULL;
    GVariant *var = g_dbus_proxy_call_finish (G_DBUS_PROXY (source), res, &error);
    
    if (error)
    {
        DEBUG ("Discoverable start - error %s", error->message);
        g_error_free (error);
    }
    else
    {
        DEBUG_VAR ("Discoverable start - result %s", var);
        if (bt->flash_timer) g_source_remove (bt->flash_timer);
        bt->flash_timer = g_timeout_add (500, flash_icon, bt);
    }
    if (var) g_variant_unref (var);
}

static void cb_discover_end (GObject *source, GAsyncResult *res, gpointer user_data)
{
    BluetoothPlugin * bt = (BluetoothPlugin *) user_data;
    GError *error = NULL;
    GVariant *var = g_dbus_proxy_call_finish (G_DBUS_PROXY (source), res, &error);
    
    if (error)
    {
        DEBUG ("Discoverable end - error %s", error->message);
        g_error_free (error);
    }
    else
    {
        DEBUG_VAR ("Discoverable end - result %s", var);
        if (bt->flash_timer) g_source_remove (bt->flash_timer);
        bt->flash_timer = 0;
        lxpanel_plugin_set_taskbar_icon (bt->panel, bt->tray_icon, "preferences-system-bluetooth");
    }
    if (var) g_variant_unref (var);
}

/* Pair device / cancel pairing */

static gboolean is_paired (BluetoothPlugin *bt, const gchar *path)
{
    GDBusInterface *interface = g_dbus_object_manager_get_interface (bt->objmanager, path, "org.bluez.Device1");
    GVariant *var = g_dbus_proxy_get_cached_property (G_DBUS_PROXY (interface), "Paired");
    gboolean res = g_variant_get_boolean (var);
    g_variant_unref (var);
    g_object_unref (interface);
    return res;
}

static void pair_device (BluetoothPlugin *bt, const gchar *path, gboolean state)
{
    GDBusInterface *interface = g_dbus_object_manager_get_interface (bt->objmanager, path, "org.bluez.Device1");

    if (state)
    {
        DEBUG ("Pairing with %s", path);
        g_dbus_proxy_call (G_DBUS_PROXY (interface), "Pair", NULL, G_DBUS_CALL_FLAGS_NONE, -1, NULL, cb_paired, bt);
    }
    else
    {
        DEBUG ("CancelPairing with %s", path);
        g_dbus_proxy_call (G_DBUS_PROXY (interface), "CancelPairing", NULL, G_DBUS_CALL_FLAGS_NONE, -1, NULL, cb_cancelled, bt);
    }
    g_object_unref (interface);
}

static void cb_paired (GObject *source, GAsyncResult *res, gpointer user_data)
{
    BluetoothPlugin * bt = (BluetoothPlugin *) user_data;
    GError *error = NULL;
    GVariant *var = g_dbus_proxy_call_finish (G_DBUS_PROXY (source), res, &error);
    DEVICE_TYPE dev;

    if (error)
    {
        DEBUG ("Pairing error %s", error->message);
        show_pairing_dialog (bt, STATE_PAIR_FAIL, NULL, error->message);
        if (bt->pairing_object)
        {
            g_free (bt->pairing_object);
            bt->pairing_object = NULL;
        }
        g_error_free (error);
    }
    else
    {
        DEBUG_VAR ("Pairing result %s", var);

        // check services available
        dev = check_uuids (bt, bt->pairing_object);
        if (dev == DEV_HID)
        {
            show_pairing_dialog (bt, STATE_PAIRED, NULL, NULL);
        }
        else if (dev == DEV_HID_LE)
        {
            show_pairing_dialog (bt, STATE_CONNECTED, NULL, NULL);
        }
        else if (dev == DEV_AUDIO_SINK)
        {
            show_pairing_dialog (bt, STATE_PAIRED_AUDIO, NULL, NULL);
            trust_device (bt, bt->pairing_object, TRUE);
            g_free (bt->pairing_object);
            bt->pairing_object = NULL;
        }
        else
        {
            show_pairing_dialog (bt, STATE_PAIRED_UNUSABLE, NULL, NULL);
            trust_device (bt, bt->pairing_object, TRUE);
            g_free (bt->pairing_object);
            bt->pairing_object = NULL;
        }
    }
    if (var) g_variant_unref (var);
}

static void cb_cancelled (GObject *source, GAsyncResult *res, gpointer user_data)
{
    GError *error = NULL;
    GVariant *var = g_dbus_proxy_call_finish (G_DBUS_PROXY (source), res, &error);

    if (error)
    {
        DEBUG ("Cancelling error %s", error->message);
        g_error_free (error);
    }
    else DEBUG_VAR ("Cancelling result %s", var);
    if (var) g_variant_unref (var);
}

/* Trust / distrust device */

static gboolean is_trusted (BluetoothPlugin *bt, const gchar *path)
{
    GDBusInterface *interface = g_dbus_object_manager_get_interface (bt->objmanager, path, "org.bluez.Device1");
    GVariant *var = g_dbus_proxy_get_cached_property (G_DBUS_PROXY (interface), "Trusted");
    gboolean res = g_variant_get_boolean (var);
    g_variant_unref (var);
    g_object_unref (interface);
    return res;
}

static void trust_device (BluetoothPlugin *bt, const gchar *path, gboolean state)
{
    GDBusInterface *interface = g_dbus_object_manager_get_interface (bt->objmanager, path, "org.bluez.Device1");
    GVariant *vbool = g_variant_new_boolean (state);
    g_variant_ref_sink (vbool);
    GVariant *var = g_variant_new ("(ssv)", g_dbus_proxy_get_interface_name (G_DBUS_PROXY (interface)), "Trusted", vbool);
    g_variant_ref_sink (var);

    if (state) DEBUG ("Trusting %s", path);
    else DEBUG ("Distrusting %s", path);
    g_dbus_proxy_call (G_DBUS_PROXY (interface), "org.freedesktop.DBus.Properties.Set", var, G_DBUS_CALL_FLAGS_NONE, -1, NULL, cb_trusted, bt);
    g_variant_unref (var);
    g_variant_unref (vbool);
    g_object_unref (interface);
}

static void cb_trusted (GObject *source, GAsyncResult *res, gpointer user_data)
{
    GError *error = NULL;
    GVariant *var = g_dbus_proxy_call_finish (G_DBUS_PROXY (source), res, &error);

    if (error)
    {
        DEBUG ("Trusting error %s", error->message);
        g_error_free (error);
    }
    else DEBUG_VAR ("Trusting result %s", var);
    if (var) g_variant_unref (var);
}

/* Connect / disconnect device */

static gboolean is_connected (BluetoothPlugin *bt, const gchar *path)
{
    GDBusInterface *interface = g_dbus_object_manager_get_interface (bt->objmanager, path, "org.bluez.Device1");
    GVariant *var = g_dbus_proxy_get_cached_property (G_DBUS_PROXY (interface), "Connected");
    gboolean res = g_variant_get_boolean (var);
    g_variant_unref (var);
    g_object_unref (interface);
    return res;
}

static void connect_device (BluetoothPlugin *bt, const gchar *path, gboolean state)
{
    GDBusInterface *interface = g_dbus_object_manager_get_interface (bt->objmanager, path, "org.bluez.Device1");

    if (state)
    {
        // if trying to connect, make sure device is trusted first
        if (!is_trusted (bt, path)) trust_device (bt, path, TRUE);

        DEBUG ("Connecting to %s", path);
        g_dbus_proxy_call (G_DBUS_PROXY (interface), "Connect", NULL, G_DBUS_CALL_FLAGS_NONE, -1, NULL, cb_connected, bt);
        //g_dbus_proxy_call (G_DBUS_PROXY (interface), "ConnectProfile", g_variant_new ("(s)", "00001116-0000-1000-8000-00805f9b34fb"), G_DBUS_CALL_FLAGS_NONE, -1, NULL, cb_connected, bt);
    }
    else
    {
        DEBUG ("Disconnecting from %s", path);
        g_dbus_proxy_call (G_DBUS_PROXY (interface), "Disconnect", NULL, G_DBUS_CALL_FLAGS_NONE, -1, NULL, cb_disconnected, bt);
    }
    g_object_unref (interface);
}

static void cb_connected (GObject *source, GAsyncResult *res, gpointer user_data)
{
    BluetoothPlugin *bt = (BluetoothPlugin *) user_data;
    GError *error = NULL;
    GVariant *var = g_dbus_proxy_call_finish (G_DBUS_PROXY (source), res, &error);

    if (error)
    {
        DEBUG ("Connect error %s", error->message);
        if (bt->pair_dialog) show_pairing_dialog (bt, STATE_CONNECT_FAIL, NULL, error->message);
        if (bt->conn_dialog) show_connect_dialog (bt, DIALOG_CONNECT, STATE_FAIL, error->message);
        g_error_free (error);
    }
    else
    {
        DEBUG_VAR ("Connect result %s", var);
        if (bt->pair_dialog) show_pairing_dialog (bt, STATE_CONNECTED, NULL, NULL);
        if (bt->conn_dialog) handle_close_connect_dialog (NULL, bt);
    }
    if (var) g_variant_unref (var);
}

static void cb_disconnected (GObject *source, GAsyncResult *res, gpointer user_data)
{
    BluetoothPlugin *bt = (BluetoothPlugin *) user_data;
    GError *error = NULL;
    GVariant *var = g_dbus_proxy_call_finish (G_DBUS_PROXY (source), res, &error);

    if (error)
    {
        DEBUG ("Disconnect error %s", error->message);
        if (bt->conn_dialog) show_connect_dialog (bt, DIALOG_DISCONNECT, STATE_FAIL, error->message);
        g_error_free (error);
    }
    else
    {
        DEBUG_VAR ("Disconnect result %s", var);
        if (bt->conn_dialog) handle_close_connect_dialog (NULL, bt);
    }
    if (var) g_variant_unref (var);
}

/* Remove device */

static void remove_device (BluetoothPlugin *bt, const gchar *path)
{
    DEBUG ("Removing %s", path);
    GVariant *var = g_variant_new ("(o)", path);
    g_variant_ref_sink (var);
    g_dbus_proxy_call (bt->adapter, "RemoveDevice", var, G_DBUS_CALL_FLAGS_NONE, -1, NULL, cb_removed, bt);
    g_variant_unref (var);
}

static void cb_removed (GObject *source, GAsyncResult *res, gpointer user_data)
{
    BluetoothPlugin *bt = (BluetoothPlugin *) user_data;
    GError *error = NULL;
    GVariant *var = g_dbus_proxy_call_finish (G_DBUS_PROXY (source), res, &error);

    if (error)
    {
        DEBUG ("Remove error %s", error->message);
        if (bt->pair_dialog) show_pairing_dialog (bt, STATE_REMOVE_FAIL, NULL, error->message);
        if (bt->conn_dialog) show_connect_dialog (bt, DIALOG_REMOVE, STATE_FAIL, error->message);
        g_error_free (error);
    }
    else
    {
        DEBUG_VAR ("Remove result %s", var);
        if (bt->pair_dialog) handle_close_pair_dialog (NULL, bt);
        if (bt->conn_dialog) handle_close_connect_dialog (NULL, bt);
    }
    if (var) g_variant_unref (var);
}

static DEVICE_TYPE check_uuids (BluetoothPlugin *bt, const gchar *path)
{
    GDBusInterface *interface = g_dbus_object_manager_get_interface (bt->objmanager, path, "org.bluez.Device1");
    GVariant *elem, *var = g_dbus_proxy_get_cached_property (G_DBUS_PROXY (interface), "UUIDs");
    GVariantIter iter;
    g_variant_iter_init (&iter, var);
    while (elem = g_variant_iter_next_value (&iter))
    {
        const char *uuid = g_variant_get_string (elem, NULL);
        if (!strncasecmp (uuid, "00001124", 8)) return DEV_HID;
        if (!strncasecmp (uuid, "0000110B", 8)) return DEV_AUDIO_SINK;
        if (!strncasecmp (uuid, "00001812", 8)) return DEV_HID_LE;
        g_variant_unref (elem);
    }
    g_variant_unref (var);
    g_object_unref (interface);
    return DEV_OTHER;
}

/* GUI... */

/* Functions to respond to method calls on the agent */

static void handle_pin_entered (GtkButton *button, gpointer user_data)
{
    BluetoothPlugin *bt = (BluetoothPlugin *) user_data;
    DEBUG ("PIN entered by user");
    show_pairing_dialog (bt, STATE_WAITING, NULL, NULL);
    GVariant *retvar = g_variant_new ("(s)", gtk_entry_buffer_get_text (bt->pinbuf));
    g_variant_ref_sink (retvar);
    g_dbus_method_invocation_return_value (bt->invocation, retvar);
    g_variant_unref (retvar);
 }

static void handle_pass_entered (GtkButton *button, gpointer user_data)
{
    BluetoothPlugin *bt = (BluetoothPlugin *) user_data;
    guint32 passkey;
    DEBUG ("Passkey entered by user");
    show_pairing_dialog (bt, STATE_WAITING, NULL, NULL);
    sscanf (gtk_entry_buffer_get_text (bt->pinbuf), "%lu", &passkey);
    GVariant *retvar = g_variant_new ("(u)", passkey);
    g_variant_ref_sink (retvar);
    g_dbus_method_invocation_return_value (bt->invocation, retvar);
    g_variant_unref (retvar);
}

static void handle_pin_confirmed (GtkButton *button, gpointer user_data)
{
    BluetoothPlugin *bt = (BluetoothPlugin *) user_data;
    DEBUG ("PIN confirmed by user");
    show_pairing_dialog (bt, STATE_WAITING, NULL, NULL);
    g_dbus_method_invocation_return_value (bt->invocation, NULL);
}

static void handle_pin_rejected (GtkButton *button, gpointer user_data)
{
    BluetoothPlugin *bt = (BluetoothPlugin *) user_data;
    DEBUG ("PIN rejected by user");
    g_dbus_method_invocation_return_dbus_error (bt->invocation, "org.bluez.Error.Rejected", "Confirmation rejected by user");
    if (bt->pairing_object)
    {
        g_free (bt->pairing_object);
        bt->pairing_object = NULL;
    }
}

static void handle_authorize_yes (GtkButton *button, gpointer user_data)
{
    BluetoothPlugin *bt = (BluetoothPlugin *) user_data;
    DEBUG ("Pairing authorized by user");
    show_pairing_dialog (bt, STATE_WAITING, NULL, NULL);
    g_dbus_method_invocation_return_value (bt->invocation, NULL);
}

static void handle_authorize_no (GtkButton *button, gpointer user_data)
{
    BluetoothPlugin *bt = (BluetoothPlugin *) user_data;
    DEBUG ("Pairing not authorized by user");
    g_dbus_method_invocation_return_dbus_error (bt->invocation, "org.bluez.Error.Rejected", "Pairing rejected by user");
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
    handle_close_pair_dialog (NULL, bt);
}

static void handle_close_pair_dialog (GtkButton *button, gpointer user_data)
{
    BluetoothPlugin *bt = (BluetoothPlugin *) user_data;
    if (bt->pair_dialog)
    {
        gtk_widget_destroy (bt->pair_dialog);
        bt->pair_dialog = NULL;
    }
    if (bt->pairing_object)
    {
        g_free (bt->pairing_object);
        bt->pairing_object = NULL;
    }
}

static gint delete_pair (GtkWidget *widget, GdkEvent *event, gpointer user_data)
{
    BluetoothPlugin *bt = (BluetoothPlugin *) user_data;
    if (bt->pairing_object) pair_device (bt, bt->pairing_object, FALSE);
    handle_close_pair_dialog (NULL, bt);
    return TRUE;
}

static void handle_reject_pair (GtkButton *button, gpointer user_data)
{
    BluetoothPlugin *bt = (BluetoothPlugin *) user_data;
    show_pairing_dialog (bt, STATE_REMOVING, NULL, NULL);
    remove_device (bt, bt->incoming_object);
}

static void handle_accept_pair (GtkButton *button, gpointer user_data)
{
    BluetoothPlugin *bt = (BluetoothPlugin *) user_data;
    show_pairing_dialog (bt, STATE_PAIRED, NULL, NULL);
    connect_device (bt, bt->incoming_object, TRUE);
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
            sprintf (buffer, _("Pairing Device '%s'"), device);
            bt->pinbuf = gtk_entry_buffer_new (NULL, -1);
            bt->pair_dialog = gtk_dialog_new_with_buttons (buffer, NULL, 0, NULL);
            gtk_window_set_icon_name (GTK_WINDOW (bt->pair_dialog), "preferences-system-bluetooth");
            gtk_window_set_position (GTK_WINDOW (bt->pair_dialog), GTK_WIN_POS_CENTER);
            gtk_container_set_border_width (GTK_CONTAINER (bt->pair_dialog), 10);
            bt->pair_label = gtk_label_new (_("Pairing request sent to device - waiting for response..."));
            gtk_label_set_line_wrap (GTK_LABEL (bt->pair_label), TRUE);
            gtk_label_set_justify (GTK_LABEL (bt->pair_label), GTK_JUSTIFY_LEFT);
#if GTK_CHECK_VERSION(3, 0, 0)
            gtk_label_set_xalign (GTK_LABEL (bt->pair_label), 0.0);
            gtk_label_set_yalign (GTK_LABEL (bt->pair_label), 0.0);
#else
            gtk_misc_set_alignment (GTK_MISC (bt->pair_label), 0.0, 0.0);
#endif
            gtk_widget_set_size_request (bt->pair_label, 350, -1);
            gtk_box_pack_start (GTK_BOX (gtk_dialog_get_content_area (GTK_DIALOG (bt->pair_dialog))), bt->pair_label, TRUE, TRUE, 0);
            bt->pair_entry = gtk_entry_new_with_buffer (bt->pinbuf);
            gtk_box_pack_start (GTK_BOX (gtk_dialog_get_content_area (GTK_DIALOG (bt->pair_dialog))), bt->pair_entry, TRUE, TRUE, 0);
            bt->pair_cancel = gtk_dialog_add_button (GTK_DIALOG (bt->pair_dialog), _("_Cancel"), 0);
            bt->pair_ok = gtk_dialog_add_button (GTK_DIALOG (bt->pair_dialog), _("_OK"), 1);
            g_signal_connect (bt->pair_dialog, "delete_event", G_CALLBACK (delete_pair), bt);
            bt->ok_instance = 0;
            bt->cancel_instance = 0;
            connect_cancel (bt, G_CALLBACK (handle_cancel_pair));
            gtk_widget_show_all (bt->pair_dialog);
            gtk_widget_hide (bt->pair_entry);
            gtk_widget_hide (bt->pair_ok);
            gtk_widget_show (bt->pair_cancel);
            break;

        case STATE_PAIR_FAIL:
            if (!bt->pair_dialog) return;
            sprintf (buffer, _("Pairing failed - %s"), param);
            gtk_label_set_text (GTK_LABEL (bt->pair_label), buffer);
            gtk_widget_hide (bt->pair_entry);
            connect_ok (bt, G_CALLBACK (handle_close_pair_dialog));
            gtk_widget_show (bt->pair_ok);
            gtk_widget_hide (bt->pair_cancel);
            break;

        case STATE_PAIRED:
            gtk_label_set_text (GTK_LABEL (bt->pair_label), _("Pairing successful - creating connection..."));
            gtk_widget_hide (bt->pair_entry);
            gtk_widget_hide (bt->pair_ok);
            gtk_widget_hide (bt->pair_cancel);
            break;

        case STATE_CONNECTED:
            gtk_label_set_text (GTK_LABEL (bt->pair_label), _("Connected successfully"));
            connect_ok (bt, G_CALLBACK (handle_close_pair_dialog));
            gtk_widget_show (bt->pair_ok);
            gtk_widget_hide (bt->pair_cancel);
            break;

        case STATE_CONNECT_FAIL:
            sprintf (buffer, _("Connection failed - %s. Try to connect manually."), param);
            gtk_label_set_text (GTK_LABEL (bt->pair_label), buffer);
            connect_ok (bt, G_CALLBACK (handle_close_pair_dialog));
            gtk_widget_show (bt->pair_ok);
            gtk_widget_hide (bt->pair_cancel);
            break;

        case STATE_DISPLAY_PIN:
            sprintf (buffer, _("Please enter code '%s' on device '%s'"), param, device);
            gtk_label_set_text (GTK_LABEL (bt->pair_label), buffer);
            connect_cancel (bt, G_CALLBACK (handle_cancel_pair));
            gtk_widget_hide (bt->pair_ok);
            gtk_widget_show (bt->pair_cancel);
            break;

        case STATE_CONFIRM_PIN:
            sprintf (buffer, _("Please confirm that device '%s' is showing the code '%s' to connect"), device, param);
            gtk_label_set_text (GTK_LABEL (bt->pair_label), buffer);
            connect_cancel (bt, G_CALLBACK (handle_pin_rejected));
            connect_ok (bt, G_CALLBACK (handle_pin_confirmed));
            gtk_widget_show (bt->pair_ok);
            gtk_widget_show (bt->pair_cancel);
            break;

        case STATE_REQUEST_PIN:
        case STATE_REQUEST_PASS:
            sprintf (buffer, _("Please enter PIN code for device '%s'"), device);
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

        case STATE_WAITING:
            gtk_label_set_text (GTK_LABEL (bt->pair_label), _("Waiting for response from device..."));
            connect_cancel (bt, G_CALLBACK (handle_cancel_pair));
            gtk_widget_hide (bt->pair_entry);
            gtk_widget_hide (bt->pair_ok);
            gtk_widget_show (bt->pair_cancel);
            break;

        case STATE_PAIR_REQUEST:
            if (bt->pair_dialog)
            {
                sprintf (buffer, _("Device '%s' has requested a pairing. Do you accept the request?"), device);
                gtk_label_set_text (GTK_LABEL (bt->pair_label), buffer);
                connect_cancel (bt, G_CALLBACK (handle_authorize_no));
                connect_ok (bt, G_CALLBACK (handle_authorize_yes));
                gtk_widget_hide (bt->pair_entry);
                gtk_widget_show (bt->pair_ok);
                gtk_widget_show (bt->pair_cancel);
            }
            else
            {
                bt->pair_dialog = gtk_dialog_new_with_buttons (_("Pairing Requested"), NULL, 0, NULL);
                gtk_window_set_icon_name (GTK_WINDOW (bt->pair_dialog), "preferences-system-bluetooth");
                gtk_window_set_position (GTK_WINDOW (bt->pair_dialog), GTK_WIN_POS_CENTER);
                gtk_container_set_border_width (GTK_CONTAINER (bt->pair_dialog), 10);
                sprintf (buffer, _("Device '%s' has requested a pairing. Do you accept the request?"), device);
                bt->pair_label = gtk_label_new (buffer);
                gtk_label_set_line_wrap (GTK_LABEL (bt->pair_label), TRUE);
                gtk_label_set_justify (GTK_LABEL (bt->pair_label), GTK_JUSTIFY_LEFT);
#if GTK_CHECK_VERSION(3, 0, 0)
                gtk_label_set_xalign (GTK_LABEL (bt->pair_label), 0.0);
#else
                gtk_misc_set_alignment (GTK_MISC (bt->pair_label), 0.0, 0.0);
#endif
                gtk_box_pack_start (GTK_BOX (gtk_dialog_get_content_area (GTK_DIALOG (bt->pair_dialog))), bt->pair_label, TRUE, TRUE, 0);
                bt->pair_cancel = gtk_dialog_add_button (GTK_DIALOG (bt->pair_dialog), _("_Cancel"), 0);
                bt->pair_ok = gtk_dialog_add_button (GTK_DIALOG (bt->pair_dialog), _("_OK"), 1);
                bt->ok_instance = 0;
                bt->cancel_instance = 0;
                connect_cancel (bt, G_CALLBACK (handle_reject_pair));
                connect_ok (bt, G_CALLBACK (handle_accept_pair));
                gtk_widget_show_all (bt->pair_dialog);
            }
            break;

        case STATE_REMOVING:
            sprintf (buffer, _("Rejecting pairing..."));
            gtk_label_set_text (GTK_LABEL (bt->pair_label), buffer);
            gtk_widget_hide (bt->pair_ok);
            gtk_widget_hide (bt->pair_cancel);
            break;

        case STATE_REMOVE_FAIL:
            sprintf (buffer, _("Removal of pairing failed - %s. Remove the device manually."), param);
            gtk_label_set_text (GTK_LABEL (bt->pair_label), buffer);
            connect_ok (bt, G_CALLBACK (handle_close_pair_dialog));
            gtk_widget_show (bt->pair_ok);
            gtk_widget_hide (bt->pair_cancel);
            break;

        case STATE_PAIRED_AUDIO:
            gtk_label_set_text (GTK_LABEL (bt->pair_label), _("Paired successfully. Use the audio menu to select as output device."));
            connect_ok (bt, G_CALLBACK (handle_close_pair_dialog));
            gtk_widget_show (bt->pair_ok);
            gtk_widget_hide (bt->pair_cancel);
            break;

        case STATE_PAIRED_UNUSABLE:
            gtk_label_set_text (GTK_LABEL (bt->pair_label), _("Paired successfully, but this device has no services which can be used with Raspberry Pi."));
            connect_ok (bt, G_CALLBACK (handle_close_pair_dialog));
            gtk_widget_show (bt->pair_ok);
            gtk_widget_hide (bt->pair_cancel);
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
        if (bt->list_dialog)
        {
            g_object_unref (bt->sorted_list);
            bt->sorted_list = NULL;
            gtk_widget_destroy (bt->list);
            bt->list = NULL;
            gtk_widget_destroy (bt->list_dialog);
            bt->list_dialog = NULL;
        }
        set_search (bt, FALSE);

        if (!is_paired (bt, path))
        {
            show_pairing_dialog (bt, STATE_PAIR_INIT, name, NULL);
            bt->pairing_object = path;
            pair_device (bt, path, TRUE);
            // path is freed as bt->pairing_object later...
        }
        else g_free (path);
        g_free (name);
    }
}

static void handle_remove (GtkButton *button, gpointer user_data)
{
    BluetoothPlugin *bt = (BluetoothPlugin *) user_data;
    show_connect_dialog (bt, DIALOG_REMOVE, STATE_CONFIRMED, bt->device_name);
    remove_device (bt, bt->device_path);
    g_free (bt->device_path);
    g_free (bt->device_name);
    bt->device_path = NULL;
    bt->device_name = NULL;
}

static void handle_close_list_dialog (GtkButton *button, gpointer user_data)
{
    BluetoothPlugin * bt = (BluetoothPlugin *) user_data;
    if (bt->list_dialog)
    {
        g_object_unref (bt->sorted_list);
        bt->sorted_list = NULL;
        gtk_widget_destroy (bt->list);
        bt->list = NULL;
        gtk_widget_destroy (bt->list_dialog);
        bt->list_dialog = NULL;
        if (is_searching (bt)) set_search (bt, FALSE);
    }
}

static gint delete_list (GtkWidget *widget, GdkEvent *event, gpointer user_data)
{
    BluetoothPlugin * bt = (BluetoothPlugin *) user_data;
    if (bt->list_dialog)
    {
        g_object_unref (bt->sorted_list);
        bt->sorted_list = NULL;
        gtk_widget_destroy (bt->list);
        bt->list = NULL;
        gtk_widget_destroy (bt->list_dialog);
        bt->list_dialog = NULL;
        if (is_searching (bt)) set_search (bt, FALSE);
    }
    return TRUE;
}

static gboolean filter_unknowns (GtkTreeModel *model, GtkTreeIter *iter, gpointer data)
{
    char *str;
    gboolean res = FALSE;

    gtk_tree_model_get (model, iter, 6, &str, -1);
    if (g_strcmp0 (str, "dialog-question")) res = TRUE;
    g_free (str);
    return res;
}

static void sel_changed (GtkTreeSelection *sel, gpointer user_data)
{
    BluetoothPlugin * bt = (BluetoothPlugin *) user_data;
    GtkTreeModel *mod;
    gtk_widget_set_sensitive (bt->list_ok, gtk_tree_selection_get_selected (sel, &mod, NULL));
}

static void show_list_dialog (BluetoothPlugin * bt, DIALOG_TYPE type)
{
    GtkWidget *btn_cancel, *frm, *lbl, *scrl;
    GtkCellRenderer *rend;

    // create the window
    bt->list_dialog = gtk_dialog_new_with_buttons (type == DIALOG_PAIR ? _("Add New Device") : _("Remove Device"), NULL, 0, NULL);
    gtk_window_set_icon_name (GTK_WINDOW (bt->list_dialog), "preferences-system-bluetooth");
    gtk_window_set_position (GTK_WINDOW (bt->list_dialog), GTK_WIN_POS_CENTER);
    gtk_container_set_border_width (GTK_CONTAINER (bt->list_dialog), 5);
    gtk_box_set_spacing (GTK_BOX (gtk_dialog_get_content_area (GTK_DIALOG (bt->list_dialog))), 10);
    gtk_box_set_homogeneous (GTK_BOX (gtk_dialog_get_content_area (GTK_DIALOG (bt->list_dialog))), FALSE);

    // add the buttons
    btn_cancel = gtk_dialog_add_button (GTK_DIALOG (bt->list_dialog), _("_Cancel"), 0);
    bt->list_ok = gtk_dialog_add_button (GTK_DIALOG (bt->list_dialog), type == DIALOG_PAIR ? _("_Pair") : _("_Remove"), 1);
    g_signal_connect (bt->list_ok, "clicked", type == DIALOG_PAIR ? G_CALLBACK (handle_pair) : G_CALLBACK (handle_remove), bt);
    g_signal_connect (btn_cancel, "clicked", G_CALLBACK (handle_close_list_dialog), bt);
    g_signal_connect (bt->list_dialog, "delete_event", G_CALLBACK (delete_list), bt);
    gtk_widget_set_sensitive (bt->list_ok, FALSE);

    // add a label
    lbl = gtk_label_new (type == DIALOG_PAIR ? _("Searching for Bluetooth devices...") : _("Paired Bluetooth devices"));
#if GTK_CHECK_VERSION(3, 0, 0)
    gtk_label_set_xalign (GTK_LABEL (lbl), 0.0);
#else
    gtk_misc_set_alignment (GTK_MISC (lbl), 0.0, 0.5);
#endif
    gtk_box_pack_start (GTK_BOX (gtk_dialog_get_content_area (GTK_DIALOG (bt->list_dialog))), lbl, FALSE, FALSE, 0);

    // add a scrolled window
    scrl = gtk_scrolled_window_new (NULL, NULL);
    gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (scrl), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_shadow_type (GTK_SCROLLED_WINDOW (scrl), GTK_SHADOW_IN);
    gtk_box_pack_start (GTK_BOX (gtk_dialog_get_content_area (GTK_DIALOG (bt->list_dialog))), scrl, TRUE, TRUE, 0);

    // create the list view and add to scrolled window
    bt->list = gtk_tree_view_new ();
    gtk_tree_view_set_fixed_height_mode (GTK_TREE_VIEW (bt->list), TRUE);
    gtk_tree_view_set_headers_visible (GTK_TREE_VIEW (bt->list), FALSE);
#if GTK_CHECK_VERSION(3, 0, 0)
    gtk_widget_set_size_request (scrl, -1, 175);
#else
    gtk_widget_set_size_request (bt->list, -1, 150);
#endif
    rend = gtk_cell_renderer_pixbuf_new ();
    gtk_tree_view_insert_column_with_attributes (GTK_TREE_VIEW (bt->list), -1, "Icon", rend, "pixbuf", 5, NULL);
    gtk_tree_view_column_set_fixed_width (gtk_tree_view_get_column (GTK_TREE_VIEW (bt->list), 0), 50);
    rend = gtk_cell_renderer_text_new ();
    gtk_tree_view_insert_column_with_attributes (GTK_TREE_VIEW (bt->list), -1, "Name", rend, "text", 1, NULL);
    gtk_tree_view_column_set_fixed_width (gtk_tree_view_get_column (GTK_TREE_VIEW (bt->list), 1), 300);

    bt->filter_list = GTK_TREE_MODEL_FILTER (gtk_tree_model_filter_new (type == DIALOG_PAIR ? GTK_TREE_MODEL (bt->unpair_list) : GTK_TREE_MODEL (bt->pair_list), NULL));
    gtk_tree_model_filter_set_visible_func (bt->filter_list, (GtkTreeModelFilterVisibleFunc) filter_unknowns, NULL, NULL);

    bt->sorted_list = GTK_TREE_MODEL_SORT (gtk_tree_model_sort_new_with_model (GTK_TREE_MODEL (bt->filter_list)));
    gtk_tree_sortable_set_sort_column_id (GTK_TREE_SORTABLE (bt->sorted_list), 1, GTK_SORT_ASCENDING);
    gtk_tree_view_set_model (GTK_TREE_VIEW (bt->list), GTK_TREE_MODEL (bt->sorted_list));
    gtk_tree_view_set_tooltip_column (GTK_TREE_VIEW (bt->list), 0);
    gtk_container_add (GTK_CONTAINER (scrl), bt->list);

    // window ready
    gtk_widget_show_all (bt->list_dialog);

    // remove the selection which mysteriously appears when the widget is shown...
    gtk_tree_selection_unselect_all (gtk_tree_view_get_selection (GTK_TREE_VIEW (bt->list)));
    g_signal_connect (gtk_tree_view_get_selection (GTK_TREE_VIEW (bt->list)), "changed", G_CALLBACK (sel_changed), bt);
    bt->selection = NULL;
}

/* Functions to manage connect / disconnect / remove notification dialogs */

static void show_connect_dialog (BluetoothPlugin *bt, DIALOG_TYPE type, CONN_STATE state, const gchar *param)
{
    char buffer1[256], buffer2[256];

    switch (type)
    {
        case DIALOG_REMOVE:
            sprintf (buffer1, _("Remove Device"));
            switch (state)
            {
                case STATE_CONFIRM:
                    sprintf (buffer2, _("Do you want to unpair '%s'?"), param);
                    break;
                case STATE_CONFIRMED:
                    sprintf (buffer2, _("Removing paired device '%s'..."), param);
                    break;
                case STATE_FAIL:
                    sprintf (buffer2, _("Removal failed - %s"), param);
                    break;
            }
            break;

        case DIALOG_CONNECT:
            sprintf (buffer1, _("Connect Device"));
            sprintf (buffer2, state == STATE_INIT ? _("Connecting to device '%s'...") : _("Connection failed - %s"), param);
            break;

        case DIALOG_DISCONNECT:
            sprintf (buffer1, _("Disconnect Device"));
            sprintf (buffer2, state == STATE_INIT ? _("Disconnecting from device '%s'...") : _("Disconnection failed - %s"), param);
            break;
    }

    switch (state)
    {
        case STATE_INIT:
        case STATE_CONFIRM:
            bt->conn_dialog = gtk_dialog_new_with_buttons (buffer1, NULL, 0, NULL);
            gtk_window_set_icon_name (GTK_WINDOW (bt->conn_dialog), "preferences-system-bluetooth");
            gtk_window_set_position (GTK_WINDOW (bt->conn_dialog), GTK_WIN_POS_CENTER);
            gtk_container_set_border_width (GTK_CONTAINER (bt->conn_dialog), 10);
            bt->conn_label = gtk_label_new (buffer2);
            gtk_label_set_line_wrap (GTK_LABEL (bt->conn_label), TRUE);
            gtk_label_set_justify (GTK_LABEL (bt->conn_label), GTK_JUSTIFY_LEFT);
#if GTK_CHECK_VERSION(3, 0, 0)
            gtk_label_set_xalign (GTK_LABEL (bt->conn_label), 0.0);
            gtk_label_set_yalign (GTK_LABEL (bt->conn_label), 0.0);
#else
            gtk_misc_set_alignment (GTK_MISC (bt->conn_label), 0.0, 0.0);
#endif
            gtk_widget_set_size_request (bt->conn_label, 350, -1);
            gtk_box_pack_start (GTK_BOX (gtk_dialog_get_content_area (GTK_DIALOG (bt->conn_dialog))), bt->conn_label, TRUE, TRUE, 0);
            g_signal_connect (bt->conn_dialog, "delete_event", G_CALLBACK (delete_conn), bt);
            if (state == STATE_CONFIRM)
            {
                bt->conn_cancel = gtk_dialog_add_button (GTK_DIALOG (bt->conn_dialog), _("_Cancel"), 0);
                g_signal_connect (bt->conn_cancel, "clicked", G_CALLBACK (handle_close_connect_dialog), bt);
                bt->conn_ok = gtk_dialog_add_button (GTK_DIALOG (bt->conn_dialog), _("_OK"), 1);
                g_signal_connect (bt->conn_ok, "clicked", G_CALLBACK (handle_remove), bt);
            }
            gtk_widget_show_all (bt->conn_dialog);
            break;

        case STATE_FAIL:
            gtk_label_set_text (GTK_LABEL (bt->conn_label), buffer2);
            bt->conn_ok = gtk_dialog_add_button (GTK_DIALOG (bt->conn_dialog), _("_OK"), 1);
            g_signal_connect (bt->conn_ok, "clicked", G_CALLBACK (handle_close_connect_dialog), bt);
            gtk_widget_show (bt->conn_ok);
            break;

        case STATE_CONFIRMED:
            gtk_label_set_text (GTK_LABEL (bt->conn_label), buffer2);
            gtk_widget_hide (bt->conn_ok);
            gtk_widget_hide (bt->conn_cancel);
            break;
    }
}

static void handle_close_connect_dialog (GtkButton *button, gpointer user_data)
{
    BluetoothPlugin *bt = (BluetoothPlugin *) user_data;

    if (bt->device_name) g_free (bt->device_name);
    if (bt->device_path) g_free (bt->device_path);
    bt->device_path = NULL;
    bt->device_name = NULL;

    if (bt->conn_dialog)
    {
        gtk_widget_destroy (bt->conn_dialog);
        bt->conn_dialog = NULL;
    }
}

static gint delete_conn (GtkWidget *widget, GdkEvent *event, gpointer user_data)
{
    BluetoothPlugin *bt = (BluetoothPlugin *) user_data;

    if (bt->device_name) g_free (bt->device_name);
    if (bt->device_path) g_free (bt->device_path);
    bt->device_path = NULL;
    bt->device_name = NULL;

    if (bt->conn_dialog)
    {
        gtk_widget_destroy (bt->conn_dialog);
        bt->conn_dialog = NULL;
    }
    return TRUE;
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
    gchar *name, *path;
    gboolean valid;
    GtkTreeIter iter;

    valid = gtk_tree_model_get_iter_first (GTK_TREE_MODEL (bt->pair_list), &iter);
    while (valid)
    {
        gtk_tree_model_get (GTK_TREE_MODEL (bt->pair_list), &iter, 0, &path, 1, &name, -1);
        if (!g_strcmp0 (gtk_widget_get_name (widget), path))
        {
            bt->device_path = path;
            bt->device_name = name;
            show_connect_dialog (bt, DIALOG_REMOVE, STATE_CONFIRM, name);
            break;
        }
        g_free (name);
        g_free (path);
        valid = gtk_tree_model_iter_next (GTK_TREE_MODEL (bt->pair_list), &iter);
    }
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
            {
                show_connect_dialog (bt, DIALOG_CONNECT, STATE_INIT, name);
                if (check_uuids (bt, path) == DEV_AUDIO_SINK)
                    show_connect_dialog (bt, DIALOG_CONNECT, STATE_FAIL, _("Use the audio menu to connect to this device"));
                else if (check_uuids (bt, path) == DEV_OTHER)
                    show_connect_dialog (bt, DIALOG_CONNECT, STATE_FAIL, _("No usable services on this device"));
                else connect_device (bt, path, TRUE);
            }
            else
            {
                show_connect_dialog (bt, DIALOG_DISCONNECT, STATE_INIT, name);
                connect_device (bt, path, FALSE);
            }
            break;
        }
        g_free (name);
        g_free (path);
        valid = gtk_tree_model_iter_next (GTK_TREE_MODEL (bt->pair_list), &iter);    
    }
}

static gboolean flash_icon (gpointer user_data)
{
    BluetoothPlugin *bt = (BluetoothPlugin *) user_data;

    if (bt->flash_timer == 0) return FALSE;
    lxpanel_plugin_set_taskbar_icon (bt->panel, bt->tray_icon, bt->flash_state ? "preferences-system-bluetooth-active" : "preferences-system-bluetooth");
    bt->flash_state ^= 1;
    return TRUE;
}

static void handle_menu_discover (GtkWidget *widget, gpointer user_data)
{
    BluetoothPlugin *bt = (BluetoothPlugin *) user_data;

    if (!is_discoverable (bt))
    {
        set_discoverable (bt, TRUE);
        if (bt->flash_timer) g_source_remove (bt->flash_timer);
        bt->flash_timer = g_timeout_add (500, flash_icon, bt);
    }
    else
    {
        set_discoverable (bt, FALSE);
        if (bt->flash_timer) g_source_remove (bt->flash_timer);
        bt->flash_timer = 0;
        lxpanel_plugin_set_taskbar_icon (bt->panel, bt->tray_icon, "preferences-system-bluetooth");
    }
}

static gboolean add_to_menu (GtkTreeModel *model, GtkTreePath *tpath, GtkTreeIter *iter, gpointer user_data)
{
    BluetoothPlugin *bt = (BluetoothPlugin *) user_data;
    gchar *name, *path;
    GtkWidget *item, *submenu, *smi, *icon;
    GList *list, *l;
    int count;
 
    gtk_tree_model_get (model, iter, 0, &path, 1, &name, -1);
#if GTK_CHECK_VERSION(3, 0, 0)
    item = lxpanel_plugin_new_menu_item (bt->panel, name, 0, NULL);
#else
    item = gtk_image_menu_item_new_with_label (name);
    gtk_image_menu_item_set_always_show_image (GTK_IMAGE_MENU_ITEM (item), TRUE);
#endif

    // create a submenu for each paired device
    submenu = gtk_menu_new ();
    gtk_menu_item_set_submenu (GTK_MENU_ITEM (item), submenu);

    // create a single item for the submenu
    icon = gtk_image_new ();
    if (is_connected (bt, path))
    {
        lxpanel_plugin_set_menu_icon (bt->panel, icon, "bluetooth-online");
        smi = gtk_menu_item_new_with_label (_("Disconnect..."));
    }
    else
    {
        lxpanel_plugin_set_menu_icon (bt->panel, icon, "bluetooth-offline");
        smi = gtk_menu_item_new_with_label (_("Connect..."));
    }

#if GTK_CHECK_VERSION(3, 0, 0)
    lxpanel_plugin_update_menu_icon (item, icon);
#else
    gtk_image_menu_item_set_image (GTK_IMAGE_MENU_ITEM (item), icon);
#endif

    // use the widget name of the menu item to store the unique path of the paired device
    gtk_widget_set_name (smi, path);

    // connect the connect toggle function and add the submenu item to the submenu
    g_signal_connect (smi, "activate", G_CALLBACK (handle_menu_connect), bt);
    gtk_menu_shell_append (GTK_MENU_SHELL (submenu), smi);

    // add the remove option to the submenu
    smi = gtk_menu_item_new_with_label (_("Remove..."));
    gtk_widget_set_name (smi, path);
    g_signal_connect (smi, "activate", G_CALLBACK (handle_menu_remove), bt);
    gtk_menu_shell_append (GTK_MENU_SHELL (submenu), smi);

    // find the start point of the last section - either a separator or the beginning of the list
    list = gtk_container_get_children (GTK_CONTAINER (bt->menu));
    count = g_list_length (list);
    l = g_list_last (list);
    while (l)
    {
        if (G_OBJECT_TYPE (l->data) == GTK_TYPE_SEPARATOR_MENU_ITEM) break;
        count--;
        l = l->prev;
    }

    // if l is NULL, init to element after start; if l is non-NULL, init to element after separator
    if (!l) l = list;
    else l = l->next;

    // loop forward from the first element, comparing against the new label
    while (l)
    {
#if GTK_CHECK_VERSION(3, 0, 0)
        if (g_strcmp0 (name, lxpanel_plugin_get_menu_label (GTK_WIDGET (l->data))) < 0) break;
#else
        if (g_strcmp0 (name, gtk_menu_item_get_label (GTK_MENU_ITEM (l->data))) < 0) break;
#endif
        count++;
        l = l->next;
    }

    // insert at the relevant offset
    gtk_menu_shell_insert (GTK_MENU_SHELL (bt->menu), item, count);

    g_list_free (list);
    g_free (name);
    g_free (path);

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

    icon = icon_from_cache (bt, var5 ? g_variant_get_string (var5, NULL) : "dialog-question");

    gtk_list_store_set (lst, &iter, 0, g_dbus_object_get_object_path (object),
        1, var1 ? g_variant_get_string (var1, NULL) : "Unnamed device", 2, g_variant_get_boolean (var2),
        3, g_variant_get_boolean (var3), 4, g_variant_get_boolean (var4), 
        5, icon, 6, var5 ? g_variant_get_string (var5, NULL) : "dialog-question", -1);

    if (var1) g_variant_unref (var1);
    if (var2) g_variant_unref (var2);
    if (var3) g_variant_unref (var3);
    if (var4) g_variant_unref (var4);
    if (var5) g_variant_unref (var5);
    g_object_unref (interface);
}

gboolean find_path (GtkTreeModel *model, GtkTreePath *path, GtkTreeIter *iter, gpointer data)
{
    BluetoothPlugin *bt = (BluetoothPlugin *) data;

    char *btpath;
    gtk_tree_model_get (model, iter, 0, &btpath, -1);
    if (!g_strcmp0 (btpath, bt->selection))
    {
        GtkTreeSelection *sel = gtk_tree_view_get_selection (GTK_TREE_VIEW (bt->list));
        gtk_tree_selection_select_iter (sel, iter);
        g_free (btpath);
        return TRUE;
    }
    g_free (btpath);
    return FALSE;
}

static void update_device_list (BluetoothPlugin *bt)
{
    GDBusInterface *interface;
    GDBusObject *object;
    GList *objects, *interfaces, *obj_elem, *if_elem;
    GVariant *var;
    gchar *name = NULL;

    // save the current highlight if there is one
    if (bt->list) selected_path (bt, &(bt->selection), &name);
    else bt->selection = NULL;

    // clear out the list store
    gtk_list_store_clear (bt->pair_list);
    gtk_list_store_clear (bt->unpair_list);

    // iterate all the objects the manager knows about
    objects = g_dbus_object_manager_get_objects (bt->objmanager);
    for (obj_elem = objects; obj_elem != NULL; obj_elem = obj_elem->next)
    {
        object = (GDBusObject *) obj_elem->data;
        interfaces = g_dbus_object_get_interfaces (object);
        for (if_elem = interfaces; if_elem != NULL; if_elem = if_elem->next)
        {
            // if an object has a Device1 interface, it is a Bluetooth device - add it to the list
            interface = G_DBUS_INTERFACE (if_elem->data);
            if (g_strcmp0 (g_dbus_proxy_get_interface_name (G_DBUS_PROXY (interface)), "org.bluez.Device1") == 0)
            {
                var = g_dbus_proxy_get_cached_property (G_DBUS_PROXY (interface), "Paired");
                if (g_variant_get_boolean (var)) add_device (bt, object, bt->pair_list);
                else add_device (bt, object, bt->unpair_list);
                g_variant_unref (var);
                break;
            }
        }
        g_list_free_full (interfaces, g_object_unref);
    }
    g_list_free_full (objects, g_object_unref);

    // replace the selection
    if (bt->selection)
    {
        GtkTreeModel *mod = gtk_tree_view_get_model (GTK_TREE_VIEW (bt->list));
        if (mod) gtk_tree_model_foreach (mod, find_path, bt);
        g_free (bt->selection);
        bt->selection = NULL;
    }
    if (name) g_free (name);

    if (bt->menu && gtk_widget_get_visible (bt->menu)) show_menu (bt);
}

static void init_icon_cache (BluetoothPlugin *bt)
{
    int i;

    for (i = 0; i < ICON_CACHE_SIZE; i++)
    {
        bt->icon_ref[i] = gtk_icon_theme_load_icon (panel_get_icon_theme (bt->panel), icon_names[i], 32, 0, NULL);
    }
}

static GdkPixbuf *icon_from_cache (BluetoothPlugin *bt, const gchar *icon_name)
{
    int i;

    for (i = 0; i < ICON_CACHE_SIZE; i++)
    {
        if (!g_strcmp0 (icon_names[i], icon_name)) return bt->icon_ref[i];
    }

    return bt->icon_ref[ICON_CACHE_SIZE - 1];
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
    GtkWidget *item;
    GtkTreeIter iter;
    GList *items, *head;
    int bt_state;

    // if the menu is currently on screen, delete all the items and rebuild rather than creating a new one
    if (bt->menu && gtk_widget_get_visible (bt->menu))
    {
        items = gtk_container_get_children (GTK_CONTAINER (bt->menu));
        g_list_free_full (items, (GDestroyNotify) gtk_widget_destroy);
    }
    else bt->menu = gtk_menu_new ();
#if GTK_CHECK_VERSION(3, 0, 0)
    gtk_menu_set_reserve_toggle_size (GTK_MENU (bt->menu), FALSE);
#endif

    bt_state = bt_enabled ();
    if ((bt_state == -2 && bt->adapter == NULL) || bt_state == -1)
    {
        // warn if no BT hardware detected
#if GTK_CHECK_VERSION(3, 0, 0)
        item = lxpanel_plugin_new_menu_item (bt->panel, _("No Bluetooth adapter found"), 0, NULL);
#else
        item = gtk_menu_item_new_with_label (_("No Bluetooth adapter found"));
#endif
        gtk_widget_set_sensitive (item, FALSE);
        gtk_menu_shell_append (GTK_MENU_SHELL (bt->menu), item);
    }
    else if (bt_state == 0)
    {
        // add enable bt option
#if GTK_CHECK_VERSION(3, 0, 0)
        item = lxpanel_plugin_new_menu_item (bt->panel, _("Turn On Bluetooth"), 0, NULL);
#else
        item = gtk_menu_item_new_with_label (_("Turn On Bluetooth"));
#endif
        g_signal_connect (G_OBJECT (item), "activate", G_CALLBACK (toggle_bt), bt);
        gtk_menu_shell_append (GTK_MENU_SHELL (bt->menu), item);
    }
    else
    {
        if (bt_state == 1)
        {
            // add disable bt option
#if GTK_CHECK_VERSION(3, 0, 0)
            item = lxpanel_plugin_new_menu_item (bt->panel, _("Turn Off Bluetooth"), 0, NULL);
#else
            item = gtk_menu_item_new_with_label (_("Turn Off Bluetooth"));
#endif
            g_signal_connect (G_OBJECT (item), "activate", G_CALLBACK (toggle_bt), bt);
            gtk_menu_shell_append (GTK_MENU_SHELL (bt->menu), item);
            item = gtk_separator_menu_item_new ();
            gtk_menu_shell_append (GTK_MENU_SHELL (bt->menu), item);
        }

        // discoverable toggle
        if (is_discoverable (bt))
#if GTK_CHECK_VERSION(3, 0, 0)
            item = lxpanel_plugin_new_menu_item (bt->panel, _("Stop Discoverable"), 0, NULL);
#else
            item = gtk_menu_item_new_with_label (_("Stop Discoverable"));
#endif
        else
#if GTK_CHECK_VERSION(3, 0, 0)
            item = lxpanel_plugin_new_menu_item (bt->panel, _("Make Discoverable"), 0, NULL);
#else
            item = gtk_menu_item_new_with_label (_("Make Discoverable"));
#endif
        g_signal_connect (item, "activate", G_CALLBACK (handle_menu_discover), bt);
        gtk_menu_shell_append (GTK_MENU_SHELL (bt->menu), item);
        item = gtk_separator_menu_item_new ();
        gtk_menu_shell_append (GTK_MENU_SHELL (bt->menu), item);

        // add and remove dialogs
#if GTK_CHECK_VERSION(3, 0, 0)
        item = lxpanel_plugin_new_menu_item (bt->panel, _("Add Device..."), 0, NULL);
#else
        item = gtk_menu_item_new_with_label (_("Add Device..."));
#endif
        g_signal_connect (item, "activate", G_CALLBACK (handle_menu_add), bt);
        gtk_menu_shell_append (GTK_MENU_SHELL (bt->menu), item);

        // paired devices
        if (gtk_tree_model_get_iter_first (GTK_TREE_MODEL (bt->pair_list), &iter))
        {
            item = gtk_separator_menu_item_new ();
            gtk_menu_shell_append (GTK_MENU_SHELL (bt->menu), item);

            gtk_tree_model_foreach (GTK_TREE_MODEL (bt->pair_list), add_to_menu, bt);
        }
    }

    // lock menu if a dialog is open
    if (bt->list_dialog || bt->pair_dialog || bt->conn_dialog)
    {
        items = gtk_container_get_children (GTK_CONTAINER (bt->menu));
        head = items;
        while (items)
        {
            gtk_widget_set_sensitive (GTK_WIDGET (items->data), FALSE);
            items = items->next;
        }
        g_list_free (head);
    }

    gtk_widget_show_all (bt->menu);
}

static void update_icon (BluetoothPlugin *bt)
{
    int bt_state;

    bt_state = bt_enabled ();
    bt_state = bt_enabled ();   // not a bug - poll a few times to allow to settle...
    bt_state = bt_enabled ();

    if ((bt_state == -2 && bt->adapter == NULL) || bt_state == -1)
    {
        // no adapter found - hide the icon
        if (bt->flash_timer)
        {
            g_source_remove (bt->flash_timer);
            bt->flash_timer = 0;
        }
#if GTK_CHECK_VERSION(3, 0, 0)
        gtk_widget_hide (bt->plugin);
#else
        gtk_widget_hide_all (bt->plugin);
#endif
        gtk_widget_set_sensitive (bt->plugin, FALSE);
        return;
    }

    if (bt_state == 0) lxpanel_plugin_set_taskbar_icon (bt->panel, bt->tray_icon, "preferences-system-bluetooth-inactive");
    else
    {
        lxpanel_plugin_set_taskbar_icon (bt->panel, bt->tray_icon, "preferences-system-bluetooth");
        if (is_discoverable (bt) && !bt->flash_timer) bt->flash_timer = g_timeout_add (500, flash_icon, bt);
    }
    gtk_widget_show_all (bt->plugin);
    gtk_widget_set_sensitive (bt->plugin, TRUE);
}

/* Handler for menu button click */
static gboolean bluetooth_button_press_event (GtkWidget *widget, GdkEventButton *event, LXPanel *panel)
{
    BluetoothPlugin *bt = lxpanel_plugin_get_data (widget);

#ifdef ENABLE_NLS
    textdomain ( GETTEXT_PACKAGE );
#endif
    /* Show or hide the popup menu on left-click */
    if (event->button == 1)
    {
        show_menu (bt);
#if GTK_CHECK_VERSION(3, 0, 0)
        gtk_menu_popup_at_widget (GTK_MENU (bt->menu), bt->plugin, GDK_GRAVITY_NORTH_WEST, GDK_GRAVITY_NORTH_WEST, (GdkEvent *) event);
#else
        gtk_menu_popup (GTK_MENU (bt->menu), NULL, NULL, menu_popup_set_position, bt, 1, gtk_get_current_event_time ());
#endif
        return TRUE;
    }
    else return FALSE;
}

/* Handler for system config changed message from panel */
static void bluetooth_configuration_changed (LXPanel *panel, GtkWidget *widget)
{
    BluetoothPlugin *bt = lxpanel_plugin_get_data (widget);

    update_icon (bt);
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

#ifdef ENABLE_NLS
    setlocale (LC_ALL, "");
    bindtextdomain (GETTEXT_PACKAGE, PACKAGE_LOCALE_DIR);
    bind_textdomain_codeset (GETTEXT_PACKAGE, "UTF-8");
    textdomain (GETTEXT_PACKAGE);
#endif

    bt->tray_icon = gtk_image_new ();
    lxpanel_plugin_set_taskbar_icon (panel, bt->tray_icon, "preferences-system-bluetooth-inactive");
    gtk_widget_set_tooltip_text (bt->tray_icon, _("Manage Bluetooth devices"));
    gtk_widget_set_visible (bt->tray_icon, TRUE);

    /* Allocate top level widget and set into Plugin widget pointer. */
    bt->panel = panel;
    bt->plugin = gtk_button_new ();
    gtk_button_set_relief (GTK_BUTTON (bt->plugin), GTK_RELIEF_NONE);
    g_signal_connect (bt->plugin, "button-press-event", G_CALLBACK (bluetooth_button_press_event), NULL);
    bt->settings = settings;
    lxpanel_plugin_set_data (bt->plugin, bt, bluetooth_destructor);
    gtk_widget_add_events (bt->plugin, GDK_BUTTON_PRESS_MASK);

    /* Allocate icon as a child of top level */
    gtk_container_add (GTK_CONTAINER (bt->plugin), bt->tray_icon);

    /* Show the widget */
    gtk_widget_show_all (bt->plugin);
#if GTK_CHECK_VERSION(3, 0, 0)
    gtk_widget_hide (bt->plugin);
#else
    gtk_widget_hide_all (bt->plugin);
#endif
    gtk_widget_set_sensitive (bt->plugin, FALSE);

    /* Initialise plugin data */
    bt->pair_list = gtk_list_store_new (7, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_UINT, G_TYPE_UINT, G_TYPE_UINT, GDK_TYPE_PIXBUF, G_TYPE_STRING);
    bt->unpair_list = gtk_list_store_new (7, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_UINT, G_TYPE_UINT, G_TYPE_UINT, GDK_TYPE_PIXBUF, G_TYPE_STRING);
    bt->ok_instance = 0;
    bt->cancel_instance = 0;
    bt->device_name = NULL;
    bt->device_path = NULL;
    bt->pair_dialog = NULL;
    bt->conn_dialog = NULL;
    bt->list_dialog = NULL;
    clear (bt);

    /* Load icon cache */
    init_icon_cache (bt);

    /* Set up callbacks to see if BlueZ is on DBus */
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
    .button_press_event = bluetooth_button_press_event,
    .gettext_package = GETTEXT_PACKAGE
};
