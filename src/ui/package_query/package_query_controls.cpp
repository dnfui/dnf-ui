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
#include "upgrade/daemon_upgrade_state.hpp"

#include <vector>

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
static bool
package_query_displayed_view_is_upgradeable(const MainWindowUiState *widgets)
{
  return widgets && widgets->query_state.displayed_query.kind == DisplayedPackageQueryKind::LIST_UPGRADEABLE;
}

// -----------------------------------------------------------------------------
// Clear a List Upgradable table when daemon upgrade information is no longer current.
// The view kind is preserved so completion code can avoid reloading stale upgrade rows.
// -----------------------------------------------------------------------------
bool
package_query_clear_displayed_upgradeable_table(MainWindowUiState *widgets)
{
  if (!package_query_displayed_view_is_upgradeable(widgets)) {
    return false;
  }

  widgets->query_state.reload_selected_nevra.clear();
  widgets->results.selected_nevra.clear();
  package_table_fill_package_view(widgets, std::vector<PackageRow> {});
  package_details_reset_details_view(widgets);
  return true;
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
                                         bool exact_match,
                                         bool latest_only,
                                         bool find_provider)
{
  if (!widgets) {
    return;
  }

  widgets->query_state.displayed_query = DisplayedPackageQueryState();
  widgets->query_state.displayed_query.kind = DisplayedPackageQueryKind::SEARCH;
  widgets->query_state.displayed_query.search_term = term;
  widgets->query_state.displayed_query.search_in_description = search_in_description;
  widgets->query_state.displayed_query.exact_match = exact_match;
  widgets->query_state.displayed_query.latest_only = latest_only;
  widgets->query_state.displayed_query.find_provider = find_provider;
}

// -----------------------------------------------------------------------------
// Remember the List Packages options that produced the current table.
// -----------------------------------------------------------------------------
void
package_query_set_displayed_list_packages_query(MainWindowUiState *widgets, bool latest_only)
{
  if (!widgets) {
    return;
  }

  widgets->query_state.displayed_query = DisplayedPackageQueryState();
  widgets->query_state.displayed_query.kind = DisplayedPackageQueryKind::LIST_PACKAGES;
  widgets->query_state.displayed_query.latest_only = latest_only;
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

  if (!widgets->query_state.reload_selected_nevra.empty()) {
    PackageTableRow selected;
    if (!package_table_get_selected_package(widgets, selected)) {
      package_details_reset_details_view(widgets);
    }
  } else {
    package_details_reset_details_view(widgets);
  }

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

  ui_helpers_clear_duration_label(widgets->window_state.query_duration_label);
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

  ui_helpers_show_duration_label(widgets->window_state.query_duration_label, title, _("Query"), started_at_us);
}

// -----------------------------------------------------------------------------
// Return true when a package list task is currently running.
// -----------------------------------------------------------------------------
bool
package_query_has_active_package_list_request(const MainWindowUiState *widgets)
{
  return widgets && widgets->query_state.active_request.cancellable;
}

