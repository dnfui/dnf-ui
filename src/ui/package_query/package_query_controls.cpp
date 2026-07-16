// -----------------------------------------------------------------------------
// package_query_controls.cpp
// Package query request state and shared UI completion helpers
//
// Owns the small pieces of package query state shared by search, list, cancellation, and reload flows.
// -----------------------------------------------------------------------------
#include "ui/package_query/package_query_controller_internal.hpp"

#include "i18n.hpp"
#include "ui/details/package_details_controller.hpp"
#include "ui/package_table/package_table_view.hpp"
#include "ui/common/ui_helpers.hpp"
#include "ui/common/widgets.hpp"
#include "ui/common/widgets_internal.hpp"

// -----------------------------------------------------------------------------
// Remember which main query flow produced the currently displayed table.
// Transaction and repository rebuilds use this to rerun the same query with fresh backend data.
// -----------------------------------------------------------------------------
void
package_query_set_displayed_query_kind(MainWindowUiState *widgets, DisplayedPackageQueryKind kind)
{
  if (!widgets) {
    return;
  }

  widgets->query_state.displayed_query = DisplayedPackageQueryState();
  widgets->query_state.displayed_query.kind = kind;
}

// -----------------------------------------------------------------------------
// Return true when the currently displayed table came from List Upgradable.
// -----------------------------------------------------------------------------
bool
package_query_displayed_view_is_upgradeable(const MainWindowUiState *widgets)
{
  return widgets && widgets->query_state.displayed_query.kind == DisplayedPackageQueryKind::LIST_UPGRADEABLE;
}

// -----------------------------------------------------------------------------
// Preserve the active search term and flags so a post-transaction refresh can
// rebuild the visible search results even if the user changes the checkboxes
// while the background work is still running.
// -----------------------------------------------------------------------------
void
package_query_set_displayed_search_query(MainWindowUiState *widgets,
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
// Complete one rebuild-triggered refresh.
// When the old selection survived the refreshed query result, leave the details pane intact.
// Otherwise clear it so outdated package info is not shown for rows that disappeared.
// -----------------------------------------------------------------------------
void
package_query_finish_results_refresh(MainWindowUiState *widgets)
{
  if (!widgets) {
    return;
  }

  if (widgets->query_state.preserve_selection_on_reload) {
    PackageRow selected;
    if (!package_table_get_selected_package_row(widgets, selected)) {
      package_details_reset_details_view(widgets);
    }
  } else {
    package_details_reset_details_view(widgets);
  }

  widgets->query_state.preserve_selection_on_reload = false;
  widgets->query_state.reload_selected_nevra.clear();
}

// -----------------------------------------------------------------------------
// Hide the package query timing label while new work is running.
// -----------------------------------------------------------------------------
void
package_query_clear_duration_label(MainWindowUiState *widgets)
{
  if (!widgets || !widgets->window_state.query_duration_label) {
    return;
  }

  gtk_label_set_text(widgets->window_state.query_duration_label, "");
  gtk_widget_set_visible(GTK_WIDGET(widgets->window_state.query_duration_label), FALSE);
}

// -----------------------------------------------------------------------------
// Show how long a package query took in the bottom bar.
// -----------------------------------------------------------------------------
void
package_query_show_duration_label(MainWindowUiState *widgets, const char *title, gint64 started_at_us)
{
  if (!widgets || !widgets->window_state.query_duration_label || started_at_us <= 0) {
    return;
  }

  gint64 elapsed_us = g_get_monotonic_time() - started_at_us;
  if (elapsed_us < 0) {
    elapsed_us = 0;
  }

  const double elapsed_seconds = static_cast<double>(elapsed_us) / 1000000.0;
  const char *display_title = title ? title : _("Query");
  std::string text = dnfui_i18n_format(_("%s: %.1f s"), display_title, elapsed_seconds);
  gtk_label_set_text(widgets->window_state.query_duration_label, text.c_str());
  gtk_widget_set_visible(GTK_WIDGET(widgets->window_state.query_duration_label), TRUE);
}

// -----------------------------------------------------------------------------
// Return true when a package list task is currently running.
// -----------------------------------------------------------------------------
bool
package_query_has_active_package_list_request(const MainWindowUiState *widgets)
{
  return widgets && widgets->query_state.package_list_cancellable;
}

// -----------------------------------------------------------------------------
// Enable or disable the idle package query controls.
// -----------------------------------------------------------------------------
void
package_query_set_idle_controls_sensitive(MainWindowUiState *widgets, bool sensitive)
{
  if (!widgets) {
    return;
  }

  gtk_widget_set_sensitive(GTK_WIDGET(widgets->query.entry), sensitive);
  gtk_widget_set_sensitive(GTK_WIDGET(widgets->query.desc_checkbox), sensitive);
  gtk_widget_set_sensitive(GTK_WIDGET(widgets->query.exact_checkbox), sensitive);
  gtk_widget_set_sensitive(GTK_WIDGET(widgets->query.history_list), sensitive);
  gtk_widget_set_sensitive(GTK_WIDGET(widgets->query.list_button), sensitive);
  gtk_widget_set_sensitive(GTK_WIDGET(widgets->query.list_available_button), sensitive);
  gtk_widget_set_sensitive(GTK_WIDGET(widgets->query.list_upgradeable_button), sensitive);
  gtk_widget_set_sensitive(GTK_WIDGET(widgets->query.search_button), sensitive);
}

// -----------------------------------------------------------------------------
// Return the button that currently works as Stop.
// -----------------------------------------------------------------------------
static GtkButton *
package_list_stop_button(MainWindowUiState *widgets, PackageListRequestKind kind)
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
  case PackageListRequestKind::EXACT_RELOAD:
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
  case PackageListRequestKind::EXACT_RELOAD:
    return _("Selected package refresh cancelled.");
  case PackageListRequestKind::NONE:
  default:
    return _("Operation cancelled.");
  }
}

