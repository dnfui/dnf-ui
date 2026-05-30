// -----------------------------------------------------------------------------
// src/ui/package_query_state.hpp
// Package query state model
//
// Keeps the non-widget state for search, package listing, cancellation, and reload handling.
// Widget pointers stay in the top-level widget state.
// -----------------------------------------------------------------------------
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <gio/gio.h>

// -----------------------------------------------------------------------------
// Active background request using the package-list action buttons
// -----------------------------------------------------------------------------
enum class PackageListRequestKind { NONE, SEARCH, LIST_INSTALLED, LIST_AVAILABLE, LIST_UPGRADEABLE };

// -----------------------------------------------------------------------------
// Last query-backed package view shown in the main table.
// This intentionally tracks only views that can be reproduced through the main
// query controls. Exact one-package views from the pending-actions sidebar are
// refreshed via the currently selected NEVRA instead of adding more global UI
// state.
// -----------------------------------------------------------------------------
enum class DisplayedPackageQueryKind { NONE, SEARCH, LIST_INSTALLED, LIST_AVAILABLE, LIST_UPGRADEABLE };

struct DisplayedPackageQueryState {
  DisplayedPackageQueryKind kind = DisplayedPackageQueryKind::NONE;
  std::string search_term;
  bool search_in_description = false;
  bool exact_match = false;
};

// -----------------------------------------------------------------------------
// Runtime state for the active background package query flow
// -----------------------------------------------------------------------------
struct PackageQueryState {
  // Active cancellable for the current background package-list request, if any.
  GCancellable *package_list_cancellable = nullptr;
  // Next package-list request id used to distinguish overlapping background tasks.
  uint64_t next_package_list_request_id = 1;
  // Current package-list request id owned by the active package-list button UI state.
  uint64_t current_package_list_request_id = 0;
  // Identifies which query button owns the active Stop state.
  PackageListRequestKind current_package_list_request_kind = PackageListRequestKind::NONE;
  // Remembers the last query-backed result view.
  // Rebuilds can repopulate the visible table instead of leaving outdated rows on screen after a transaction.
  DisplayedPackageQueryState displayed_query;
  // Temporary selection snapshot used only while a rebuild-triggered query is
  // reloading. This lets the refreshed view keep the previously selected row
  // and details panel when the package is still present.
  bool preserve_selection_on_reload = false;
  std::string reload_selected_nevra;
  std::vector<std::string> history;
};

// -----------------------------------------------------------------------------
// EOF
// -----------------------------------------------------------------------------
