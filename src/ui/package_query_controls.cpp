// -----------------------------------------------------------------------------
// package_query_controls.cpp
// Package query request state and shared UI completion helpers
//
// Owns the small pieces of package query state that are shared by search, list,
// cancellation, and reload flows.
// -----------------------------------------------------------------------------
#include "package_query_controller_internal.hpp"

#include "i18n.hpp"
#include "package_info_controller.hpp"
#include "package_table_view.hpp"
#include "ui_helpers.hpp"
#include "widgets.hpp"
#include "widgets_internal.hpp"

// -----------------------------------------------------------------------------
// Remember which main query flow produced the currently displayed table.
// Transaction and repository rebuilds use this to rerun the same query and
// replace outdated rows with fresh backend data.
// -----------------------------------------------------------------------------
void
package_query_set_displayed_query_kind(SearchWidgets *widgets, DisplayedPackageQueryKind kind)
{
  if (!widgets) {
    return;
  }

  widgets->query_state.displayed_query = DisplayedPackageQueryState();
  widgets->query_state.displayed_query.kind = kind;
}

// -----------------------------------------------------------------------------
// Preserve the active search term and flags so a post-transaction refresh can
// rebuild the visible search results even if the user changes the checkboxes
// while the background work is still running.
// -----------------------------------------------------------------------------
void
package_query_set_displayed_search_query(SearchWidgets *widgets,
                                         const std::string &term,
                                         bool search_in_description,
                                         bool exact_match)
{
  if (!widgets) {
    return;
  }

  widgets->query_state.displayed_query = DisplayedPackageQueryState();
  widgets->query_state.displayed_query.kind = DisplayedPackageQueryKind::SEARCH;
  widgets->query_state.displayed_query.search_term = term;
  widgets->query_state.displayed_query.search_in_description = search_in_description;
  widgets->query_state.displayed_query.exact_match = exact_match;
}

// -----------------------------------------------------------------------------
// Complete one rebuild-triggered refresh. When the old selection survived the
// refreshed query result, leave the details pane intact; otherwise clear it so
// outdated package info is not shown for rows that disappeared.
// -----------------------------------------------------------------------------
void
package_query_finish_results_refresh(SearchWidgets *widgets)
{
  if (!widgets) {
    return;
  }

  if (widgets->query_state.preserve_selection_on_reload) {
    PackageRow selected;
    if (!package_table_get_selected_package_row(widgets, selected)) {
      package_info_reset_details_view(widgets);
    }
  } else {
    package_info_reset_details_view(widgets);
  }

  widgets->query_state.preserve_selection_on_reload = false;
  widgets->query_state.reload_selected_nevra.clear();
}

// -----------------------------------------------------------------------------
// Return true when a package list task is currently running.
// -----------------------------------------------------------------------------
bool
package_query_has_active_package_list_request(const SearchWidgets *widgets)
{
  return widgets && widgets->query_state.package_list_cancellable &&
      !g_cancellable_is_cancelled(widgets->query_state.package_list_cancellable);
}

// -----------------------------------------------------------------------------
// Return the button that currently works as Stop.
// -----------------------------------------------------------------------------
static GtkButton *
package_list_stop_button(SearchWidgets *widgets, PackageListRequestKind kind)
{
  if (!widgets) {
    return nullptr;
  }

  switch (kind) {
  case PackageListRequestKind::LIST_INSTALLED:
    return widgets->query.list_button;
  case PackageListRequestKind::LIST_AVAILABLE:
    return widgets->query.list_available_button;
  case PackageListRequestKind::LIST_UPGRADEABLE:
    return widgets->query.list_upgradeable_button;
  case PackageListRequestKind::SEARCH:
  case PackageListRequestKind::NONE:
  default:
    return widgets->query.search_button;
  }
}

// -----------------------------------------------------------------------------
// Human-readable cancel status for the current background package list request.
// -----------------------------------------------------------------------------
static const char *
package_list_cancelled_status(PackageListRequestKind kind)
{
  switch (kind) {
  case PackageListRequestKind::SEARCH:
    return _("Search cancelled.");
  case PackageListRequestKind::LIST_INSTALLED:
    return _("Listing installed packages cancelled.");
  case PackageListRequestKind::LIST_AVAILABLE:
    return _("Listing packages cancelled.");
  case PackageListRequestKind::LIST_UPGRADEABLE:
    return _("Listing upgradable packages cancelled.");
  case PackageListRequestKind::NONE:
  default:
    return _("Operation cancelled.");
  }
}

