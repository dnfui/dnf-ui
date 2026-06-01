// -----------------------------------------------------------------------------
// src/ui/main_menu.cpp
// Main window menu bar
// Keeps secondary application actions in the top menu instead of crowding the main package workflow toolbar.
// -----------------------------------------------------------------------------
#include "main_menu.hpp"

#include "i18n.hpp"
#include "package_query_controller.hpp"
#include "pending_transaction_apply.hpp"
#include "ui_helpers.hpp"
#include "widgets.hpp"

#ifndef DNFUI_VERSION
#define DNFUI_VERSION "unknown"
#endif

struct MainMenuActionData {
  SearchWidgets *widgets = nullptr;
  GtkWidget *window = nullptr;
  GtkWidget *history_panel = nullptr;
  GtkWidget *info_panel = nullptr;
};

// -----------------------------------------------------------------------------
// Menu action callbacks
// -----------------------------------------------------------------------------
static void
on_menu_clear_list(GSimpleAction *, GVariant *, gpointer user_data)
{
  MainMenuActionData *data = static_cast<MainMenuActionData *>(user_data);
  if (!data || !data->widgets) {
    return;
  }

  package_query_on_clear_button_clicked(nullptr, data->widgets);
}

// -----------------------------------------------------------------------------
// Clear cached package search results from the menu.
// -----------------------------------------------------------------------------
static void
on_menu_clear_cache(GSimpleAction *, GVariant *, gpointer user_data)
{
  MainMenuActionData *data = static_cast<MainMenuActionData *>(user_data);
  if (!data || !data->widgets) {
    return;
  }

  package_query_clear_search_cache();
  ui_helpers_set_status(data->widgets->query.status_label, _("Search cache cleared."), "green");
}

// -----------------------------------------------------------------------------
// Close the main window from the menu.
// -----------------------------------------------------------------------------
static void
on_menu_quit(GSimpleAction *, GVariant *, gpointer user_data)
{
  MainMenuActionData *data = static_cast<MainMenuActionData *>(user_data);
  if (!data || !data->window) {
    return;
  }

  gtk_window_close(GTK_WINDOW(data->window));
}

// -----------------------------------------------------------------------------
// Show the application About dialog.
// -----------------------------------------------------------------------------
static void
on_menu_about(GSimpleAction *, GVariant *, gpointer user_data)
{
  MainMenuActionData *data = static_cast<MainMenuActionData *>(user_data);
  if (!data || !data->window) {
    return;
  }

  const char *authors[] = {
    "ErikMN",
    nullptr,
  };

  gtk_show_about_dialog(GTK_WINDOW(data->window),
                        "program-name",
                        _("DNF UI"),
                        "version",
                        DNFUI_VERSION,
                        "comments",
                        _("Graphical package manager frontend for Fedora."),
                        "website",
                        "https://github.com/ErikMN/dnf-ui",
                        "website-label",
                        _("GitHub repository"),
                        "authors",
                        authors,
                        "logo-icon-name",
                        "com.fedora.dnfui",
                        "license-type",
                        GTK_LICENSE_MIT_X11,
                        nullptr);
}

// -----------------------------------------------------------------------------
// Show or hide the history panel from the menu.
// -----------------------------------------------------------------------------
static void
on_menu_show_history_changed(GSimpleAction *action, GVariant *value, gpointer user_data)
{
  MainMenuActionData *data = static_cast<MainMenuActionData *>(user_data);
  if (!data || !data->history_panel || !value) {
    return;
  }

  gboolean visible = g_variant_get_boolean(value);
  gtk_widget_set_visible(data->history_panel, visible);
  g_simple_action_set_state(action, value);
}

// -----------------------------------------------------------------------------
// Show or hide the package info panel from the menu.
// -----------------------------------------------------------------------------
static void
on_menu_show_info_changed(GSimpleAction *action, GVariant *value, gpointer user_data)
{
  MainMenuActionData *data = static_cast<MainMenuActionData *>(user_data);
  if (!data || !data->info_panel || !value) {
    return;
  }

  gboolean visible = g_variant_get_boolean(value);
  gtk_widget_set_visible(data->info_panel, visible);
  g_simple_action_set_state(action, value);
}

