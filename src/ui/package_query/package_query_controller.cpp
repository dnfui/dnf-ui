// -----------------------------------------------------------------------------
// package_query_controller.cpp
// Public package query controller callbacks
//
// Handles the GTK entry points for search, package lists, clear, history, and view reload.
// Worker task details live in package_query_tasks.cpp.
// -----------------------------------------------------------------------------
#include "ui/package_query/package_query_controller.hpp"

#include "dnf_backend/base_manager.hpp"
#include "dnf_backend/dnf_backend.hpp"
#include "i18n.hpp"
#include "ui/details/package_details_controller.hpp"
#include "ui/package_query/package_query_cache.hpp"
#include "ui/package_query/package_query_controller_internal.hpp"
#include "ui/package_table/package_table_view.hpp"
#include "ui/common/ui_helpers.hpp"
#include "ui/common/widgets.hpp"

#include <string>
#include <vector>

// -----------------------------------------------------------------------------
// Clear cached search results.
// Used by the Clear Cache button, repository refresh, and transaction refresh.
// -----------------------------------------------------------------------------
void
package_query_clear_search_cache()
{
  package_query_cache_clear();
}

// -----------------------------------------------------------------------------
// Add a new search term to history if it is not already present.
// -----------------------------------------------------------------------------
static void
add_to_history(MainWindowUiState *widgets, const std::string &term)
{
  if (term.empty()) {
    return;
  }

  // Prevent duplicate history entries.
  for (const auto &s : widgets->query_state.history) {
    if (s == term) {
      return;
    }
  }

  // Append the new term to internal state and the visible history list.
  widgets->query_state.history.push_back(term);
  GtkWidget *row = gtk_label_new(term.c_str());
  gtk_label_set_xalign(GTK_LABEL(row), 0.0);
  gtk_list_box_append(widgets->query.history_list, row);
}

// -----------------------------------------------------------------------------
// Run a search from cache or start a background search task.
// -----------------------------------------------------------------------------
static void
perform_search(MainWindowUiState *widgets, const std::string &term)
{
  if (term.empty()) {
    return;
  }

  // Include the current checkboxes in the cache key even for history searches.
  dnf_backend_set_search_options({
      .search_in_description =
          static_cast<bool>(gtk_check_button_get_active(GTK_CHECK_BUTTON(widgets->query.desc_checkbox))),
      .exact_match = static_cast<bool>(gtk_check_button_get_active(GTK_CHECK_BUTTON(widgets->query.exact_checkbox))),
  });
  const DnfBackendSearchOptions search_options = dnf_backend_get_search_options();

  gtk_editable_set_text(GTK_EDITABLE(widgets->query.entry), term.c_str());
  std::string searching_message = dnfui_i18n_format(_("Searching for '%s'..."), term.c_str());
  ui_helpers_set_status(widgets->query.status_label, searching_message, "blue");
  package_query_clear_duration_label(widgets);
  if (!widgets->query_state.preserve_selection_on_reload) {
    widgets->results.selected_nevra.clear();
  }

  // Look up saved rows before starting a new backend query.
  // Base drops do not invalidate search results because they only release memory.
  // Generation and cache epoch still reject rows after refreshes, transactions, or explicit cache clears.
  const std::string key = package_query_cache_key_for(term);
  const uint64_t generation = BaseManager::instance().current_generation();
  const uint64_t cache_epoch = package_query_cache_current_epoch();
  const gint64 started_at_us = g_get_monotonic_time();
  std::vector<PackageRow> cached_packages;
  if (package_query_cache_lookup(key, generation, cache_epoch, cached_packages)) {
    // Show saved rows and skip the worker thread.
    package_query_set_displayed_search_query(
        widgets, term, search_options.search_in_description, search_options.exact_match);

    package_table_fill_package_view(widgets,
                                    cached_packages,
                                    cached_packages.empty() ? PackageTableEmptyState::NO_RESULTS
                                                            : PackageTableEmptyState::READY);

    std::string msg =
        dnfui_i18n_format_count(cached_packages.size(), "Loaded %zu cached result.", "Loaded %zu cached results.");
    ui_helpers_set_status(widgets->query.status_label, msg, "gray");
    package_query_show_duration_label(widgets, _("Search"), started_at_us);
    package_query_finish_results_refresh(widgets);

    return;
  }

  // No cache match, so start a worker thread search.
  package_query_start_search_task(widgets, term, key, generation, cache_epoch, search_options);
}

