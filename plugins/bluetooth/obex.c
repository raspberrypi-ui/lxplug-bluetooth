/*
Copyright (c) 2021 Raspberry Pi (Trading) Ltd.
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
#include <glib/gi18n.h>
#include <glib/gstdio.h> // for g_rename()
#include <glib/gprintf.h>
#include "obex.h"

#define DEBUG_ON
#ifdef DEBUG_ON
#define DEBUG(fmt,args...) if(getenv("DEBUG_BT"))g_message("bt: " fmt,##args)
#else
#define DEBUG
#endif

static const gchar introspection_xml[] =
"<!DOCTYPE node PUBLIC \"-//freedesktop//DTD D-BUS Object Introspection 1.0//EN\"\n"
"\"http://www.freedesktop.org/standards/dbus/1.0/introspect.dtd\";>\n"
"<node>\n"
"   <interface name=\"org.bluez.obex.Agent1\">\n"
"       <method name=\"Release\">\n"
"       </method>\n"
"       <method name=\"AuthorizePush\">\n"
"           <arg name=\"device\" type=\"o\" direction=\"in\"/>\n"
"           <arg type=\"s\" direction=\"out\"/>\n"
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

static void initialise (Obex *obex);
static void clear (Obex *obex);

static void cb_name_owned (GDBusConnection *connection, const gchar *name, const gchar *owner, gpointer user_data);
static void cb_name_unowned (GDBusConnection *connection, const gchar *name, gpointer user_data);
static void cb_object_added (GDBusObjectManager *manager, GDBusObject *object, gpointer user_data);
static void cb_object_removed (GDBusObjectManager *manager, GDBusObject *object, gpointer user_data);

static void register_agent (Obex *obex);

static gint del_confirm_dlg (GtkWidget *widget, GdkEvent *event, Obex *obex);
static void accept_file (GtkButton *button, Obex *obex);
static void cancel_file (GtkButton *button, Obex *obex);


void send_notification (const gchar *title, const gchar *text, const gchar *icon_name)
{
    GApplication *application = g_application_new("plugin.bluetooth", G_APPLICATION_FLAGS_NONE);
    g_application_register (application, NULL, NULL);
    GNotification *notification = g_notification_new (title);
	g_notification_set_body (notification, text);
	GIcon *icon = g_themed_icon_new (icon_name);
	g_notification_set_icon (notification, icon);
	g_application_send_notification (application, NULL, notification);
	g_object_unref (icon);
	g_object_unref (notification);
	g_object_unref (application);
}

// Automatically gets a new filename of an existing file. eg. test.jpg -> test1.jpg
gchar* renamed_path (const gchar *oldname, const gchar *dir)
{
    if (oldname == NULL || dir==NULL)
        return NULL;
    gchar *tmp, *newname, *path=NULL;
    gchar **frags = g_strsplit(oldname, ".", -1);
    // get number of fragments
    int count = 0;
    while (frags[count]!=NULL) count++;

    for (int version=1; ; version++)
    {
        if (count==1) {// no '.' in filename
            newname = g_strdup_printf("%s%d", frags[0], version);
        }
        else {
            newname = g_strdup(frags[0]);
            // append version number just before extension
            for (int i=1; i<count; i++) {
                if (i==count-1)
                    tmp = g_strdup_printf("%s%d.%s", newname, version, frags[i]);
                else {
                    tmp = g_strjoin(".", newname, frags[i], NULL);
                }
                g_free(newname);
                newname = tmp;
            }
        }
        path = g_build_path("/", dir, newname, NULL);

        if (!g_file_test (path, G_FILE_TEST_EXISTS)) {
            g_free(newname);
            break;
        }
        g_free(path);
    }
    g_strfreev(frags);
    return path;
}

static void show_accept_file_dialog (Obex *obex)
{
    GtkWidget *accept_btn, *cancel_btn;
    obex->confirm_dlg = gtk_message_dialog_new (NULL, GTK_DIALOG_MODAL, GTK_MESSAGE_INFO,
                         GTK_BUTTONS_NONE, "Do you want to receive file \n%s \nfrom %s ?",
                                            obex->filename, obex->dev_name);
    gtk_window_set_icon_name (GTK_WINDOW (obex->confirm_dlg), "preferences-system-bluetooth");

    accept_btn = gtk_dialog_add_button (GTK_DIALOG (obex->confirm_dlg), _("_Accept"), 1);
    cancel_btn = gtk_dialog_add_button (GTK_DIALOG (obex->confirm_dlg), _("_Cancel"), 0);
    g_signal_connect (accept_btn, "clicked", G_CALLBACK (accept_file), obex);
    g_signal_connect (cancel_btn, "clicked", G_CALLBACK (cancel_file), obex);
    g_signal_connect (obex->confirm_dlg, "delete_event", G_CALLBACK (del_confirm_dlg), obex);
    gtk_widget_show_all (obex->confirm_dlg);
}

static gint del_confirm_dlg (GtkWidget *widget, GdkEvent *event, Obex *obex)
{
    cancel_file (NULL, obex);
    return TRUE;
}

static void accept_file (GtkButton *button, Obex *obex)
{
    if (obex->confirm_dlg) gtk_widget_destroy (obex->confirm_dlg);
    obex->confirm_dlg = NULL;

    GVariant *retvar = g_variant_new ("(s)", obex->filename);

    g_variant_ref_sink (retvar);
    g_dbus_method_invocation_return_value (obex->invocation, retvar);
    g_variant_unref (retvar);
}

static void cancel_file (GtkButton *button, Obex *obex)
{
    if (obex->confirm_dlg) gtk_widget_destroy (obex->confirm_dlg);
    obex->confirm_dlg = NULL;
    g_dbus_method_invocation_return_dbus_error (obex->invocation, "org.bluez.obex.Error.Rejected", "Pairing rejected by user");
}

// use this when g_dbus_proxy_get_cached_property() does not work
static GVariant* interface_get_property (GDBusInterface *interface, const gchar *name)
{
    GVariant *var, *res;
    const gchar *interface_name = g_dbus_proxy_get_interface_name (G_DBUS_PROXY(interface));

    GVariant *args = g_variant_new ("(ss)", interface_name, name);
    g_variant_ref_sink (args);
    var = g_dbus_proxy_call_sync (G_DBUS_PROXY(interface), "org.freedesktop.DBus.Properties.Get",
                                            args, G_DBUS_CALL_FLAGS_NONE, -1, NULL, NULL);
    g_variant_get(var, "(v)", &res);
    g_variant_unref(args);
    g_variant_unref(var);

    return res;
}

// Callback when a property of any DBus Object gets updated
static void
cb_properties_changed (GDBusObjectManagerClient *manager,
                       GDBusObjectProxy         *object_proxy,
                       GDBusProxy               *interface_proxy,
                       GVariant                 *changed_properties,
                       GStrv                     invalidated_properties,
                       gpointer                  user_data)
{
    Obex *obex = (Obex *) user_data;
    const gchar *obj_path = g_dbus_object_get_object_path (G_DBUS_OBJECT(object_proxy));

    GVariant *var = g_variant_lookup_value(changed_properties, "Status", NULL);
    if (var) {
        const gchar *status = g_variant_get_string (var, NULL);
        DEBUG ("Status : %s", status);
        // move file & show notification only if we are 'Receiving' files
        if (g_strcmp0 (status, "complete")==0 && g_str_has_prefix(obj_path, obex->session))
        {
            gchar *old_path = g_build_path("/", obex->root_path, obex->filename, NULL);
            gchar *new_path = g_build_path("/", obex->incoming_dir, obex->filename, NULL);
            // should not overwrite existing file
            if (g_file_test (new_path, G_FILE_TEST_EXISTS)) {
                g_free(new_path);
                new_path = renamed_path(obex->filename, obex->incoming_dir);
                g_free(obex->filename);
                obex->filename = g_path_get_basename (new_path);
            }
            g_rename(old_path, new_path);
            g_free (old_path);
            g_free (new_path);
            send_notification("File Received", obex->filename, "preferences-system-bluetooth");
        }
        g_variant_unref(var);
    }
}

/* Handler for method calls on the agent */