// -----------------------------------------------------------------------------
// Record the active package list task and switch its button to Stop.
// -----------------------------------------------------------------------------
void
package_query_begin_package_list_request(SearchWidgets *widgets,
                                         GCancellable *c,
                                         uint64_t request_id,
                                         PackageListRequestKind kind)
{
  if (!widgets || !c) {
    return;
  }

  if (widgets->query_state.package_list_cancellable) {
    g_object_unref(widgets->query_state.package_list_cancellable);
  }

  widgets->query_state.package_list_cancellable = G_CANCELLABLE(g_object_ref(c));
  widgets->query_state.current_package_list_request_id = request_id;
  widgets->query_state.current_package_list_request_kind = kind;
  GtkButton *stop_button = package_list_stop_button(widgets, kind);

  ui_helpers_set_icon_button(widgets->query.search_button, "system-search-symbolic", _("Search"));
  ui_helpers_set_icon_button(widgets->query.list_button, "view-list-symbolic", _("List Installed"));
  ui_helpers_set_icon_button(widgets->query.list_available_button, "view-list-symbolic", _("List Packages"));
  ui_helpers_set_icon_button(widgets->query.list_upgradeable_button, "view-list-symbolic", _("List Upgradable"));
  ui_helpers_set_icon_button(stop_button, "process-stop-symbolic", _("Stop"));
  gtk_widget_set_sensitive(GTK_WIDGET(widgets->query.entry), FALSE);
  gtk_widget_set_sensitive(GTK_WIDGET(widgets->query.desc_checkbox), FALSE);
  gtk_widget_set_sensitive(GTK_WIDGET(widgets->query.exact_checkbox), FALSE);
  gtk_widget_set_sensitive(GTK_WIDGET(widgets->query.history_list), FALSE);
  gtk_widget_set_sensitive(GTK_WIDGET(widgets->query.search_button), FALSE);
  gtk_widget_set_sensitive(GTK_WIDGET(widgets->query.list_button), FALSE);
  gtk_widget_set_sensitive(GTK_WIDGET(widgets->query.list_available_button), FALSE);
  gtk_widget_set_sensitive(GTK_WIDGET(widgets->query.list_upgradeable_button), FALSE);
  gtk_widget_set_sensitive(GTK_WIDGET(stop_button), TRUE);
}

// -----------------------------------------------------------------------------
// Restore the normal search and list controls after a package query stops or finishes.
// -----------------------------------------------------------------------------
static void
restore_package_list_controls(SearchWidgets *widgets)
{
  if (!widgets) {
    return;
  }

  ui_helpers_set_icon_button(widgets->query.search_button, "system-search-symbolic", _("Search"));
  ui_helpers_set_icon_button(widgets->query.list_button, "view-list-symbolic", _("List Installed"));
  ui_helpers_set_icon_button(widgets->query.list_available_button, "view-list-symbolic", _("List Packages"));
  ui_helpers_set_icon_button(widgets->query.list_upgradeable_button, "view-list-symbolic", _("List Upgradable"));
  gtk_widget_set_sensitive(GTK_WIDGET(widgets->query.entry), TRUE);
  gtk_widget_set_sensitive(GTK_WIDGET(widgets->query.desc_checkbox), TRUE);
  gtk_widget_set_sensitive(GTK_WIDGET(widgets->query.exact_checkbox), TRUE);
  gtk_widget_set_sensitive(GTK_WIDGET(widgets->query.history_list), TRUE);
  gtk_widget_set_sensitive(GTK_WIDGET(widgets->query.list_button), TRUE);
  gtk_widget_set_sensitive(GTK_WIDGET(widgets->query.list_available_button), TRUE);
  gtk_widget_set_sensitive(GTK_WIDGET(widgets->query.list_upgradeable_button), TRUE);
  gtk_widget_set_sensitive(GTK_WIDGET(widgets->query.search_button), TRUE);
}

// -----------------------------------------------------------------------------
// Restore the package list controls when the active task is done.
// -----------------------------------------------------------------------------
void
package_query_end_package_list_request(SearchWidgets *widgets, uint64_t request_id, PackageListRequestKind kind)
{
  if (!widgets || widgets->query_state.current_package_list_request_id != request_id ||
      widgets->query_state.current_package_list_request_kind != kind) {
    return;
  }

  if (widgets->query_state.package_list_cancellable) {
    g_object_unref(widgets->query_state.package_list_cancellable);
    widgets->query_state.package_list_cancellable = nullptr;
  }
  widgets->query_state.current_package_list_request_id = 0;
  widgets->query_state.current_package_list_request_kind = PackageListRequestKind::NONE;
  restore_package_list_controls(widgets);
}

// -----------------------------------------------------------------------------
// Cancel the active package list request and immediately unlock the shared controls.
// -----------------------------------------------------------------------------
void
package_query_cancel_active_package_list_request(SearchWidgets *widgets)
{
  if (!widgets || !widgets->query_state.package_list_cancellable) {
    return;
  }

  uint64_t request_id = widgets->query_state.current_package_list_request_id;
  PackageListRequestKind kind = widgets->query_state.current_package_list_request_kind;
  GCancellable *c = widgets->query_state.package_list_cancellable;
  if (!g_cancellable_is_cancelled(c)) {
    g_cancellable_cancel(c);
  }

  // Release only the spinner slot owned by this request so other running tasks
  // can keep their progress indication visible.
  widgets_spinner_release(widgets->query.spinner);

  // Clear the active request after its controls have been restored.
  package_query_end_package_list_request(widgets, request_id, kind);

  ui_helpers_set_status(widgets->query.status_label, package_list_cancelled_status(kind), "gray");
}

// -----------------------------------------------------------------------------
// EOF
// -----------------------------------------------------------------------------
