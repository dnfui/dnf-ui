// -----------------------------------------------------------------------------
// src/ui/transaction/pending_transaction_controller.hpp
// Public pending transaction controller entry points
//
// Declares the GTK callbacks used by the main window for pending transactions.
// The implementations are split by concern in the matching source files.
// -----------------------------------------------------------------------------
#pragma once

#include <gtk/gtk.h>

// -----------------------------------------------------------------------------
// Mark the selected package for install.
// -----------------------------------------------------------------------------
void pending_transaction_on_install_button_clicked(GtkButton *, gpointer user_data);
// -----------------------------------------------------------------------------
// Mark the selected package for removal.
// -----------------------------------------------------------------------------
void pending_transaction_on_remove_button_clicked(GtkButton *, gpointer user_data);
// -----------------------------------------------------------------------------
// Mark the selected package for reinstall.
// -----------------------------------------------------------------------------
void pending_transaction_on_reinstall_button_clicked(GtkButton *, gpointer user_data);
// -----------------------------------------------------------------------------
// Start previewing and applying all available package upgrades.
// -----------------------------------------------------------------------------
void pending_transaction_on_upgrade_all_button_clicked(GtkButton *, gpointer user_data);
// -----------------------------------------------------------------------------
// Start previewing and applying pending package actions.
// -----------------------------------------------------------------------------
void pending_transaction_on_apply_button_clicked(GtkButton *, gpointer user_data);
// -----------------------------------------------------------------------------
// Clear all pending package actions.
// -----------------------------------------------------------------------------
void pending_transaction_on_clear_pending_button_clicked(GtkButton *, gpointer user_data);

// -----------------------------------------------------------------------------
// EOF
// -----------------------------------------------------------------------------
