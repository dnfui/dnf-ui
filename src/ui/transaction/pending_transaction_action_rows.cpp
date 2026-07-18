// -----------------------------------------------------------------------------
// src/ui/transaction/pending_transaction_action_rows.cpp
// Pending transaction action row resolver
//
// Keeps the package ID rules for upgrade, install, remove, and reinstall in one
// place. This file must not run libdnf queries because it is used while GTK is
// updating buttons and opening context menus.
// -----------------------------------------------------------------------------
#include "ui/transaction/pending_transaction_action_rows.hpp"

#include "upgrade/daemon_upgrade_state.hpp"

namespace {

// -----------------------------------------------------------------------------
// Return the package spec used when asking dnf5daemon to upgrade one installed package.
// -----------------------------------------------------------------------------
std::string
upgrade_transaction_spec(const PackageRow &row)
{
  if (row.arch.empty()) {
    return row.name;
  }

  return row.name + "." + row.arch;
}

// -----------------------------------------------------------------------------
// Remove one pending action by package ID.
// -----------------------------------------------------------------------------
void
remove_pending_action_by_nevra(std::vector<PendingAction> &actions, const std::string &nevra)
{
  for (size_t i = 0; i < actions.size();) {
    if (actions[i].nevra == nevra) {
      actions.erase(actions.begin() + i);
      continue;
    }
    ++i;
  }
}

// -----------------------------------------------------------------------------
// Remove stale pending upgrades that target the same daemon upgrade spec.
// -----------------------------------------------------------------------------
void
remove_pending_upgrade_by_transaction_spec(std::vector<PendingAction> &actions, const std::string &transaction_spec)
{
  if (transaction_spec.empty()) {
    return;
  }

  for (size_t i = 0; i < actions.size();) {
    if (actions[i].type == PendingAction::UPGRADE && actions[i].transaction_spec == transaction_spec) {
      actions.erase(actions.begin() + i);
      continue;
    }
    ++i;
  }
}

// -----------------------------------------------------------------------------
// Return true when a displayed daemon upgrade target still matches the shared snapshot.
// -----------------------------------------------------------------------------
bool
daemon_upgrade_target_is_current(const TransactionServiceUpgradeTarget &target, uint64_t upgrade_generation)
{
  DaemonUpgradeSnapshot snapshot = DaemonUpgradeState::instance().snapshot();
  if (snapshot.status != DaemonUpgradeSnapshotStatus::READY || snapshot.generation != upgrade_generation) {
    return false;
  }

  auto it = snapshot.targets_by_name_arch.find(target.name_arch_key());
  if (it == snapshot.targets_by_name_arch.end()) {
    return false;
  }

  return it->second.nevra == target.nevra && it->second.full_nevra == target.full_nevra &&
      it->second.upgrade_spec() == target.upgrade_spec();
}

} // namespace

// -----------------------------------------------------------------------------
// Resolve package IDs for action buttons without running libdnf queries.
// -----------------------------------------------------------------------------
PendingTransactionActionRows
pending_transaction_action_rows_for_selection(const PackageRow &selected)
{
  return pending_transaction_action_rows_for_selection(selected, nullptr, 0);
}

