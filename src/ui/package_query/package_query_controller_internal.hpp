// -----------------------------------------------------------------------------
// package_query_controller_internal.hpp
// Shared helpers for package query controller files
//
// The public controller keeps the GTK signal entry points. The task file owns
// worker thread callbacks. This header contains only the small shared functions
// needed between those files.
// -----------------------------------------------------------------------------
#pragma once

#include "dnf_backend/dnf_backend.hpp"
#include "ui/package_query/package_query_state.hpp"

#include <cstdint>
#include <string>

#include <gio/gio.h>

struct MainWindowUiState;

// -----------------------------------------------------------------------------
// Remember which query-backed view is currently displayed.
// -----------------------------------------------------------------------------
void package_query_set_displayed_query_kind(MainWindowUiState *widgets, DisplayedPackageQueryKind kind);
// -----------------------------------------------------------------------------
// Return true when the currently displayed table came from List Upgradable.
// -----------------------------------------------------------------------------
bool package_query_displayed_view_is_upgradeable(const MainWindowUiState *widgets);
// -----------------------------------------------------------------------------
// Remember the search query that produced the current table.
// -----------------------------------------------------------------------------
void package_query_set_displayed_search_query(MainWindowUiState *widgets,
                                              const std::string &term,
                                              bool search_in_description,
                                              bool exact_match,
                                              bool latest_only);
// -----------------------------------------------------------------------------
// Finish one refresh of the package table and update the details pane.
// -----------------------------------------------------------------------------
void package_query_finish_results_refresh(MainWindowUiState *widgets);
// -----------------------------------------------------------------------------
// Hide the package query timing label while new work is running.
// -----------------------------------------------------------------------------
void package_query_clear_duration_label(MainWindowUiState *widgets);
// -----------------------------------------------------------------------------
// Show how long a package query took in the bottom bar.
// -----------------------------------------------------------------------------
void package_query_show_duration_label(MainWindowUiState *widgets, const char *title, gint64 started_at_us);
// -----------------------------------------------------------------------------
// Return true when a package query worker is active.
// -----------------------------------------------------------------------------
bool package_query_has_active_package_list_request(const MainWindowUiState *widgets);
// -----------------------------------------------------------------------------
// Enable or disable the idle package query controls.
// -----------------------------------------------------------------------------
void package_query_set_idle_controls_sensitive(MainWindowUiState *widgets, bool sensitive);
// -----------------------------------------------------------------------------
// Put one query button into Stop mode while a worker owns it.
// -----------------------------------------------------------------------------
void package_query_begin_package_list_request(MainWindowUiState *widgets,
                                              GCancellable *c,
                                              uint64_t request_id,
                                              PackageListRequestKind kind);
// -----------------------------------------------------------------------------
// Restore normal query controls after the matching worker ends.
// -----------------------------------------------------------------------------
void
package_query_end_package_list_request(MainWindowUiState *widgets, uint64_t request_id, PackageListRequestKind kind);
// -----------------------------------------------------------------------------
// Ask the active package query to stop.
// The worker restores controls when it actually ends.
// -----------------------------------------------------------------------------
void package_query_cancel_active_package_list_request(MainWindowUiState *widgets);
// -----------------------------------------------------------------------------
// Start one installed-package list worker.
// -----------------------------------------------------------------------------
void package_query_start_list_installed_task(MainWindowUiState *widgets);
// -----------------------------------------------------------------------------
// Start one package browse worker.
// -----------------------------------------------------------------------------
void package_query_start_list_available_task(MainWindowUiState *widgets);
// -----------------------------------------------------------------------------
// Start one upgradable-package list worker.
// -----------------------------------------------------------------------------
void package_query_start_list_upgradeable_task(MainWindowUiState *widgets);
// -----------------------------------------------------------------------------
// Start one package search worker.
// -----------------------------------------------------------------------------
void package_query_start_search_task(MainWindowUiState *widgets,
                                     const std::string &term,
                                     const std::string &cache_key,
                                     uint64_t generation,
                                     uint64_t cache_epoch,
                                     const DnfBackendSearchOptions &search_options);
// -----------------------------------------------------------------------------
// Start one exact selected-package reload worker.
// -----------------------------------------------------------------------------
void package_query_start_exact_package_reload_task(MainWindowUiState *widgets, const std::string &nevra);

// -----------------------------------------------------------------------------
// EOF
// -----------------------------------------------------------------------------