static void handle_method_call (GDBusConnection *connection, const gchar *sender,
                                const gchar *object_path, const gchar *interface_name,
                                const gchar *method_name, GVariant *parameters,
                                GDBusMethodInvocation *invocation, gpointer user_data)
{
    DEBUG ("Agent  %s()  %s", method_name, g_variant_print (parameters, TRUE));
    Obex *obex = (Obex*) user_data;
    obex->invocation = invocation;

    // when sender cancels transfer, or confirmation timeout is reached
    if (g_strcmp0 (method_name, "Cancel") == 0) {
        if (obex->confirm_dlg) gtk_widget_destroy (obex->confirm_dlg);
        obex->confirm_dlg = NULL;
        return;
    }

    GDBusInterface *transfer_if, *session_if;
    GVariant *name_var, *sess_var, *dest_var, *root_var, *var1, *var2, *var3;
    // A file is about to receive , we have to give permission
    if (g_strcmp0 (method_name, "AuthorizePush") != 0)  return;

    // objpath is like '/org/bluez/obex/server/session3/transfer2'
    const gchar *objpath = g_variant_get_string (g_variant_get_child_value (parameters, 0), NULL);
    transfer_if = g_dbus_object_manager_get_interface (obex->objmanager, objpath, "org.bluez.obex.Transfer1");

    g_free(obex->filename);
    // g_dbus_proxy_get_cached_property() returns NULL, so another way is used
    name_var = interface_get_property(transfer_if, "Name");
    obex->filename = g_variant_dup_string (name_var, NULL);
    DEBUG ("Name : %s", obex->filename);

    sess_var = g_dbus_proxy_get_cached_property(G_DBUS_PROXY(transfer_if), "Session");
    const gchar *session = g_variant_get_string(sess_var, NULL);
    // ask permission only for the first file in a session. auto accept others
    if ( g_strcmp0 (session, obex->session)==0 ) {
        accept_file(NULL, obex);
        goto end;
    }
    g_free(obex->session);
    obex->session = g_strdup(session);

    session_if = g_dbus_object_manager_get_interface (obex->objmanager, session, "org.bluez.obex.Session1");

    dest_var = g_dbus_proxy_get_cached_property(G_DBUS_PROXY(session_if), "Destination");
    const gchar *dest = g_variant_get_string(dest_var, NULL);

    // Obex writes files in the obex Root directory (~/cache/obexd)
    // So after receiving files, we have to move files to desired directory
    root_var = g_dbus_proxy_get_cached_property(G_DBUS_PROXY(session_if), "Root");
    g_free(obex->root_path);
    obex->root_path = g_variant_dup_string(root_var, NULL);
    //DEBUG ("Root : %s", obex->root_path);

    gboolean is_trusted = FALSE;

    GList *objects;
    GDBusObject *object;
    GDBusInterface *interface;
    // iterate all the objects, and get device which has same Address
    objects = g_dbus_object_manager_get_objects (obex->btobjmanager);
    while (objects != NULL)
    {
        object = (GDBusObject *) objects->data;
        interface = g_dbus_object_get_interface (object, "org.bluez.Device1");
        if (interface)
        {
            var1 = g_dbus_proxy_get_cached_property (G_DBUS_PROXY (interface), "Address");
            if ( g_strcmp0(g_variant_get_string (var1, NULL), dest) == 0) {
                var2 = g_dbus_proxy_get_cached_property (G_DBUS_PROXY (interface), "Trusted");
                is_trusted = g_variant_get_boolean (var2);
                var3 = g_dbus_proxy_get_cached_property (G_DBUS_PROXY (interface), "Alias");
                g_free(obex->dev_name);
                obex->dev_name = g_variant_dup_string (var3, NULL);
                g_variant_unref (var2);
                g_variant_unref (var3);
                break;
            }
            g_variant_unref (var1);
            g_object_unref (interface);
        }
        objects = objects->next;
    }
    g_list_free_full (objects, g_object_unref);

    DEBUG ("Device Name : %s", obex->dev_name);
    //DEBUG ("Trusted : %d", is_trusted);

    if (is_trusted && obex->accept_trusted) {
        accept_file(NULL, obex);
    }
    else {
        show_accept_file_dialog(obex);
    }

    g_variant_unref(dest_var);
    g_variant_unref(root_var);
    g_object_unref (session_if);
end:
    g_object_unref (transfer_if);
    g_variant_unref(name_var);
    g_variant_unref(sess_var);
}

