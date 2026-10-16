#pragma once

#include <gtk/gtk.h>

// -----------------------------------------------------------------------------
// Follow GNOME's color preference without overriding other desktops or GTK_THEME.
// -----------------------------------------------------------------------------
void setup_color_scheme(GtkApplication *app);
