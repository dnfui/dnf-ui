// -----------------------------------------------------------------------------
// src/ui/window/main_menu.cpp
// Main window menu bar
// Keeps secondary application actions in the top menu instead of crowding the main package workflow toolbar.
// -----------------------------------------------------------------------------
#include "ui/window/main_menu.hpp"

#include "i18n.hpp"
#include "ui/package_query/package_query_controller.hpp"
#include "ui/package_table/package_table_columns.hpp"
#include "ui/package_table/package_table_view.hpp"
#include "ui/common/ui_helpers.hpp"
#include "ui/common/widgets.hpp"

#include <string>

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
// Return the menu action name for one package table column.
// -----------------------------------------------------------------------------
static std::string
column_action_name_for_id(const char *column_id)
{
  std::string action_name = "column-";
  if (column_id) {
    action_name += column_id;
  }
  return action_name;
}

// -----------------------------------------------------------------------------
// Return the package table column id handled by one menu action.
// -----------------------------------------------------------------------------
static std::string
column_id_for_action_name(const char *action_name)
{
  constexpr const char *prefix = "column-";
  if (!action_name) {
    return {};
  }

  std::string name = action_name;
  if (name.rfind(prefix, 0) != 0) {
    return {};
  }

  return name.substr(std::string(prefix).size());
}

// -----------------------------------------------------------------------------
// Update View menu column checkmarks from the saved column settings.
// -----------------------------------------------------------------------------
static void
sync_column_action_states(GSimpleActionGroup *actions)
{
  if (!actions) {
    return;
  }

  for (const auto &column : package_table_column_infos()) {
    std::string action_name = column_action_name_for_id(column.id);
    GAction *action = g_action_map_lookup_action(G_ACTION_MAP(actions), action_name.c_str());
    if (action && G_IS_SIMPLE_ACTION(action)) {
      g_simple_action_set_state(G_SIMPLE_ACTION(action),
                                g_variant_new_boolean(package_table_column_is_visible(column.id)));
    }
  }
}

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
// Show or hide one package table column from the View menu.
// -----------------------------------------------------------------------------
static void
on_menu_column_visibility_changed(GSimpleAction *action, GVariant *value, gpointer user_data)
{
  MainMenuActionData *data = static_cast<MainMenuActionData *>(user_data);
  if (!data || !data->widgets || !action || !value) {
    return;
  }

  std::string column_id = column_id_for_action_name(g_action_get_name(G_ACTION(action)));
  if (column_id.empty()) {
    return;
  }

  gboolean visible = g_variant_get_boolean(value);
  if (package_table_set_column_visible(data->widgets, column_id.c_str(), visible)) {
    g_simple_action_set_state(action, value);
  } else {
    ui_helpers_set_status(
        data->widgets->query.status_label, _("At least one package table column must remain visible."), "blue");
  }
}

// -----------------------------------------------------------------------------
// Restore package table columns to their default visibility.
// -----------------------------------------------------------------------------
static void
on_menu_restore_default_columns(GSimpleAction *, GVariant *, gpointer user_data)
{
  MainMenuActionData *data = static_cast<MainMenuActionData *>(user_data);
  if (!data || !data->widgets || !data->window) {
    return;
  }

  package_table_reset_columns_to_default(data->widgets);
  GSimpleActionGroup *actions =
      static_cast<GSimpleActionGroup *>(g_object_get_data(G_OBJECT(data->window), "dnfui-menu-action-group"));
  sync_column_action_states(actions);
  ui_helpers_set_status(data->widgets->query.status_label, _("Package table columns restored to defaults."), "green");
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
  GMenu *columns_menu = g_menu_new();
  for (const auto &column : package_table_column_infos()) {
    std::string detailed_action = "win.";
    detailed_action += column_action_name_for_id(column.id);
    g_menu_append(columns_menu, _(column.title), detailed_action.c_str());
  }
  g_menu_append(columns_menu, _("Restore Default Columns"), "win.restore-default-columns");
  g_menu_append_submenu(view_menu, _("Columns"), G_MENU_MODEL(columns_menu));
  g_object_unref(columns_menu);
  g_menu_append_submenu(menu_bar, _("View"), G_MENU_MODEL(view_menu));
  g_object_unref(view_menu);

  GMenu *package_menu = g_menu_new();
  g_menu_append(package_menu, _("Clear List"), "win.clear-list");
  g_menu_append(package_menu, _("Clear Search Cache"), "win.clear-cache");
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
  entries[6].name = "restore-default-columns";
  entries[6].activate = on_menu_restore_default_columns;

  GSimpleActionGroup *actions = g_simple_action_group_new();
  g_action_map_add_action_entries(G_ACTION_MAP(actions), entries, G_N_ELEMENTS(entries), data);
  for (const auto &column : package_table_column_infos()) {
    std::string action_name = column_action_name_for_id(column.id);
    GSimpleAction *action = g_simple_action_new_stateful(
        action_name.c_str(), nullptr, g_variant_new_boolean(package_table_column_is_visible(column.id)));
    g_signal_connect(action, "change-state", G_CALLBACK(on_menu_column_visibility_changed), data);
    g_action_map_add_action(G_ACTION_MAP(actions), G_ACTION(action));
    g_object_unref(action);
  }
  gtk_widget_insert_action_group(menu_widgets.window, "win", G_ACTION_GROUP(actions));
  g_object_set_data_full(G_OBJECT(menu_widgets.window), "dnfui-menu-action-group", actions, g_object_unref);
}

// -----------------------------------------------------------------------------
// EOF
// -----------------------------------------------------------------------------