static const GDBusInterfaceVTable interface_vtable =
{
    handle_method_call,
    NULL,
    NULL
};


/* Clear all the BlueZ OBEX data if the DBus connection is lost */

static void clear (Obex *obex)
{
    if (obex->objmanager) g_object_unref (obex->objmanager);
    obex->objmanager = NULL;
    if (obex->bus)
    {
        if (obex->agentmanager) {
            g_dbus_proxy_call_sync (obex->agentmanager, "UnregisterAgent", g_variant_new ("(o)", "/obexagent"), G_DBUS_CALL_FLAGS_NONE, -1, NULL, NULL);
            g_object_unref (obex->agentmanager);
            obex->agentmanager = NULL;
        }
        if (obex->agentid) g_dbus_connection_unregister_object (obex->bus, obex->agentid);
        g_object_unref (obex->bus);
    }
    obex->bus = NULL;
    obex->agentid = 0;
    obex->agentmanager = NULL;
    g_free(obex->dev_name);
    obex->dev_name = NULL;
    g_free(obex->filename);
    obex->filename = NULL;
    g_free(obex->session);
    obex->session = NULL;
}


static void initialise (Obex *obex)
{
    // make sure everything is reset - should be unnecessary...
    clear (obex);

    GError *error = NULL;
    GDBusNodeInfo *introspection_data;

    // get a connection to the session DBus
    obex->bus = g_bus_get_sync (G_BUS_TYPE_SESSION, NULL, &error);
    if (error)
    {
        DEBUG ("Error getting session bus - %s", error->message);
        g_error_free (error);
    }

    obex->objmanager = g_dbus_object_manager_client_new_sync (obex->bus, 0,
                    "org.bluez.obex", "/", NULL, NULL, NULL, NULL, &error);
    if (!error)
    {
        // register callbacks on object manager
        g_signal_connect (obex->objmanager, "object-added", G_CALLBACK (cb_object_added), obex);
        g_signal_connect (obex->objmanager, "object-removed", G_CALLBACK (cb_object_removed), obex);
        g_signal_connect (obex->objmanager, "interface-proxy-properties-changed", G_CALLBACK (cb_properties_changed), obex);
    }
    else {
        DEBUG ("Error getting obex object manager - %s", error->message);
        g_error_free (error);
    }

    // create the agent from XML spec
    introspection_data = g_dbus_node_info_new_for_xml (introspection_xml, &error);
    if (error)
    {
        DEBUG ("Error creating obex agent node - %s", error->message);
        g_error_free (error);
    }

    // export Agent object. This agent id is only used to unregister agent
    obex->agentid = g_dbus_connection_register_object (obex->bus, "/obexagent",
                introspection_data->interfaces[0], &interface_vtable, obex, NULL, &error);
    if (error)
    {
        DEBUG ("Error registering obex agent on bus - %s", error->message);
        g_error_free (error);
    }
    register_agent(obex);
    // clean up
    g_dbus_node_info_unref (introspection_data);
}

