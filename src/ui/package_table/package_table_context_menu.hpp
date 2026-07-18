// -----------------------------------------------------------------------------
// Package table context menu helpers
//
// Owns the right-click transaction menu shown for one package table row.
// -----------------------------------------------------------------------------
#pragma once

#include "ui/package_table/package_table_view.hpp"

#include <functional>
#include <string>

#include <gtk/gtk.h>

struct MainWindowUiState;

// -----------------------------------------------------------------------------
// Show the package table context menu for one row.
// -----------------------------------------------------------------------------
void package_table_show_context_menu(GtkWidget *anchor,
                                     MainWindowUiState *widgets,
                                     const PackageTableRow &row,
                                     double x,
                                     double y,
                                     const std::function<bool(const std::string &)> &select_row);

// -----------------------------------------------------------------------------
// EOF
// -----------------------------------------------------------------------------
