#define ICON_CACHE_SIZE 14

/* Plug-in global data */

typedef struct {
#ifdef LXPLUG
    LXPanel *panel;                 /* Back pointer to panel */
    config_setting_t *settings;     /* Plugin settings */
#else
    int icon_size;                  /* Variables used under wf-panel */
    gboolean bottom;
    gboolean wizard;
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
} BluetoothPlugin;