static void register_agent (Obex *obex)
{
    GError *error = NULL;
    GDBusInterface *newagentmanager = NULL;
    newagentmanager = g_dbus_object_manager_get_interface (obex->objmanager,
                                     "/org/bluez/obex", "org.bluez.obex.AgentManager1");

    GVariant *res;

    if (!newagentmanager)
    {
        DEBUG ("No obex agent manager found");
        if (obex->agentmanager) g_object_unref (obex->agentmanager);
        obex->agentmanager = NULL;
    }
    else if (G_DBUS_PROXY(newagentmanager) != obex->agentmanager)
    {
        DEBUG ("New obex agent manager found");
        // if there is already an agent manager, unregister the agent from it
        if (obex->agentmanager)
        {
            error = NULL;
            res = g_dbus_proxy_call_sync (obex->agentmanager, "UnregisterAgent", g_variant_new ("(o)", "/obexagent"), G_DBUS_CALL_FLAGS_NONE, -1, NULL, &error);
            if (error)
            {
                DEBUG ("Error unregistering obex agent with manager - %s", error->message);
                g_error_free (error);
            }
            g_object_unref (obex->agentmanager);
        }
        obex->agentmanager = G_DBUS_PROXY(newagentmanager);

        // register the agent with the new agent manager
        error = NULL;
        res = g_dbus_proxy_call_sync (obex->agentmanager, "RegisterAgent",
                    g_variant_new ("(o)", "/obexagent"), G_DBUS_CALL_FLAGS_NONE, -1, NULL, &error);
        if (error)
        {
            DEBUG ("Error registering obex agent with manager - %s", error->message);
            g_error_free (error);
        }
        if (res) g_variant_unref (res);
    }
}


