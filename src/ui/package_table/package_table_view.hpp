// -----------------------------------------------------------------------------
// src/ui/package_table/package_table_view.hpp
// Public package table view entry points
//
// Owns the package table population, current row selection lookup, and visible
// status refresh used after pending transaction changes.
// -----------------------------------------------------------------------------
#pragma once

#include "dnf_backend/dnf_backend.hpp"
#include "ui/package_table/package_table_columns.hpp"
#include "upgrade/daemon_upgrade_target.hpp"

#include <gtk/gtk.h>

#include <memory>
#include <vector>

struct MainWindowUiState;

enum class PackageTableEmptyState {
  READY,
  NO_RESULTS,
};

// Only List Upgradable rows need daemon target data. Keep it behind a pointer so normal package rows stay small.
struct DaemonUpgradeRowContext {
  DaemonUpgradeTarget target;
  uint64_t upgrade_generation = 0;
};

using DaemonUpgradeRowContextPtr = std::shared_ptr<const DaemonUpgradeRowContext>;

struct PackageTableRow {
  PackageRow row;
  DaemonUpgradeRowContextPtr daemon_upgrade;

  const DaemonUpgradeTarget *upgrade_target() const
  {
    return daemon_upgrade ? &daemon_upgrade->target : nullptr;
  }

  uint64_t upgrade_generation() const
  {
    return daemon_upgrade ? daemon_upgrade->upgrade_generation : 0;
  }
};

// -----------------------------------------------------------------------------
// Return the currently selected package table row, including any daemon upgrade target.
// -----------------------------------------------------------------------------
bool package_table_get_selected_package(MainWindowUiState *widgets, PackageTableRow &out_pkg);
// -----------------------------------------------------------------------------
// Return all package table rows currently displayed in the package table.
// -----------------------------------------------------------------------------
std::vector<PackageTableRow> package_table_get_displayed_packages(MainWindowUiState *widgets);
// -----------------------------------------------------------------------------
// Replace the package table contents with the provided rows.
// -----------------------------------------------------------------------------
void package_table_fill_package_view(MainWindowUiState *widgets,
                                     const std::vector<PackageRow> &items,
                                     PackageTableEmptyState empty_state = PackageTableEmptyState::READY);
// -----------------------------------------------------------------------------
// Replace the package table contents with rows that may include daemon upgrade targets.
// -----------------------------------------------------------------------------
void package_table_fill_package_view(MainWindowUiState *widgets,
                                     const std::vector<PackageTableRow> &items,
                                     PackageTableEmptyState empty_state = PackageTableEmptyState::READY);
// -----------------------------------------------------------------------------
// Update the local package filter when its text or visibility changes.
// -----------------------------------------------------------------------------
void package_table_on_quick_filter_changed(GtkEditable *editable, gpointer user_data);
void package_table_on_quick_filter_mode_changed(GObject *bar, GParamSpec *, gpointer user_data);
// -----------------------------------------------------------------------------
// Return true while quick filtering is still processing results.
// -----------------------------------------------------------------------------
bool package_table_quick_filter_is_pending(MainWindowUiState *widgets);
// -----------------------------------------------------------------------------
// Refresh status values for all package rows.
// -----------------------------------------------------------------------------
void package_table_refresh_statuses(MainWindowUiState *widgets);
// -----------------------------------------------------------------------------
// Return true when one package table column is visible in the current GTK view.
// -----------------------------------------------------------------------------
bool package_table_column_is_visible(MainWindowUiState *widgets, PackageColumnKind kind);
// -----------------------------------------------------------------------------
// Change one package table column setting and update the current table if shown.
// -----------------------------------------------------------------------------
bool package_table_set_column_visible(MainWindowUiState *widgets, const char *column_id, bool visible);
// -----------------------------------------------------------------------------
// Reset package table columns to their default visibility and update the table.
// -----------------------------------------------------------------------------
void package_table_reset_columns_to_default(MainWindowUiState *widgets);

// -----------------------------------------------------------------------------
// EOF
// -----------------------------------------------------------------------------
