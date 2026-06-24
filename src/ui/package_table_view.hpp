// -----------------------------------------------------------------------------
// src/ui/package_table_view.hpp
// Public package table view entry points
//
// Owns the package table population, current row selection lookup, and visible
// status refresh used after pending transaction changes.
// -----------------------------------------------------------------------------
#pragma once

#include "dnf_backend/dnf_backend.hpp"

#include <vector>

struct SearchWidgets;

// -----------------------------------------------------------------------------
// Return the currently selected package row.
// -----------------------------------------------------------------------------
bool package_table_get_selected_package_row(SearchWidgets *widgets, PackageRow &out_pkg);
// -----------------------------------------------------------------------------
// Replace the package table contents with the provided rows.
// -----------------------------------------------------------------------------
void package_table_fill_package_view(SearchWidgets *widgets, const std::vector<PackageRow> &items);
// -----------------------------------------------------------------------------
// Refresh status values for all visible package rows.
// -----------------------------------------------------------------------------
void package_table_refresh_statuses(SearchWidgets *widgets);
// -----------------------------------------------------------------------------
// Change one package table column setting and update the current table if shown.
// -----------------------------------------------------------------------------
bool package_table_set_column_visible(SearchWidgets *widgets, const char *column_id, bool visible);
// -----------------------------------------------------------------------------
// Reset package table columns to their default visibility and update the table.
// -----------------------------------------------------------------------------
void package_table_reset_columns_to_default(SearchWidgets *widgets);

// -----------------------------------------------------------------------------
// EOF
// -----------------------------------------------------------------------------
