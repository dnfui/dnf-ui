// -----------------------------------------------------------------------------
// src/ui/package_table/package_table_status.hpp
// Package table status rendering helpers
//
// Owns status text, sort priority, and CSS updates for the package table Status column.
// -----------------------------------------------------------------------------
#pragma once

#include "dnf_backend/dnf_backend.hpp"
#include "ui/package_table/package_table_view.hpp"

#include <gtk/gtk.h>

struct MainWindowUiState;

// -----------------------------------------------------------------------------
// Return display text for one package install state.
// -----------------------------------------------------------------------------
const char *package_table_status_text(PackageInstallState state);
// Remove all status CSS classes from a Status cell.
// -----------------------------------------------------------------------------
void package_table_clear_status_css(GtkWidget *cell);
// -----------------------------------------------------------------------------
// Remove pending action CSS classes from one table cell.
// -----------------------------------------------------------------------------
void package_table_clear_pending_action_css(GtkWidget *cell);
// -----------------------------------------------------------------------------
// Return the pending action CSS class for one package table row.
// -----------------------------------------------------------------------------
const char *package_table_pending_action_css_class(MainWindowUiState *widgets, const PackageTableRow &row);
// -----------------------------------------------------------------------------
// Update one package Status cell for the current table row state.
// -----------------------------------------------------------------------------
void package_table_update_status_label(GtkWidget *cell, MainWindowUiState *widgets, const PackageTableRow &row);

// -----------------------------------------------------------------------------
// EOF
// -----------------------------------------------------------------------------
