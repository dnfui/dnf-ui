// -----------------------------------------------------------------------------
// Package table context menu helpers
//
// Owns the right-click transaction menu shown for one package table row.
// -----------------------------------------------------------------------------
#pragma once

#include "dnf_backend/dnf_backend.hpp"

#include <functional>
#include <string>

#include <gtk/gtk.h>

struct SearchWidgets;

// -----------------------------------------------------------------------------
// Show the package table context menu for one row.
// -----------------------------------------------------------------------------
void package_table_show_context_menu(GtkWidget *anchor,
                                     SearchWidgets *widgets,
                                     const PackageRow &row,
                                     double x,
                                     double y,
                                     const std::function<bool(const std::string &)> &select_row);

// -----------------------------------------------------------------------------
// EOF
// -----------------------------------------------------------------------------
