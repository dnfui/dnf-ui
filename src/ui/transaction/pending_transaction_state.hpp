// -----------------------------------------------------------------------------
// src/ui/transaction/pending_transaction_state.hpp
// Pending transaction state model
//
// Keeps marked package actions and pending transaction widgets separate from the top-level widget state.
// -----------------------------------------------------------------------------
#pragma once

#include <string>
#include <vector>

#include <gtk/gtk.h>

#include "transaction/pending_action.hpp"
#include "ui/transaction/failed_apply_refresh_state.hpp"

// -----------------------------------------------------------------------------
// Pending transaction runtime state
// -----------------------------------------------------------------------------
struct PendingTransactionState {
  bool preview_request_in_progress = false;
  bool apply_in_progress = false;
  bool preview_upgrade_all = false;
  bool preview_requires_offline = false;
  FailedApplyRefreshState failed_apply_refresh_state = FailedApplyRefreshState::IDLE;
  std::string failed_apply_exact_nevra;
  std::vector<PendingAction> actions;
  std::string preview_transaction_path;
};

// -----------------------------------------------------------------------------
// Pending transaction widgets
// -----------------------------------------------------------------------------
struct PendingTransactionWidgets {
  GtkButton *install_button = nullptr;
  GtkButton *remove_button = nullptr;
  GtkButton *reinstall_button = nullptr;
  GtkButton *upgrade_all_button = nullptr;
  GtkButton *mark_listed_upgrades_button = nullptr;
  GtkButton *apply_button = nullptr;
  GtkButton *clear_pending_button = nullptr;
  GtkListBox *pending_list = nullptr;
};

// -----------------------------------------------------------------------------
// EOF
// -----------------------------------------------------------------------------
