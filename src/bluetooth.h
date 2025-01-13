/*============================================================================
Copyright (c) 2018-2025 Raspberry Pi Holdings Ltd.
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
============================================================================*/

/*----------------------------------------------------------------------------*/
/* Typedefs and macros */
/*----------------------------------------------------------------------------*/

#define ICON_CACHE_SIZE 14

typedef struct {
#ifdef LXPLUG
    LXPanel *panel;                 /* Back pointer to panel */
    config_setting_t *settings;     /* Plugin settings */
#else
    int icon_size;                  /* Variables used under wf-panel */
    gboolean bottom;
    GtkGesture *gesture;
#endif
    GtkWidget *plugin;              /* Back pointer to the widget */
    GtkWidget *tray_icon;           /* Displayed image */
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
    guint hid_autopair;
    guint watch;
    gboolean rfkill;
    gboolean wizard;
} BluetoothPlugin;

/* End of file */
/*----------------------------------------------------------------------------*/
