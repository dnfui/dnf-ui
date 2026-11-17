// -----------------------------------------------------------------------------
// src/ui/package_table/package_table_status.hpp
// Package table status rendering helpers
//
// Owns status text, sort priority, and CSS updates for the package table Status column.
// -----------------------------------------------------------------------------
#pragma once

#include "dnf_backend/dnf_backend.hpp"
#include "ui/package_table/package_table_view.hpp"
#include "ui/transaction/pending_transaction_action_rows.hpp"
#include "ui/transaction/pending_transaction_state.hpp"

#include <gtk/gtk.h>

struct MainWindowUiState;

// -----------------------------------------------------------------------------
// Return display text for one package install state.
// -----------------------------------------------------------------------------
const char *package_table_status_text(PackageInstallState state);
// -----------------------------------------------------------------------------
// Return display text for one package table row after action rows have been resolved.
// -----------------------------------------------------------------------------
const char *package_table_status_text_for_resolved_row(const PackageTableRow &row,
                                                       const PendingTransactionActionRows &action_rows,
                                                       bool exact_available_rows);
// -----------------------------------------------------------------------------
// Return the pending action for one package table row using already-resolved action rows.
// -----------------------------------------------------------------------------
bool package_table_pending_action_for_resolved_row(MainWindowUiState *widgets,
                                                   const PackageTableRow &row,
                                                   const PendingTransactionActionRows &action_rows,
                                                   PendingAction::Type &out_type,
                                                   bool *resolved_only = nullptr);
// -----------------------------------------------------------------------------
// Look up a display-only daemon action without searching explicit marks.
// -----------------------------------------------------------------------------
bool package_table_resolved_action_for_row(MainWindowUiState *widgets,
                                           const PackageTableRow &row,
                                           PendingAction::Type &out_type);
// -----------------------------------------------------------------------------
// Return display text for one pending package action.
// -----------------------------------------------------------------------------
const char *package_table_pending_action_status_text(PendingAction::Type action_type,
                                                     PackageInstallState install_state);
// -----------------------------------------------------------------------------
// Return the Status-cell CSS class for one pending action type.
// -----------------------------------------------------------------------------
const char *package_table_pending_action_css_class_for_type(PendingAction::Type action_type);
// -----------------------------------------------------------------------------
// Return text that distinguishes daemon-resolved actions from explicit marks.
// -----------------------------------------------------------------------------
const char *package_table_resolved_action_status_text(PendingAction::Type action_type);
// Remove all status CSS classes from a Status cell.
// -----------------------------------------------------------------------------
void package_table_clear_status_css(GtkWidget *cell);
// -----------------------------------------------------------------------------
// Remove pending action CSS classes from one table cell.
// -----------------------------------------------------------------------------
void package_table_clear_pending_action_css(GtkWidget *cell);
void package_table_update_resolved_status_label(GtkWidget *cell,
                                                MainWindowUiState *widgets,
                                                const PackageTableRow &row,
                                                const PendingTransactionActionRows &action_rows);

// -----------------------------------------------------------------------------
// EOF
// -----------------------------------------------------------------------------
