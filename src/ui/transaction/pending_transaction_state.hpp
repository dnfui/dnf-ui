// -----------------------------------------------------------------------------
// src/ui/transaction/pending_transaction_state.hpp
// Pending transaction state model
//
// Keeps marked package actions and pending transaction widgets separate from the top-level widget state.
// -----------------------------------------------------------------------------
#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include <gtk/gtk.h>

// -----------------------------------------------------------------------------
// Pending actions for mark --> review --> apply workflow
// -----------------------------------------------------------------------------
struct PendingAction {
  enum Type { INSTALL, UPGRADE, DOWNGRADE, REMOVE, REINSTALL } type;
  // Package row shown in the pending list.
  std::string nevra;
  // Package spec sent to the transaction client. Empty means use nevra.
  std::string transaction_spec;
  // Package identity used to keep one pending action per name and architecture.
  std::string package_key;
};

// -----------------------------------------------------------------------------
// Return true for actions controlled by the install button.
// -----------------------------------------------------------------------------
inline bool
pending_action_type_is_install_side(PendingAction::Type type)
{
  return type == PendingAction::INSTALL || type == PendingAction::UPGRADE || type == PendingAction::DOWNGRADE;
}

// -----------------------------------------------------------------------------
// Return the pending action type handled by the install button for one package ID.
// -----------------------------------------------------------------------------
inline bool
pending_actions_get_install_side_action_type(const std::vector<PendingAction> &actions,
                                             const std::string &nevra,
                                             PendingAction::Type &out_type)
{
  for (const auto &action : actions) {
    if (action.nevra == nevra && pending_action_type_is_install_side(action.type)) {
      out_type = action.type;
      return true;
    }
  }

  return false;
}

// -----------------------------------------------------------------------------
// Remove all pending actions for one package name and architecture.
// -----------------------------------------------------------------------------
inline bool
pending_actions_remove_package_key(std::vector<PendingAction> &actions, const std::string &package_key)
{
  if (package_key.empty()) {
    return false;
  }

  bool removed = false;
  for (std::size_t i = 0; i < actions.size();) {
    if (actions[i].package_key == package_key) {
      actions.erase(actions.begin() + i);
      removed = true;
      continue;
    }
    ++i;
  }
  return removed;
}

// -----------------------------------------------------------------------------
// Pending transaction widgets and marked package actions
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
  bool preview_request_in_progress = false;
  bool apply_in_progress = false;
  bool preview_upgrade_all = false;
  std::vector<PendingAction> actions;
  std::string preview_transaction_path;
};

// -----------------------------------------------------------------------------
// EOF
// -----------------------------------------------------------------------------