// -----------------------------------------------------------------------------
// Resolve package IDs for action buttons without running libdnf queries.
// -----------------------------------------------------------------------------
PendingTransactionActionRows
pending_transaction_action_rows_for_selection(const PackageRow &selected,
                                              const TransactionServiceUpgradeTarget *upgrade_target,
                                              uint64_t upgrade_generation)
{
  PendingTransactionActionRows rows;
  rows.state = upgrade_target ? PackageInstallState::UPGRADEABLE : dnf_backend_get_package_install_state(selected);
  rows.install_is_upgrade = rows.state == PackageInstallState::UPGRADEABLE;
  rows.install_row = selected;
  rows.installed_row = selected;

  const bool selected_is_installed = dnf_backend_is_package_installed_exact(selected);
  rows.has_installed_row = selected_is_installed;

  // Upgrade actions need the available package ID, not always the visible row ID.
  if (rows.install_is_upgrade) {
    if (upgrade_target) {
      rows.has_install_row = daemon_upgrade_target_is_current(*upgrade_target, upgrade_generation);
      rows.uses_daemon_upgrade_target = rows.has_install_row;
      rows.install_row.nevra = upgrade_target->nevra.empty() ? selected.nevra : upgrade_target->nevra;
      rows.upgrade_spec = upgrade_target->upgrade_spec();
      rows.has_installed_row = dnf_backend_get_installed_package_row_by_name_arch(selected, rows.installed_row);
      rows.can_try_reinstall = rows.has_installed_row;
      return rows;
    }

    if (selected_is_installed) {
      // Installed-list rows store the matching available upgrade package ID when the backend annotates them.
      rows.has_install_row = !selected.repo_candidate_nevra.empty();
      rows.install_row.nevra = selected.repo_candidate_nevra;
    } else {
      // Upgradable-list rows are already the available upgrade package.
      // The installed package ID comes from the installed snapshot.
      rows.has_install_row = true;
      rows.has_installed_row = dnf_backend_get_installed_package_row_by_name_arch(selected, rows.installed_row);
    }
    rows.upgrade_spec = upgrade_transaction_spec(rows.has_installed_row ? rows.installed_row : selected);
    rows.can_try_reinstall = rows.has_installed_row;
    return rows;
  }

  // Plain available packages can only be installed.
  if (rows.state == PackageInstallState::AVAILABLE) {
    rows.has_install_row = true;
  }

  // Reinstall needs an installed package that is still available from repositories.
  rows.can_try_reinstall = rows.has_installed_row && rows.state != PackageInstallState::LOCAL_ONLY &&
      rows.state != PackageInstallState::INSTALLED_NEWER_THAN_REPO;

  return rows;
}

// -----------------------------------------------------------------------------
// Add or replace one pending upgrade action from a package table row.
// -----------------------------------------------------------------------------
bool
pending_transaction_mark_upgrade_action_for_row(std::vector<PendingAction> &actions, const PackageRow &row)
{
  return pending_transaction_mark_upgrade_action_for_row(actions, row, nullptr, 0);
}

// -----------------------------------------------------------------------------
// Add or replace one pending upgrade action from a package table row.
// -----------------------------------------------------------------------------
bool
pending_transaction_mark_upgrade_action_for_row(std::vector<PendingAction> &actions,
                                                const PackageRow &row,
                                                const TransactionServiceUpgradeTarget *upgrade_target,
                                                uint64_t upgrade_generation)
{
  PendingTransactionActionRows action_rows =
      pending_transaction_action_rows_for_selection(row, upgrade_target, upgrade_generation);
  if (!action_rows.install_is_upgrade || !action_rows.has_install_row) {
    return false;
  }

  remove_pending_upgrade_by_transaction_spec(actions, action_rows.upgrade_spec);
  remove_pending_action_by_nevra(actions, row.nevra);
  if (action_rows.has_installed_row) {
    remove_pending_action_by_nevra(actions, action_rows.installed_row.nevra);
  }
  remove_pending_action_by_nevra(actions, action_rows.install_row.nevra);

  actions.push_back({ PendingAction::UPGRADE, action_rows.install_row.nevra, action_rows.upgrade_spec });
  return true;
}

// -----------------------------------------------------------------------------
// Return true when self-protection should block the install button path.
// A normal upgrade is allowed because dnf5daemon still resolves the final preview.
// -----------------------------------------------------------------------------
bool
pending_transaction_install_action_blocked_by_self_protection(const PendingTransactionActionRows &rows,
                                                              bool self_protected)
{
  return self_protected && !rows.install_is_upgrade;
}

// -----------------------------------------------------------------------------
// EOF
// -----------------------------------------------------------------------------