static void cb_name_owned (GDBusConnection *connection, const gchar *name, const gchar *owner, gpointer user_data)
{
    Obex * obex = (Obex *) user_data;
    DEBUG ("Name %s owned on DBus", name);
    initialise (obex);
}

static void cb_name_unowned (GDBusConnection *connection, const gchar *name, gpointer user_data)
{
    Obex * obex = (Obex *) user_data;
    DEBUG ("Name %s unowned on DBus", name);
    clear (obex);
}

static void cb_object_added (GDBusObjectManager *manager, GDBusObject *object, gpointer user_data)
{
    Obex * obex = (Obex *) user_data;
    DEBUG ("Obex - object added at path %s", g_dbus_object_get_object_path (object));
    register_agent (obex);
}

static void cb_object_removed (GDBusObjectManager *manager, GDBusObject *object, gpointer user_data)
{
    Obex * obex = (Obex *) user_data;
    DEBUG ("Obex - object removed at path %s", g_dbus_object_get_object_path (object));
    register_agent (obex);
}


void obex_read_settings (Obex *obex, config_setting_t *settings)
{
    const char *str;
    int val;

    if (config_setting_lookup_string (settings, "IncomingDir", &str))
    {
        if ( g_file_test (str, G_FILE_TEST_EXISTS)) {
            g_free(obex->incoming_dir);
            obex->incoming_dir = g_strdup(str);
        }
    }
    if (config_setting_lookup_int (settings, "AcceptTrusted", &val))
    {
        obex->accept_trusted = val;
    }
}

Obex* obex_create ()
{
    Obex *obex = g_new0 (Obex, 1);

    g_bus_watch_name (G_BUS_TYPE_SESSION, "org.bluez.obex", G_BUS_NAME_WATCHER_FLAGS_AUTO_START,
                            cb_name_owned, cb_name_unowned, obex, NULL);
    obex->accept_trusted = 1;
    obex->incoming_dir = g_build_path("/", g_get_home_dir(), "Downloads", NULL);
    // if ~/Downloads does not exist, fall back to home dir
    if (! g_file_test (obex->incoming_dir, G_FILE_TEST_EXISTS)) {
        g_free (obex->incoming_dir);
        obex->incoming_dir = g_strdup(g_get_home_dir());
    }
    return obex;
}

// call it to free memory only when plugin is destroyed
void obex_destroy (Obex *obex)
{
    clear (obex);
    g_free (obex->incoming_dir);
    g_free (obex);
}
