// -----------------------------------------------------------------------------
// src/ui/transaction/pending_transaction_action_rows.hpp
// Pending transaction action row resolver
//
// A visible package row can mean two things when an update exists:
//   - the installed package currently on disk
//   - the available package that would be installed by an upgrade
//
// The UI needs both package IDs.
// Upgrade shows the available package ID but sends a package spec for the installed package.
// Remove and reinstall use the installed package ID.
// -----------------------------------------------------------------------------
#pragma once

#include "dnf_backend/dnf_backend.hpp"
#include "dnf5daemon_client/transaction_service_client.hpp"
#include "ui/transaction/pending_transaction_state.hpp"

#include <vector>

struct PendingTransactionActionRows {
  PackageInstallState state = PackageInstallState::AVAILABLE;
  bool install_is_upgrade = false;
  bool has_install_row = false;
  bool has_installed_row = false;
  // Fast UI check only. This does not prove that reinstall is available from repositories.
  bool can_try_reinstall = false;
  std::string upgrade_spec;
  PackageRow install_row;
  PackageRow installed_row;
};

// Resolve package IDs for a row that may carry a dnf5daemon upgrade target.
// -----------------------------------------------------------------------------
PendingTransactionActionRows
pending_transaction_action_rows_for_selection(const PackageRow &selected,
                                              const TransactionServiceUpgradeTarget *upgrade_target,
                                              uint64_t upgrade_generation);
// -----------------------------------------------------------------------------
// Add or replace one pending upgrade action from a package row with an optional daemon target.
// Returns false when the row is not an upgrade candidate.
// -----------------------------------------------------------------------------
bool pending_transaction_mark_upgrade_action_for_row(std::vector<PendingAction> &actions,
                                                     const PackageRow &row,
                                                     const TransactionServiceUpgradeTarget *upgrade_target,
                                                     uint64_t upgrade_generation);
// -----------------------------------------------------------------------------
// Return true when self-protection should block the install button path.
// A normal upgrade is allowed because dnf5daemon still resolves the final preview.
// -----------------------------------------------------------------------------
bool pending_transaction_install_action_blocked_by_self_protection(const PendingTransactionActionRows &rows,
                                                                   bool self_protected);

// -----------------------------------------------------------------------------
// EOF
// -----------------------------------------------------------------------------
