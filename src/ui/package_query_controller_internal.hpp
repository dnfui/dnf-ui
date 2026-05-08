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
#include "package_query_state.hpp"

#include <cstdint>
#include <string>

#include <gio/gio.h>

struct SearchWidgets;

// -----------------------------------------------------------------------------
// Remember which query-backed view is currently displayed.
// -----------------------------------------------------------------------------
void package_query_set_displayed_query_kind(SearchWidgets *widgets, DisplayedPackageQueryKind kind);
// -----------------------------------------------------------------------------
// Remember the search query that produced the current table.
// -----------------------------------------------------------------------------
void package_query_set_displayed_search_query(SearchWidgets *widgets,
                                              const std::string &term,
                                              bool search_in_description,
                                              bool exact_match);
// -----------------------------------------------------------------------------
// Finish one refresh of the package table and update the details pane.
// -----------------------------------------------------------------------------
void package_query_finish_results_refresh(SearchWidgets *widgets);
// -----------------------------------------------------------------------------
// Return true when a package query worker is active.
// -----------------------------------------------------------------------------
bool package_query_has_active_package_list_request(const SearchWidgets *widgets);
// -----------------------------------------------------------------------------
// Put one query button into Stop mode while a worker owns it.
// -----------------------------------------------------------------------------
void package_query_begin_package_list_request(SearchWidgets *widgets,
                                              GCancellable *c,
                                              uint64_t request_id,
                                              PackageListRequestKind kind);
// -----------------------------------------------------------------------------
// Restore normal query controls after the matching worker ends.
// -----------------------------------------------------------------------------
void package_query_end_package_list_request(SearchWidgets *widgets, uint64_t request_id, PackageListRequestKind kind);
// -----------------------------------------------------------------------------
// Cancel the active package query and restore the controls.
// -----------------------------------------------------------------------------
void package_query_cancel_active_package_list_request(SearchWidgets *widgets);
// -----------------------------------------------------------------------------
// Start one installed-package list worker.
// -----------------------------------------------------------------------------
void package_query_start_list_installed_task(SearchWidgets *widgets);
// -----------------------------------------------------------------------------
// Start one package browse worker.
// -----------------------------------------------------------------------------
void package_query_start_list_available_task(SearchWidgets *widgets);
// -----------------------------------------------------------------------------
// Start one upgradable-package list worker.
// -----------------------------------------------------------------------------
void package_query_start_list_upgradeable_task(SearchWidgets *widgets);
// -----------------------------------------------------------------------------
// Start one package search worker.
// -----------------------------------------------------------------------------
void package_query_start_search_task(SearchWidgets *widgets,
                                     const std::string &term,
                                     const std::string &cache_key,
                                     uint64_t generation,
                                     const DnfBackendSearchOptions &search_options);

// -----------------------------------------------------------------------------
// EOF
// -----------------------------------------------------------------------------
