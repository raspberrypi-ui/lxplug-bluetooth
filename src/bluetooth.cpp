/*============================================================================
Copyright (c) 2024 Raspberry Pi Holdings Ltd.
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

#include <glibmm.h>
#include "gtk-utils.hpp"
#include "bluetooth.hpp"

extern "C" {
    WayfireWidget *create () { return new WayfireBluetooth; }
    void destroy (WayfireWidget *w) { delete w; }

    static constexpr conf_table_t conf_table[1] = {
        {CONF_NONE, NULL, NULL}
    };
    const conf_table_t *config_params (void) { return conf_table; };
    const char *display_name (void) { return N_("Bluetooth"); };
    const char *package_name (void) { return GETTEXT_PACKAGE; };
}

void WayfireBluetooth::bar_pos_changed_cb (void)
{
    if ((std::string) bar_pos == "bottom") bt->bottom = TRUE;
    else bt->bottom = FALSE;
}

void WayfireBluetooth::icon_size_changed_cb (void)
{
    bt->icon_size = icon_size;
    bt_update_display (bt);
}

void WayfireBluetooth::command (const char *cmd)
{
    bt_control_msg (bt, cmd);
}

bool WayfireBluetooth::set_icon (void)
{
    bt_update_display (bt);
    return false;
}

void WayfireBluetooth::init (Gtk::HBox *container)
{
    /* Create the button */
    plugin = std::make_unique <Gtk::Button> ();
    plugin->set_name (PLUGIN_NAME);
    container->pack_start (*plugin, false, false);

    /* Setup structure */
    bt = g_new0 (BluetoothPlugin, 1);
    bt->plugin = (GtkWidget *)((*plugin).gobj());
    bt->icon_size = icon_size;
    icon_timer = Glib::signal_idle().connect (sigc::mem_fun (*this, &WayfireBluetooth::set_icon));
    bar_pos_changed_cb ();

    /* Add long press for right click */
    gesture = add_longpress_default (*plugin);

    /* Initialise the plugin */
    bt_init (bt);

    /* Setup callbacks */
    icon_size.set_callback (sigc::mem_fun (*this, &WayfireBluetooth::icon_size_changed_cb));
    bar_pos.set_callback (sigc::mem_fun (*this, &WayfireBluetooth::bar_pos_changed_cb));
}

WayfireBluetooth::~WayfireBluetooth()
{
    icon_timer.disconnect ();
    bt_destructor (bt);
}

/* End of file */
/*----------------------------------------------------------------------------*/
