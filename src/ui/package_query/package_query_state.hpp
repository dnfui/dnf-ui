// -----------------------------------------------------------------------------
// src/ui/package_query/package_query_state.hpp
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
enum class PackageListRequestKind {
  NONE,
  SEARCH,
  LIST_INSTALLED,
  LIST_UNNEEDED,
  LIST_PACKAGES,
  LIST_UPGRADEABLE,
  EXACT_RELOAD
};

struct ActivePackageListRequest {
  // Active cancellable for the current background package-list request, if any.
  GCancellable *cancellable = nullptr;
  // Request id owned by the active package-list button UI state.
  uint64_t id = 0;
  // Identifies which query button owns the active Stop state.
  PackageListRequestKind kind = PackageListRequestKind::NONE;
};

// -----------------------------------------------------------------------------
// Last query-backed package view shown in the main table.
// This intentionally tracks only views that can be reproduced through the main query controls.
// Exact one-package views from the pending-actions sidebar are
// refreshed via the currently selected NEVRA instead of adding more global UI state.
// -----------------------------------------------------------------------------
enum class DisplayedPackageQueryKind { NONE, SEARCH, LIST_INSTALLED, LIST_UNNEEDED, LIST_PACKAGES, LIST_UPGRADEABLE };

struct DisplayedPackageQueryState {
  DisplayedPackageQueryKind kind = DisplayedPackageQueryKind::NONE;
  std::string search_term;
  bool search_in_description = false;
  bool exact_match = false;
  bool latest_only = true;
  bool find_provider = false;
  bool exact_installonly_action = false;
};

// -----------------------------------------------------------------------------
// Return true when the displayed package table uses one compact row per package stream.
// -----------------------------------------------------------------------------
inline bool
displayed_package_query_uses_compact_rows(const DisplayedPackageQueryState &displayed)
{
  if (displayed.kind == DisplayedPackageQueryKind::LIST_UPGRADEABLE) {
    return true;
  }

  if (displayed.kind == DisplayedPackageQueryKind::SEARCH ||
      displayed.kind == DisplayedPackageQueryKind::LIST_PACKAGES) {
    return displayed.latest_only;
  }

  return false;
}

// -----------------------------------------------------------------------------
// Return true when the displayed package table may project upgrade actions onto installed rows.
// -----------------------------------------------------------------------------
inline bool
displayed_package_query_projects_upgrade_actions(const DisplayedPackageQueryState &displayed)
{
  if (displayed.kind == DisplayedPackageQueryKind::LIST_INSTALLED ||
      displayed.kind == DisplayedPackageQueryKind::LIST_UNNEEDED ||
      displayed.kind == DisplayedPackageQueryKind::LIST_UPGRADEABLE) {
    return true;
  }

  if (displayed.kind == DisplayedPackageQueryKind::SEARCH ||
      displayed.kind == DisplayedPackageQueryKind::LIST_PACKAGES) {
    return displayed.latest_only;
  }

  return false;
}

// -----------------------------------------------------------------------------
// Return true when an exact installed row may start the available upgrade action.
// This does not decide whether pending upgrade status is projected onto that row.
// -----------------------------------------------------------------------------
inline bool
displayed_package_query_allows_installed_upgrade_action(const DisplayedPackageQueryState &displayed)
{
  return displayed.kind == DisplayedPackageQueryKind::SEARCH ||
      displayed.kind == DisplayedPackageQueryKind::LIST_INSTALLED ||
      displayed.kind == DisplayedPackageQueryKind::LIST_UNNEEDED ||
      displayed.kind == DisplayedPackageQueryKind::LIST_PACKAGES ||
      displayed.kind == DisplayedPackageQueryKind::LIST_UPGRADEABLE;
}

// -----------------------------------------------------------------------------
// Return true when available repository rows are shown as exact package versions.
// -----------------------------------------------------------------------------
inline bool
displayed_package_query_uses_exact_available_rows(const DisplayedPackageQueryState &displayed)
{
  if (displayed.kind == DisplayedPackageQueryKind::SEARCH ||
      displayed.kind == DisplayedPackageQueryKind::LIST_PACKAGES) {
    return !displayed.latest_only;
  }

  return false;
}

// -----------------------------------------------------------------------------
// Return true when exact available installonly rows should queue exact install actions.
// -----------------------------------------------------------------------------
inline bool
displayed_package_query_uses_exact_installonly_actions(const DisplayedPackageQueryState &displayed)
{
  return displayed_package_query_uses_exact_available_rows(displayed) || displayed.exact_installonly_action;
}

// -----------------------------------------------------------------------------
// Runtime state for the active background package query flow
// -----------------------------------------------------------------------------
struct PackageQueryState {
  ActivePackageListRequest active_request;
  // Next package-list request id used to distinguish overlapping background tasks.
  uint64_t next_package_list_request_id = 1;
  // Remembers the last query-backed result view.
  // Rebuilds can repopulate the visible table instead of leaving outdated rows on screen after a transaction.
  DisplayedPackageQueryState displayed_query;
  // Temporary selection snapshot used only while a rebuild-triggered query is reloading.
  // Empty means no selection should be preserved.
  std::string reload_selected_nevra;
  // True when a quiet List Upgradable count check should run after current work is idle.
  bool upgrade_indicator_refresh_pending = false;
  std::vector<std::string> history;
};

// -----------------------------------------------------------------------------
// EOF
// -----------------------------------------------------------------------------
