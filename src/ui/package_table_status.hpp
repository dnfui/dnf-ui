// -----------------------------------------------------------------------------
// src/ui/package_table_status.hpp
// Package table status rendering helpers
//
// Owns status text, sort priority, tooltip text, and CSS updates for the package table Status column.
// -----------------------------------------------------------------------------
#pragma once

#include "dnf_backend/dnf_backend.hpp"
#include "pending_transaction_state.hpp"

#include <string>

#include <gtk/gtk.h>

struct SearchWidgets;

enum class PackageTableStatusEffect {
  NONE,
  PENDING_INSTALL,
  PENDING_REINSTALL,
  PENDING_REMOVE,
  PREVIEW_INSTALL,
  PREVIEW_UPGRADE,
  PREVIEW_DOWNGRADE,
  PREVIEW_REINSTALL,
  PREVIEW_REMOVE,
};

// -----------------------------------------------------------------------------
// Return display text for one package install state.
// -----------------------------------------------------------------------------
const char *package_table_status_text(PackageInstallState state);
// -----------------------------------------------------------------------------
// Return the package table sort rank for one install state.
// -----------------------------------------------------------------------------
int package_table_status_rank(PackageInstallState state);
// -----------------------------------------------------------------------------
// Remove all status CSS classes from a Status cell.
// -----------------------------------------------------------------------------
void package_table_clear_status_css(GtkWidget *cell);
// -----------------------------------------------------------------------------
// Return the current transaction effect for one package row.
// -----------------------------------------------------------------------------
PackageTableStatusEffect package_table_status_effect_for_row(const PendingTransactionWidgets &transaction,
                                                             const std::string &nevra,
                                                             const std::string &alternate_nevra);
// -----------------------------------------------------------------------------
// Update one package Status cell for the current row state.
// -----------------------------------------------------------------------------
void package_table_update_status_label(GtkWidget *cell, SearchWidgets *widgets, const PackageRow &row);

// -----------------------------------------------------------------------------
// EOF
// -----------------------------------------------------------------------------
