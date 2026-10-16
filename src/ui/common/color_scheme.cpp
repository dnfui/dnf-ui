#include "ui/common/color_scheme.hpp"

// -----------------------------------------------------------------------------
// Apply GNOME's color preference to GTK.
// -----------------------------------------------------------------------------
static void
on_color_scheme_changed(GSettings *settings, gchar *, gpointer)
{
  gchar *scheme = g_settings_get_string(settings, "color-scheme");
  // Named themes still use this setting to select their dark variant despite its deprecation.
  // Changing gtk-interface-color-scheme can discard a theme's loaded resources.
  g_object_set(
      gtk_settings_get_default(), "gtk-application-prefer-dark-theme", g_strcmp0(scheme, "prefer-dark") == 0, nullptr);
  g_free(scheme);
}

#if GTK_CHECK_VERSION(4, 20, 0)
// -----------------------------------------------------------------------------
// Reload the named theme after GTK updates its system color preference.
// GTK can discard theme resources while reloading color-scheme CSS.
// -----------------------------------------------------------------------------
static void
on_gtk_color_scheme_changed(GtkSettings *, GParamSpec *, gpointer user_data)
{
  on_color_scheme_changed(G_SETTINGS(user_data), nullptr, nullptr);
}
#endif

// -----------------------------------------------------------------------------
// Follow GNOME's color preference without overriding other desktops or GTK_THEME.
// -----------------------------------------------------------------------------
void
setup_color_scheme(GtkApplication *app)
{
  const char *theme = g_getenv("GTK_THEME");
  if (theme && *theme) {
    return;
  }

  const char *desktop = g_getenv("XDG_CURRENT_DESKTOP");
  gchar **desktops = g_strsplit(desktop ? desktop : "", ":", -1);
  bool is_gnome = false;
  for (gchar **name = desktops; *name; ++name) {
    if (g_strcmp0(*name, "GNOME") == 0) {
      is_gnome = true;
      break;
    }
  }
  g_strfreev(desktops);
  if (!is_gnome) {
    return;
  }

  GSettingsSchemaSource *source = g_settings_schema_source_get_default();
  GSettingsSchema *schema =
      source ? g_settings_schema_source_lookup(source, "org.gnome.desktop.interface", TRUE) : nullptr;
  if (!schema) {
    return;
  }

  if (!g_settings_schema_has_key(schema, "color-scheme")) {
    g_settings_schema_unref(schema);
    return;
  }

  GSettings *settings = g_settings_new_full(schema, nullptr, nullptr);
  g_settings_schema_unref(schema);
  g_signal_connect(settings, "changed::color-scheme", G_CALLBACK(on_color_scheme_changed), nullptr);
#if GTK_CHECK_VERSION(4, 20, 0)
  // GTK 4.24 forwards the dark-theme setting to the color-scheme setting.
  // Connecting the reload handler there would create a notification loop.
  if (gtk_check_version(4, 24, 0) != nullptr) {
    g_signal_connect_object(gtk_settings_get_default(),
                            "notify::gtk-interface-color-scheme",
                            G_CALLBACK(on_gtk_color_scheme_changed),
                            settings,
                            G_CONNECT_DEFAULT);
  }
#endif
  on_color_scheme_changed(settings, nullptr, nullptr);
  g_object_set_data_full(G_OBJECT(app), "dnfui-color-scheme-settings", settings, g_object_unref);
}
