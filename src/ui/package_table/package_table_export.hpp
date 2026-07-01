// -----------------------------------------------------------------------------
// src/ui/package_table/package_table_export.hpp
// Package table export helpers
//
// Exports the rows currently shown in the package table without re-running a backend query.
// -----------------------------------------------------------------------------
#pragma once

#include <gtk/gtk.h>

struct MainWindowUiState;

// -----------------------------------------------------------------------------
// Export the current package table to a user-selected CSV file.
// -----------------------------------------------------------------------------
void package_table_export_visible_rows_to_csv(MainWindowUiState *widgets, GtkWindow *parent);

// -----------------------------------------------------------------------------
// EOF
// -----------------------------------------------------------------------------
