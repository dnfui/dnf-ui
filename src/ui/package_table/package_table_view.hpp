// -----------------------------------------------------------------------------
// src/ui/package_table/package_table_view.hpp
// Public package table view entry points
//
// Owns the package table population, current row selection lookup, and visible
// status refresh used after pending transaction changes.
// -----------------------------------------------------------------------------
#pragma once

#include "dnf_backend/dnf_backend.hpp"
#include "dnf5daemon_client/transaction_service_client.hpp"

#include <optional>
#include <vector>

struct MainWindowUiState;

enum class PackageTableEmptyState {
  READY,
  NO_RESULTS,
};

struct PackageTableRow {
  PackageRow row;
  std::optional<TransactionServiceUpgradeTarget> upgrade_target;
  uint64_t upgrade_generation = 0;
};

// -----------------------------------------------------------------------------
// Return the currently selected package table row, including any daemon upgrade target.
// -----------------------------------------------------------------------------
bool package_table_get_selected_package(MainWindowUiState *widgets, PackageTableRow &out_pkg);
// -----------------------------------------------------------------------------
// Return the currently selected package row.
// -----------------------------------------------------------------------------
bool package_table_get_selected_package_row(MainWindowUiState *widgets, PackageRow &out_pkg);
// -----------------------------------------------------------------------------
// Return all package table rows currently displayed in the package table.
// -----------------------------------------------------------------------------
std::vector<PackageTableRow> package_table_get_displayed_packages(MainWindowUiState *widgets);
// -----------------------------------------------------------------------------
// Return all package rows currently displayed in the package table.
// -----------------------------------------------------------------------------
std::vector<PackageRow> package_table_get_displayed_package_rows(MainWindowUiState *widgets);
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
// Refresh status values for all visible package rows.
// -----------------------------------------------------------------------------
void package_table_refresh_statuses(MainWindowUiState *widgets);
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