// -----------------------------------------------------------------------------
// Show or hide required package changes in the package table.
// -----------------------------------------------------------------------------
static void
on_menu_show_required_package_changes_changed(GSimpleAction *action, GVariant *value, gpointer user_data)
{
  MainMenuActionData *data = static_cast<MainMenuActionData *>(user_data);
  if (!data || !data->widgets || !value) {
    return;
  }

  gboolean enabled = g_variant_get_boolean(value);
  data->widgets->transaction.show_required_package_changes = enabled;
  g_simple_action_set_state(action, value);
  pending_transaction_refresh_affected_packages(data->widgets);
}

// -----------------------------------------------------------------------------
// Build the top menu bar shown above the package workflow controls
// -----------------------------------------------------------------------------
GtkWidget *
main_menu_create()
{
  GMenu *menu_bar = g_menu_new();

  GMenu *file_menu = g_menu_new();
  g_menu_append(file_menu, _("Quit"), "win.quit");
  g_menu_append_submenu(menu_bar, _("File"), G_MENU_MODEL(file_menu));
  g_object_unref(file_menu);

  GMenu *view_menu = g_menu_new();
  g_menu_append(view_menu, _("History Panel"), "win.show-history");
  g_menu_append(view_menu, _("Package Info Panel"), "win.show-info");
  g_menu_append_submenu(menu_bar, _("View"), G_MENU_MODEL(view_menu));
  g_object_unref(view_menu);

  GMenu *package_menu = g_menu_new();
  g_menu_append(package_menu, _("Clear List"), "win.clear-list");
  g_menu_append(package_menu, _("Clear Search Cache"), "win.clear-cache");
  g_menu_append(package_menu, _("Show Required Package Changes"), "win.show-required-package-changes");
  g_menu_append_submenu(menu_bar, _("Package"), G_MENU_MODEL(package_menu));
  g_object_unref(package_menu);

  GMenu *help_menu = g_menu_new();
  g_menu_append(help_menu, _("About DNF UI"), "win.about");
  g_menu_append_submenu(menu_bar, _("Help"), G_MENU_MODEL(help_menu));
  g_object_unref(help_menu);

  GtkWidget *menu = gtk_popover_menu_bar_new_from_model(G_MENU_MODEL(menu_bar));
  g_object_unref(menu_bar);

  return menu;
}

// -----------------------------------------------------------------------------
// Connect menu actions to existing UI behavior
// -----------------------------------------------------------------------------
void
main_menu_connect_actions(const MainMenuWidgets &menu_widgets, SearchWidgets *widgets)
{
  MainMenuActionData *data = new MainMenuActionData();
  data->widgets = widgets;
  data->window = menu_widgets.window;
  data->history_panel = menu_widgets.history_panel;
  data->info_panel = menu_widgets.info_panel;
  g_object_set_data_full(
      G_OBJECT(menu_widgets.window), "dnfui-menu-action-data", data, +[](gpointer p) {
        delete static_cast<MainMenuActionData *>(p);
      });

  GActionEntry entries[7] = {};
  entries[0].name = "quit";
  entries[0].activate = on_menu_quit;
  entries[1].name = "clear-list";
  entries[1].activate = on_menu_clear_list;
  entries[2].name = "clear-cache";
  entries[2].activate = on_menu_clear_cache;
  entries[3].name = "show-history";
  entries[3].state = "true";
  entries[3].change_state = on_menu_show_history_changed;
  entries[4].name = "show-info";
  entries[4].state = "true";
  entries[4].change_state = on_menu_show_info_changed;
  entries[5].name = "about";
  entries[5].activate = on_menu_about;
  entries[6].name = "show-required-package-changes";
  entries[6].state = "false";
  entries[6].change_state = on_menu_show_required_package_changes_changed;

  GSimpleActionGroup *actions = g_simple_action_group_new();
  g_action_map_add_action_entries(G_ACTION_MAP(actions), entries, G_N_ELEMENTS(entries), data);
  gtk_widget_insert_action_group(menu_widgets.window, "win", G_ACTION_GROUP(actions));
  g_object_set_data_full(G_OBJECT(menu_widgets.window), "dnfui-menu-action-group", actions, g_object_unref);
}

// -----------------------------------------------------------------------------
// EOF
// -----------------------------------------------------------------------------
