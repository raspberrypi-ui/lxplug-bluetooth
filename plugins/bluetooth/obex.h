#pragma once
#include <gio/gio.h>
#include "plugin.h"

typedef struct {
    GDBusConnection *bus;
    GDBusObjectManager *btobjmanager;//object manager for org.bluez
    GDBusObjectManager *objmanager;// object manager for org.bluez.obex
    GDBusProxy *agentmanager;
    guint agentid;
    GDBusMethodInvocation *invocation;// for returning values of callbacks
    gchar *session;
    gchar *dev_name;//Alias
    gchar *filename;
    gchar *root_path;
    // Transfer settings
    int accept_trusted;
    gchar *incoming_dir;
    // Accept file confirmation dialog
    GtkWidget *confirm_dlg;
} Obex;


Obex* obex_create ();
void obex_destroy (Obex *obex);

void obex_read_settings (Obex *obex, config_setting_t *settings);
// Show desktop notification
void send_notification (const gchar *title, const gchar *text, const gchar *icon_name);