// -----------------------------------------------------------------------------
// Refresh the List Upgradable button label from the current daemon upgrade snapshot.
// -----------------------------------------------------------------------------
void
package_query_refresh_upgrade_indicator(MainWindowUiState *widgets)
{
  if (!widgets || !widgets->query.list_upgradeable_button) {
    return;
  }

  if (widgets->query_state.active_request.cancellable &&
      widgets->query_state.active_request.kind == PackageListRequestKind::LIST_UPGRADEABLE) {
    return;
  }

  std::string label = _("List Upgradable");
  DaemonUpgradeSnapshot snapshot = DaemonUpgradeState::instance().snapshot();
  if (snapshot.status == DaemonUpgradeSnapshotStatus::REFRESHING) {
    label = _("Checking Upgrades...");
  } else if (snapshot.status == DaemonUpgradeSnapshotStatus::READY && !snapshot.targets_by_name_arch.empty()) {
    label =
        dnfui_i18n_format_count(snapshot.targets_by_name_arch.size(), "List Upgradable (%zu)", "List Upgradable (%zu)");
  }

  ui_helpers_set_icon_button_with_fallback(widgets->query.list_upgradeable_button,
                                           "software-update-available-symbolic",
                                           "view-list-symbolic",
                                           label.c_str());
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
  const bool package_search = gtk_drop_down_get_selected(widgets->query.mode_dropdown) == 0;
  gtk_widget_set_sensitive(GTK_WIDGET(widgets->query.mode_dropdown), sensitive);
  gtk_widget_set_sensitive(GTK_WIDGET(widgets->query.options_button), sensitive && package_search);
  gtk_widget_set_sensitive(GTK_WIDGET(widgets->query.latest_checkbox), sensitive);
  gtk_widget_set_sensitive(GTK_WIDGET(widgets->query.history_list), sensitive);
  gtk_widget_set_sensitive(GTK_WIDGET(widgets->query.list_button), sensitive);
  gtk_widget_set_sensitive(GTK_WIDGET(widgets->query.list_packages_button), sensitive);
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
  case PackageListRequestKind::LIST_UNNEEDED:
    return widgets->query.list_button;
  case PackageListRequestKind::LIST_PACKAGES:
    return widgets->query.list_packages_button;
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
  case PackageListRequestKind::LIST_UNNEEDED:
    return _("Listing unneeded packages cancelled.");
  case PackageListRequestKind::LIST_PACKAGES:
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
  case PackageListRequestKind::LIST_UNNEEDED:
    return _("Stopping unneeded package listing...");
  case PackageListRequestKind::LIST_PACKAGES:
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

  if (widgets->query_state.active_request.cancellable) {
    g_object_unref(widgets->query_state.active_request.cancellable);
  }

  widgets->query_state.active_request.cancellable = G_CANCELLABLE(g_object_ref(c));
  widgets->query_state.active_request.id = request_id;
  widgets->query_state.active_request.kind = kind;
  GtkButton *stop_button = package_list_stop_button(widgets, kind);

  package_query_clear_duration_label(widgets);

  ui_helpers_set_icon_button(widgets->query.search_button, "edit-find-symbolic", _("Search"));
  ui_helpers_set_icon_button_with_fallback(
      widgets->query.list_button, "drive-harddisk-system-symbolic", "view-list-symbolic", _("List Installed"));
  ui_helpers_set_icon_button_with_fallback(
      widgets->query.list_packages_button, "package-x-generic-symbolic", "view-list-symbolic", _("List Packages"));
  package_query_refresh_upgrade_indicator(widgets);
  ui_helpers_set_icon_button_with_fallback(
      stop_button, "process-stop-symbolic", "media-playback-stop-symbolic", _("Stop"));
  package_query_set_idle_controls_sensitive(widgets, false);
  if (widgets->transaction_widgets.mark_listed_upgrades_button) {
    gtk_widget_set_sensitive(GTK_WIDGET(widgets->transaction_widgets.mark_listed_upgrades_button), FALSE);
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

  ui_helpers_set_icon_button(widgets->query.search_button, "edit-find-symbolic", _("Search"));
  ui_helpers_set_icon_button_with_fallback(
      widgets->query.list_button, "drive-harddisk-system-symbolic", "view-list-symbolic", _("List Installed"));
  ui_helpers_set_icon_button_with_fallback(
      widgets->query.list_packages_button, "package-x-generic-symbolic", "view-list-symbolic", _("List Packages"));
  package_query_refresh_upgrade_indicator(widgets);
  package_query_set_idle_controls_sensitive(widgets, true);
  if (widgets->transaction_widgets.mark_listed_upgrades_button) {
    bool transaction_busy =
        widgets->transaction_state.preview_request_in_progress || widgets->transaction_state.apply_in_progress;
    gtk_widget_set_sensitive(GTK_WIDGET(widgets->transaction_widgets.mark_listed_upgrades_button), !transaction_busy);
  }
}

// -----------------------------------------------------------------------------
// Restore the package list controls when the active task is done.
// -----------------------------------------------------------------------------
void
package_query_end_package_list_request(MainWindowUiState *widgets, uint64_t request_id, PackageListRequestKind kind)
{
  if (!widgets || widgets->query_state.active_request.id != request_id ||
      widgets->query_state.active_request.kind != kind) {
    return;
  }

  bool request_cancelled = widgets->query_state.active_request.cancellable &&
      g_cancellable_is_cancelled(widgets->query_state.active_request.cancellable);

  if (widgets->query_state.active_request.cancellable) {
    g_object_unref(widgets->query_state.active_request.cancellable);
  }
  widgets->query_state.active_request = {};
  restore_package_list_controls(widgets);

  if (request_cancelled) {
    ui_helpers_set_status(widgets->query.status_label, package_list_cancelled_status(kind), "gray");
  }

  package_query_maybe_start_upgrade_indicator_refresh(widgets);
}

// -----------------------------------------------------------------------------
// Ask the active package list request to stop.
// Controls stay busy until the worker reaches a cancellation point.
// -----------------------------------------------------------------------------
void
package_query_cancel_active_package_list_request(MainWindowUiState *widgets)
{
  if (!widgets || !widgets->query_state.active_request.cancellable) {
    return;
  }

  PackageListRequestKind kind = widgets->query_state.active_request.kind;
  GCancellable *c = widgets->query_state.active_request.cancellable;
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
