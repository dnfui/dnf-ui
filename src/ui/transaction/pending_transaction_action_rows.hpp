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
#include "ui/transaction/pending_transaction_state.hpp"
#include "upgrade/daemon_upgrade_target.hpp"

#include <set>
#include <string>
#include <vector>

struct PendingTransactionActionRows {
  PackageInstallState state = PackageInstallState::AVAILABLE;
  bool install_is_upgrade = false;
  bool install_is_downgrade = false;
  bool has_install_row = false;
  bool has_installed_row = false;
  // True when the installed package that would be modified owns the running app.
  bool self_protected = false;
  // True when the exact installed package can be reinstalled from an enabled repository.
  bool exact_reinstall_available = false;
  // True when an already queued action should be removable from this row.
  bool install_action_from_pending = false;
  std::string package_key;
  std::string upgrade_spec;
  PackageRow install_row;
  PackageRow installed_row;
};

struct PendingTransactionSelectionActions {
  PendingTransactionActionRows rows;
  bool has_install_action = false;
  bool has_installed_action = false;
  bool install_blocked_by_self_protection = false;
  bool installed_action_blocked_by_self_protection = false;
  bool can_install = false;
  bool can_remove = false;
  bool can_reinstall = false;
  bool install_is_pending = false;
  bool remove_is_pending = false;
  bool reinstall_is_pending = false;
  std::string install_action_nevra;
  std::string installed_action_nevra;
};

// One eligible action resolved from a selected package table row.
struct PendingTransactionBatchAction {
  PendingTransactionActionRows rows;
  PendingAction::Type type;
};

struct DisplayedPackageQueryState;

// -----------------------------------------------------------------------------
// Resolve package IDs for a row that may carry a dnf5daemon upgrade target.
// -----------------------------------------------------------------------------
PendingTransactionActionRows pending_transaction_action_rows_for_selection(const PackageRow &selected,
                                                                           const DaemonUpgradeTarget *upgrade_target,
                                                                           uint64_t upgrade_generation,
                                                                           bool exact_installonly_actions);
PendingTransactionActionRows
pending_transaction_action_rows_for_resolved_selection(const PackageRow &selected,
                                                       const DaemonUpgradeTarget *upgrade_target,
                                                       uint64_t upgrade_generation,
                                                       const InstalledPackageResolution &installed_resolution,
                                                       bool exact_installonly_actions);
PendingTransactionActionRows
pending_transaction_action_rows_for_selection_with_pending(const PackageRow &selected,
                                                           const DaemonUpgradeTarget *upgrade_target,
                                                           uint64_t upgrade_generation,
                                                           bool exact_installonly_actions,
                                                           const std::vector<PendingAction> &actions);
PendingTransactionActionRows pending_transaction_action_rows_for_resolved_selection_with_pending(
    const PackageRow &selected,
    const DaemonUpgradeTarget *upgrade_target,
    uint64_t upgrade_generation,
    const InstalledPackageResolution &installed_resolution,
    bool exact_installonly_actions,
    const std::vector<PendingAction> &actions);
// -----------------------------------------------------------------------------
// Resolve which package actions the visible row can use in the current table view.
// -----------------------------------------------------------------------------
PendingTransactionSelectionActions
pending_transaction_actions_for_selection(const PackageRow &selected,
                                          const DaemonUpgradeTarget *upgrade_target,
                                          uint64_t upgrade_generation,
                                          const DisplayedPackageQueryState &displayed,
                                          const std::vector<PendingAction> &actions);
PendingTransactionSelectionActions
pending_transaction_actions_for_resolved_selection(const PackageRow &selected,
                                                   const DaemonUpgradeTarget *upgrade_target,
                                                   uint64_t upgrade_generation,
                                                   const InstalledPackageResolution &installed_resolution,
                                                   const DisplayedPackageQueryState &displayed,
                                                   const std::vector<PendingAction> &actions);

// -----------------------------------------------------------------------------
// Return true when the selected row may use its install, upgrade, or downgrade action.
// Some views may project an upgrade onto the installed package stream.
// Exact-version available rows only allow the row whose package ID is the action target.
// -----------------------------------------------------------------------------
bool pending_transaction_selection_allows_install_action(const PackageRow &selected,
                                                         const PendingTransactionActionRows &rows,
                                                         bool projects_upgrade_actions);
// -----------------------------------------------------------------------------
// Return true when the selected row may use its install, upgrade, or downgrade button action.
// Exact installed rows may start an upgrade without displaying the pending target status.
// -----------------------------------------------------------------------------
bool pending_transaction_selection_allows_install_button_action(const PackageRow &selected,
                                                                const PendingTransactionActionRows &rows,
                                                                bool allows_installed_upgrade_action);
// -----------------------------------------------------------------------------
// Return true when the selected row may modify its installed package.
// -----------------------------------------------------------------------------
bool pending_transaction_selection_allows_installed_action(const PackageRow &selected,
                                                           const PendingTransactionActionRows &rows,
                                                           bool compact_view);

// -----------------------------------------------------------------------------
// Add or replace one pending upgrade unless this package identity was already handled.
// Returns false when the row is not a valid upgrade candidate or the package identity was already marked.
// -----------------------------------------------------------------------------
bool pending_transaction_mark_unique_upgrade_action(std::vector<PendingAction> &actions,
                                                    std::set<std::string> &marked_package_keys,
                                                    const PackageRow &selected,
                                                    const PendingTransactionActionRows &rows,
                                                    bool projects_upgrade_actions);
// -----------------------------------------------------------------------------
// Add or replace one pending install, upgrade, or downgrade action from resolved rows.
// Returns false when the row has no install-button action.
// -----------------------------------------------------------------------------
bool pending_transaction_mark_install_side_action(std::vector<PendingAction> &actions,
                                                  const PendingTransactionActionRows &rows);
// -----------------------------------------------------------------------------
// Add or replace one pending remove action from resolved rows.
// Returns false when the row has no installed package action.
// -----------------------------------------------------------------------------
bool pending_transaction_mark_remove_action(std::vector<PendingAction> &actions,
                                            const PendingTransactionActionRows &rows);
// -----------------------------------------------------------------------------
// Add or replace one pending reinstall action from resolved rows.
// Returns false when the row has no installed package action.
// -----------------------------------------------------------------------------
bool pending_transaction_mark_reinstall_action(std::vector<PendingAction> &actions,
                                               const PendingTransactionActionRows &rows);
// -----------------------------------------------------------------------------
// Mark a batch without replacing a pending action with a different selected version.
// Leave the original actions unchanged if the batch contains conflicting or invalid requests.
// -----------------------------------------------------------------------------
bool pending_transaction_mark_batch(std::vector<PendingAction> &actions,
                                    const std::vector<PendingTransactionBatchAction> &selected,
                                    size_t &marked_count);
// -----------------------------------------------------------------------------
// Return true when self-protection should block the install button path.
// A normal upgrade is allowed because dnf5daemon still resolves the final preview.
// -----------------------------------------------------------------------------
bool pending_transaction_install_action_blocked_by_self_protection(const PendingTransactionActionRows &rows,
                                                                   bool self_protected);
// -----------------------------------------------------------------------------
// Return true when activating this visible row should use the remove action.
// -----------------------------------------------------------------------------
bool pending_transaction_activation_should_remove(const PackageRow &selected, const PendingTransactionActionRows &rows);

// -----------------------------------------------------------------------------
// EOF
// -----------------------------------------------------------------------------