// -----------------------------------------------------------------------------
// Handle the List Installed button.
// The same button changes to Stop while its worker task is running.
// -----------------------------------------------------------------------------
void
package_query_on_list_button_clicked(GtkButton *, gpointer user_data)
{
  MainWindowUiState *widgets = static_cast<MainWindowUiState *>(user_data);
  if (package_query_has_active_package_list_request(widgets)) {
    if (widgets->query_state.current_package_list_request_kind == PackageListRequestKind::LIST_INSTALLED) {
      package_query_cancel_active_package_list_request(widgets);
    }
    return;
  }

  ui_helpers_set_status(widgets->query.status_label, _("Listing installed packages..."), "blue");
  package_query_start_list_installed_task(widgets);
}

// -----------------------------------------------------------------------------
// Handle the List Packages button.
// Starts background listing of the merged package view.
// The same button changes to Stop while the worker task is running.
// -----------------------------------------------------------------------------
void
package_query_on_list_available_button_clicked(GtkButton *, gpointer user_data)
{
  MainWindowUiState *widgets = static_cast<MainWindowUiState *>(user_data);
  if (package_query_has_active_package_list_request(widgets)) {
    if (widgets->query_state.current_package_list_request_kind == PackageListRequestKind::LIST_AVAILABLE) {
      package_query_cancel_active_package_list_request(widgets);
    }
    return;
  }

  ui_helpers_set_status(widgets->query.status_label, _("Listing packages..."), "blue");
  package_query_start_list_available_task(widgets);
}

// -----------------------------------------------------------------------------
// Handle the List Upgradable button.
// Starts background listing of installed packages that have available updates.
// The same button changes to Stop while the worker task is running.
// -----------------------------------------------------------------------------
void
package_query_on_list_upgradeable_button_clicked(GtkButton *, gpointer user_data)
{
  MainWindowUiState *widgets = static_cast<MainWindowUiState *>(user_data);
  if (package_query_has_active_package_list_request(widgets)) {
    if (widgets->query_state.current_package_list_request_kind == PackageListRequestKind::LIST_UPGRADEABLE) {
      package_query_cancel_active_package_list_request(widgets);
    }
    return;
  }

  ui_helpers_set_status(widgets->query.status_label, _("Listing upgradable packages..."), "blue");
  package_query_start_list_upgradeable_task(widgets);
}

// -----------------------------------------------------------------------------
// Handle the Search button or Enter in the search field.
// Reads options, checks the cache, and starts background search when needed.
// The same button acts as Stop while a search worker task is running.
// -----------------------------------------------------------------------------
void
package_query_on_search_button_clicked(GtkButton *, gpointer user_data)
{
  MainWindowUiState *widgets = static_cast<MainWindowUiState *>(user_data);
  if (package_query_has_active_package_list_request(widgets)) {
    if (widgets->query_state.current_package_list_request_kind == PackageListRequestKind::SEARCH ||
        widgets->query_state.current_package_list_request_kind == PackageListRequestKind::EXACT_RELOAD) {
      package_query_cancel_active_package_list_request(widgets);
    }
    return;
  }

  const char *txt = gtk_editable_get_text(GTK_EDITABLE(widgets->query.entry));
  std::string pattern = txt ? txt : "";

  if (pattern.empty()) {
    return;
  }

  // Save the search term and start lookup.
  add_to_history(widgets, pattern);
  perform_search(widgets, pattern);
}

// -----------------------------------------------------------------------------
// Handle selecting a search term from the history list.
// -----------------------------------------------------------------------------
void
package_query_on_history_row_selected(GtkListBox *, GtkListBoxRow *row, gpointer user_data)
{
  if (!row) {
    return;
  }

  MainWindowUiState *widgets = static_cast<MainWindowUiState *>(user_data);
  GtkWidget *child = gtk_list_box_row_get_child(row);
  const char *term = gtk_label_get_text(GTK_LABEL(child));
  perform_search(widgets, term);
}

