// -----------------------------------------------------------------------------
// src/ui/pending_transaction_state.hpp
// Pending transaction state model
//
// Keeps marked package actions and pending transaction widgets separate from the top-level widget state.
// -----------------------------------------------------------------------------
#pragma once

#include <string>
#include <vector>

#include <gtk/gtk.h>

// -----------------------------------------------------------------------------
// Pending actions for mark --> review --> apply workflow
// -----------------------------------------------------------------------------
struct PendingAction {
  enum Type { INSTALL, UPGRADE, REMOVE, REINSTALL } type;
  std::string nevra;
};

// -----------------------------------------------------------------------------
// Pending transaction widgets and marked package actions
// -----------------------------------------------------------------------------
struct PendingTransactionWidgets {
  GtkButton *install_button = nullptr;
  GtkButton *remove_button = nullptr;
  GtkButton *reinstall_button = nullptr;
  GtkButton *upgrade_all_button = nullptr;
  GtkButton *apply_button = nullptr;
  GtkButton *clear_pending_button = nullptr;
  GtkListBox *pending_list = nullptr;
  bool preview_request_in_progress = false;
  bool apply_in_progress = false;
  bool preview_upgrade_all = false;
  std::vector<PendingAction> actions;
  std::string preview_transaction_path;
};

// -----------------------------------------------------------------------------
// EOF
// -----------------------------------------------------------------------------