// -----------------------------------------------------------------------------
// Human-readable status shown after the user asks a background request to stop.
// The worker may still need a moment to leave libdnf query code.
// -----------------------------------------------------------------------------
static const char *
package_list_stopping_status(PackageListRequestKind kind)
{
  switch (kind) {
  case PackageListRequestKind::SEARCH:
    return _("Stopping search...");
  case PackageListRequestKind::LIST_INSTALLED:
    return _("Stopping installed package listing...");
  case PackageListRequestKind::LIST_AVAILABLE:
    return _("Stopping package listing...");
  case PackageListRequestKind::LIST_UPGRADEABLE:
    return _("Stopping upgradable package listing...");
  case PackageListRequestKind::EXACT_RELOAD:
    return _("Stopping selected package refresh...");
  case PackageListRequestKind::NONE:
  default:
    return _("Stopping operation...");
  }
}

// -----------------------------------------------------------------------------
// Record the active package list task and switch its button to Stop.
// -----------------------------------------------------------------------------
void
package_query_begin_package_list_request(MainWindowUiState *widgets,
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

  package_query_clear_duration_label(widgets);

  ui_helpers_set_icon_button(widgets->query.search_button, "system-search-symbolic", _("Search"));
  ui_helpers_set_icon_button(widgets->query.list_button, "view-list-symbolic", _("List Installed"));
  ui_helpers_set_icon_button(widgets->query.list_available_button, "view-list-symbolic", _("List Packages"));
  ui_helpers_set_icon_button(widgets->query.list_upgradeable_button, "view-list-symbolic", _("List Upgradable"));
  ui_helpers_set_icon_button(stop_button, "process-stop-symbolic", _("Stop"));
  package_query_set_idle_controls_sensitive(widgets, false);
  if (widgets->transaction.mark_listed_upgrades_button) {
    gtk_widget_set_sensitive(GTK_WIDGET(widgets->transaction.mark_listed_upgrades_button), FALSE);
  }
  gtk_widget_set_sensitive(GTK_WIDGET(stop_button), TRUE);
}

// -----------------------------------------------------------------------------
// Restore the normal search and list controls after a package query stops or finishes.
// -----------------------------------------------------------------------------
static void
restore_package_list_controls(MainWindowUiState *widgets)
{
  if (!widgets) {
    return;
  }

  ui_helpers_set_icon_button(widgets->query.search_button, "system-search-symbolic", _("Search"));
  ui_helpers_set_icon_button(widgets->query.list_button, "view-list-symbolic", _("List Installed"));
  ui_helpers_set_icon_button(widgets->query.list_available_button, "view-list-symbolic", _("List Packages"));
  ui_helpers_set_icon_button(widgets->query.list_upgradeable_button, "view-list-symbolic", _("List Upgradable"));
  package_query_set_idle_controls_sensitive(widgets, true);
  if (widgets->transaction.mark_listed_upgrades_button) {
    bool transaction_busy = widgets->transaction.preview_request_in_progress || widgets->transaction.apply_in_progress;
    gtk_widget_set_sensitive(GTK_WIDGET(widgets->transaction.mark_listed_upgrades_button), !transaction_busy);
  }
}

// -----------------------------------------------------------------------------
// Restore the package list controls when the active task is done.
// -----------------------------------------------------------------------------
void
package_query_end_package_list_request(MainWindowUiState *widgets, uint64_t request_id, PackageListRequestKind kind)
{
  if (!widgets || widgets->query_state.current_package_list_request_id != request_id ||
      widgets->query_state.current_package_list_request_kind != kind) {
    return;
  }

  bool request_cancelled = widgets->query_state.package_list_cancellable &&
      g_cancellable_is_cancelled(widgets->query_state.package_list_cancellable);

  if (widgets->query_state.package_list_cancellable) {
    g_object_unref(widgets->query_state.package_list_cancellable);
    widgets->query_state.package_list_cancellable = nullptr;
  }
  widgets->query_state.current_package_list_request_id = 0;
  widgets->query_state.current_package_list_request_kind = PackageListRequestKind::NONE;
  restore_package_list_controls(widgets);

  if (request_cancelled) {
    ui_helpers_set_status(widgets->query.status_label, package_list_cancelled_status(kind), "gray");
  }
}

// -----------------------------------------------------------------------------
// Ask the active package list request to stop.
// Controls stay busy until the worker reaches a cancellation point.
// -----------------------------------------------------------------------------
void
package_query_cancel_active_package_list_request(MainWindowUiState *widgets)
{
  if (!widgets || !widgets->query_state.package_list_cancellable) {
    return;
  }

  PackageListRequestKind kind = widgets->query_state.current_package_list_request_kind;
  GCancellable *c = widgets->query_state.package_list_cancellable;
  if (g_cancellable_is_cancelled(c)) {
    ui_helpers_set_status(widgets->query.status_label, package_list_stopping_status(kind), "gray");
    return;
  }

  g_cancellable_cancel(c);

  ui_helpers_set_status(widgets->query.status_label, package_list_stopping_status(kind), "gray");
}

// -----------------------------------------------------------------------------
// EOF
// -----------------------------------------------------------------------------