// -----------------------------------------------------------------------------
// Handle the Clear List button.
// Clears displayed package rows and resets the details panel.
// -----------------------------------------------------------------------------
void
package_query_on_clear_button_clicked(GtkButton *, gpointer user_data)
{
  MainWindowUiState *widgets = static_cast<MainWindowUiState *>(user_data);

  package_query_cancel_active_package_list_request(widgets);

  widgets->query_state.displayed_query = DisplayedPackageQueryState();
  widgets->query_state.preserve_selection_on_reload = false;
  widgets->query_state.reload_selected_nevra.clear();
  widgets->results.selected_nevra.clear();
  package_table_fill_package_view(widgets, std::vector<PackageRow> {});
  package_query_clear_duration_label(widgets);

  // Reset status labels and package actions.
  ui_helpers_set_status(widgets->query.status_label, _("Ready."), "gray");
  package_details_reset_details_view(widgets);
  ui_helpers_update_action_button_labels(widgets, "");
}

// -----------------------------------------------------------------------------
// Rebuild the currently displayed package table after a transaction or repo
// refresh. Query-backed views are replayed through their normal async entry
// points, exact one-package views are refreshed from the selected NEVRA.
// -----------------------------------------------------------------------------
void
package_query_reload_current_view(MainWindowUiState *widgets)
{
  if (!widgets || package_query_has_active_package_list_request(widgets)) {
    return;
  }

  widgets->query_state.preserve_selection_on_reload = !widgets->results.selected_nevra.empty();
  widgets->query_state.reload_selected_nevra = widgets->results.selected_nevra;

  const DisplayedPackageQueryState view_state = widgets->query_state.displayed_query;

  switch (view_state.kind) {
  case DisplayedPackageQueryKind::SEARCH:
    if (view_state.search_term.empty()) {
      widgets->query_state.preserve_selection_on_reload = false;
      widgets->query_state.reload_selected_nevra.clear();
      BaseManager::instance().drop_cached_base();
      return;
    }

    gtk_check_button_set_active(GTK_CHECK_BUTTON(widgets->query.desc_checkbox), view_state.search_in_description);
    gtk_check_button_set_active(GTK_CHECK_BUTTON(widgets->query.exact_checkbox), view_state.exact_match);
    perform_search(widgets, view_state.search_term);
    return;
  case DisplayedPackageQueryKind::LIST_INSTALLED:
    package_query_on_list_button_clicked(nullptr, widgets);
    return;
  case DisplayedPackageQueryKind::LIST_AVAILABLE:
    package_query_on_list_available_button_clicked(nullptr, widgets);
    return;
  case DisplayedPackageQueryKind::LIST_UPGRADEABLE:
    package_query_on_list_upgradeable_button_clicked(nullptr, widgets);
    return;
  case DisplayedPackageQueryKind::NONE:
  default:
    break;
  }

  // Exact one-package views are not part of the main query state.
  // When the user is reviewing one package from the pending-actions sidebar,
  // refresh the selected NEVRA directly so removed rows disappear without extra global view state.
  if (widgets->results.selected_nevra.empty()) {
    widgets->query_state.preserve_selection_on_reload = false;
    widgets->query_state.reload_selected_nevra.clear();
    BaseManager::instance().drop_cached_base();
    return;
  }

  package_query_start_exact_package_reload_task(widgets, widgets->results.selected_nevra);
}

// -----------------------------------------------------------------------------
// Show one exact package from a pending action without doing backend work on the GTK thread.
// -----------------------------------------------------------------------------
void
package_query_show_exact_package(MainWindowUiState *widgets, const std::string &nevra)
{
  if (!widgets || nevra.empty()) {
    return;
  }

  if (package_query_has_active_package_list_request(widgets)) {
    ui_helpers_set_status(widgets->query.status_label, _("Wait for the current package query to finish."), "blue");
    return;
  }

  widgets->query_state.displayed_query = DisplayedPackageQueryState();
  widgets->query_state.preserve_selection_on_reload = true;
  widgets->query_state.reload_selected_nevra = nevra;
  widgets->results.selected_nevra = nevra;

  ui_helpers_set_status(widgets->query.status_label, _("Loading package..."), "blue");
  package_query_clear_duration_label(widgets);
  package_query_start_exact_package_reload_task(widgets, nevra);
}

// -----------------------------------------------------------------------------
// EOF
// -----------------------------------------------------------------------------
