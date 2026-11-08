// -----------------------------------------------------------------------------
// src/ui/transaction/pending_transaction_action_rows.cpp
// Pending transaction action row resolver
//
// Keeps the package ID rules for upgrade, install, remove, and reinstall in one
// place. This file must not run libdnf queries because it is used while GTK is
// updating buttons and opening context menus.
// -----------------------------------------------------------------------------
#include "ui/transaction/pending_transaction_action_rows.hpp"

#include "ui/package_query/package_query_state.hpp"
#include "upgrade/daemon_upgrade_state.hpp"

#include <map>
#include <utility>

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
// Return true when one pending action is queued by the install button path.
// -----------------------------------------------------------------------------
bool
pending_action_is_install_side(PendingAction::Type type)
{
  return type == PendingAction::INSTALL || type == PendingAction::UPGRADE || type == PendingAction::DOWNGRADE;
}

// -----------------------------------------------------------------------------
// Return true when one pending action changes an installed package directly.
// -----------------------------------------------------------------------------
bool
pending_action_is_installed_side(PendingAction::Type type)
{
  return type == PendingAction::REMOVE || type == PendingAction::REINSTALL;
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
// Remove stale pending install, upgrade, or downgrade actions for one package name and architecture.
// -----------------------------------------------------------------------------
void
remove_pending_install_side_action_by_package_key(std::vector<PendingAction> &actions,
                                                  const std::string &package_key,
                                                  bool keep_other_installonly_installs)
{
  if (package_key.empty()) {
    return;
  }

  for (size_t i = 0; i < actions.size();) {
    if (pending_action_is_install_side(actions[i].type) && actions[i].package_key == package_key) {
      if (keep_other_installonly_installs && actions[i].type == PendingAction::INSTALL && actions[i].installonly) {
        ++i;
        continue;
      }
      actions.erase(actions.begin() + i);
      continue;
    }
    ++i;
  }
}

// -----------------------------------------------------------------------------
// Remove pending remove or reinstall actions for one package name and architecture.
// -----------------------------------------------------------------------------
void
remove_pending_installed_side_action_by_package_key(std::vector<PendingAction> &actions, const std::string &package_key)
{
  if (package_key.empty()) {
    return;
  }

  for (size_t i = 0; i < actions.size();) {
    if (pending_action_is_installed_side(actions[i].type) && actions[i].package_key == package_key) {
      actions.erase(actions.begin() + i);
      continue;
    }
    ++i;
  }
}

// -----------------------------------------------------------------------------
// Remove older pending upgrades that were queued before package identity was available.
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
// Return true when the exact installed package represented by this row can be reinstalled.
// -----------------------------------------------------------------------------
bool
resolved_row_exact_reinstall_available(const PackageRow &selected,
                                       const InstalledPackageResolution &installed_resolution)
{
  if (!installed_resolution.has_installed_row) {
    return false;
  }

  if (installed_resolution.exact_installed) {
    return selected.repo_candidate_exact_available ||
        (selected.nevra == installed_resolution.installed_row.nevra &&
         installed_resolution.installed_row.repo_candidate_exact_available);
  }

  return installed_resolution.installed_row.repo_candidate_exact_available;
}

// -----------------------------------------------------------------------------
// Return true when one queued install side action belongs to the resolved row.
// -----------------------------------------------------------------------------
bool
pending_install_side_action_matches_rows(const PendingAction &action,
                                         const PackageRow &selected,
                                         const PendingTransactionActionRows &rows)
{
  if (!pending_action_is_install_side(action.type)) {
    return false;
  }

  if (action.nevra == selected.nevra) {
    return true;
  }

  if (rows.has_install_row && action.nevra == rows.install_row.nevra) {
    return true;
  }

  return false;
}

// -----------------------------------------------------------------------------
// Return the queued install side action that should control this row's action meaning.
// -----------------------------------------------------------------------------
const PendingAction *
find_pending_install_side_action(const std::vector<PendingAction> &actions,
                                 const PackageRow &selected,
                                 const PendingTransactionActionRows &rows)
{
  for (const auto &action : actions) {
    if (pending_install_side_action_matches_rows(action, selected, rows)) {
      return &action;
    }
  }

  return nullptr;
}

// -----------------------------------------------------------------------------
// Return true when one queued action matches the requested package ID and type.
// -----------------------------------------------------------------------------
bool
has_pending_action(const std::vector<PendingAction> &actions, const std::string &nevra, PendingAction::Type type)
{
  if (nevra.empty()) {
    return false;
  }

  for (const auto &action : actions) {
    if (action.nevra == nevra && action.type == type) {
      return true;
    }
  }

  return false;
}

// -----------------------------------------------------------------------------
// Return true when the resolved row already represents the queued action type.
// -----------------------------------------------------------------------------
bool
resolved_install_side_action_matches_pending(const PendingTransactionActionRows &rows, const PendingAction &action)
{
  if (!rows.has_install_row) {
    return false;
  }

  if (action.type == PendingAction::INSTALL) {
    return !rows.install_is_upgrade && !rows.install_is_downgrade;
  }

  if (action.type == PendingAction::UPGRADE) {
    return rows.install_is_upgrade;
  }

  if (action.type == PendingAction::DOWNGRADE) {
    return rows.install_is_downgrade;
  }

  return false;
}

// -----------------------------------------------------------------------------
// Preserve the meaning of a queued install side action when the user changes views.
// -----------------------------------------------------------------------------
void
apply_pending_install_side_action(PendingTransactionActionRows &rows,
                                  const PackageRow &selected,
                                  const PendingAction &action)
{
  rows.package_key = action.package_key.empty() ? rows.package_key : action.package_key;
  rows.has_install_row = true;
  rows.install_action_from_pending = true;
  rows.install_is_upgrade = action.type == PendingAction::UPGRADE;
  rows.install_is_downgrade = action.type == PendingAction::DOWNGRADE;
  rows.upgrade_spec = rows.install_is_upgrade ? action.transaction_spec : "";

  if (selected.nevra == action.nevra) {
    rows.install_row = selected;
  } else {
    rows.install_row.nevra = action.nevra;
    rows.install_row.name = selected.name;
    rows.install_row.arch = selected.arch;
    rows.install_row.installonly = action.installonly;
  }
}

} // namespace

// -----------------------------------------------------------------------------
// Resolve package IDs for action buttons without running libdnf queries.
// -----------------------------------------------------------------------------
PendingTransactionActionRows
pending_transaction_action_rows_for_resolved_selection(const PackageRow &selected,
                                                       const DaemonUpgradeTarget *upgrade_target,
                                                       uint64_t upgrade_generation,
                                                       const InstalledPackageResolution &installed_resolution,
                                                       bool exact_installonly_actions)
{
  PendingTransactionActionRows rows;
  rows.state = upgrade_target ? PackageInstallState::UPGRADEABLE : installed_resolution.state;
  rows.install_is_upgrade = rows.state == PackageInstallState::UPGRADEABLE;
  rows.install_is_downgrade = rows.state == PackageInstallState::DOWNGRADEABLE;
  rows.package_key = selected.name_arch_key();
  rows.install_row = selected;
  rows.installed_row = selected;
  rows.has_installed_row = installed_resolution.exact_installed;
  rows.self_protected = installed_resolution.exact_installed && installed_resolution.self_protected;
  if (!installed_resolution.exact_installed && installed_resolution.has_installed_row) {
    rows.has_installed_row = true;
    rows.installed_row = installed_resolution.installed_row;
    rows.self_protected = installed_resolution.self_protected;
  }

  if (exact_installonly_actions && selected.installonly && !installed_resolution.exact_installed && !upgrade_target) {
    rows.install_is_upgrade = false;
    rows.install_is_downgrade = false;
    rows.has_install_row = true;
    rows.install_row = selected;
    rows.exact_reinstall_available = false;
    return rows;
  }

  if (exact_installonly_actions && selected.installonly && installed_resolution.exact_installed && !upgrade_target) {
    rows.install_is_upgrade = false;
    rows.install_is_downgrade = false;
    rows.has_install_row = false;
    rows.exact_reinstall_available = resolved_row_exact_reinstall_available(selected, installed_resolution);
    return rows;
  }

  // Upgrade actions need the available package ID, not always the visible row ID.
  if (rows.install_is_upgrade) {
    if (upgrade_target) {
      rows.has_install_row = DaemonUpgradeState::instance().is_current_target(*upgrade_target, upgrade_generation);
      rows.install_row.nevra = upgrade_target->nevra.empty() ? selected.nevra : upgrade_target->nevra;
      rows.upgrade_spec = upgrade_target->upgrade_spec();
      rows.has_installed_row = installed_resolution.has_installed_row;
      rows.self_protected = rows.has_installed_row && installed_resolution.self_protected;
      rows.exact_reinstall_available = resolved_row_exact_reinstall_available(selected, installed_resolution);
      return rows;
    }

    if (installed_resolution.exact_installed) {
      // Installed-list rows store the matching available upgrade package ID when the backend annotates them.
      rows.has_install_row = !selected.repo_candidate_nevra.empty() && selected.repo_candidate_is_newest_available;
      rows.install_row.nevra = selected.repo_candidate_nevra;
    } else {
      // Available update rows are already repository package rows.
      // Only the newest available row can use the upgrade action.
      rows.has_install_row = selected.is_newest_available;
      rows.has_installed_row = installed_resolution.has_installed_row;
      rows.self_protected = rows.has_installed_row && installed_resolution.self_protected;
    }
    rows.upgrade_spec = upgrade_transaction_spec(rows.has_installed_row ? rows.installed_row : selected);
    rows.exact_reinstall_available = resolved_row_exact_reinstall_available(selected, installed_resolution);
    return rows;
  }

  // Older available rows can be marked as a downgrade to that exact package ID.
  if (rows.install_is_downgrade) {
    rows.has_install_row = true;
    rows.exact_reinstall_available = resolved_row_exact_reinstall_available(selected, installed_resolution);
    return rows;
  }

  // Plain available packages can only be installed.
  if (rows.state == PackageInstallState::AVAILABLE) {
    rows.has_install_row = true;
  }

  // Reinstall needs the exact installed package to still be available from repositories.
  rows.exact_reinstall_available = resolved_row_exact_reinstall_available(selected, installed_resolution);

  return rows;
}

PendingTransactionActionRows
pending_transaction_action_rows_for_selection(const PackageRow &selected,
                                              const DaemonUpgradeTarget *upgrade_target,
                                              uint64_t upgrade_generation,
                                              bool exact_installonly_actions)
{
  InstalledPackageResolution installed_resolution = dnf_backend_resolve_installed_package(selected);
  return pending_transaction_action_rows_for_resolved_selection(
      selected, upgrade_target, upgrade_generation, installed_resolution, exact_installonly_actions);
}

PendingTransactionActionRows
pending_transaction_action_rows_for_resolved_selection_with_pending(
    const PackageRow &selected,
    const DaemonUpgradeTarget *upgrade_target,
    uint64_t upgrade_generation,
    const InstalledPackageResolution &installed_resolution,
    bool exact_installonly_actions,
    const std::vector<PendingAction> &actions)
{
  PendingTransactionActionRows rows = pending_transaction_action_rows_for_resolved_selection(
      selected, upgrade_target, upgrade_generation, installed_resolution, exact_installonly_actions);

  const PendingAction *pending_action = find_pending_install_side_action(actions, selected, rows);
  if (pending_action && !resolved_install_side_action_matches_pending(rows, *pending_action)) {
    apply_pending_install_side_action(rows, selected, *pending_action);
  }

  return rows;
}

PendingTransactionActionRows
pending_transaction_action_rows_for_selection_with_pending(const PackageRow &selected,
                                                           const DaemonUpgradeTarget *upgrade_target,
                                                           uint64_t upgrade_generation,
                                                           bool exact_installonly_actions,
                                                           const std::vector<PendingAction> &actions)
{
  InstalledPackageResolution installed_resolution = dnf_backend_resolve_installed_package(selected);
  return pending_transaction_action_rows_for_resolved_selection_with_pending(
      selected, upgrade_target, upgrade_generation, installed_resolution, exact_installonly_actions, actions);
}

// -----------------------------------------------------------------------------
// Resolve which package actions the visible row can use in the current table view.
// -----------------------------------------------------------------------------
PendingTransactionSelectionActions
pending_transaction_actions_for_resolved_selection(const PackageRow &selected,
                                                   const DaemonUpgradeTarget *upgrade_target,
                                                   uint64_t upgrade_generation,
                                                   const InstalledPackageResolution &installed_resolution,
                                                   const DisplayedPackageQueryState &displayed,
                                                   const std::vector<PendingAction> &actions)
{
  PendingTransactionSelectionActions result;

  const bool exact_installonly_actions = displayed_package_query_uses_exact_installonly_actions(displayed);
  result.rows = pending_transaction_action_rows_for_resolved_selection_with_pending(
      selected, upgrade_target, upgrade_generation, installed_resolution, exact_installonly_actions, actions);

  const bool compact_view = displayed_package_query_uses_compact_rows(displayed);
  const bool allows_installed_upgrade_action = displayed_package_query_allows_installed_upgrade_action(displayed);

  result.has_install_action = pending_transaction_selection_allows_install_button_action(
      selected, result.rows, allows_installed_upgrade_action);
  result.has_installed_action =
      pending_transaction_selection_allows_installed_action(selected, result.rows, compact_view);
  result.install_blocked_by_self_protection =
      pending_transaction_install_action_blocked_by_self_protection(result.rows, result.rows.self_protected);
  result.installed_action_blocked_by_self_protection = result.rows.self_protected;
  result.can_install = result.has_install_action && !result.install_blocked_by_self_protection;
  result.can_remove = result.has_installed_action && !result.installed_action_blocked_by_self_protection;
  result.can_reinstall = result.can_remove && result.rows.exact_reinstall_available;
  result.install_action_nevra = result.has_install_action ? result.rows.install_row.nevra : "";
  result.installed_action_nevra = result.has_installed_action ? result.rows.installed_row.nevra : "";

  result.install_is_pending = has_pending_action(actions, result.install_action_nevra, PendingAction::INSTALL) ||
      has_pending_action(actions, result.install_action_nevra, PendingAction::UPGRADE) ||
      has_pending_action(actions, result.install_action_nevra, PendingAction::DOWNGRADE);
  result.remove_is_pending = has_pending_action(actions, result.installed_action_nevra, PendingAction::REMOVE);
  result.reinstall_is_pending = has_pending_action(actions, result.installed_action_nevra, PendingAction::REINSTALL);

  return result;
}

PendingTransactionSelectionActions
pending_transaction_actions_for_selection(const PackageRow &selected,
                                          const DaemonUpgradeTarget *upgrade_target,
                                          uint64_t upgrade_generation,
                                          const DisplayedPackageQueryState &displayed,
                                          const std::vector<PendingAction> &actions)
{
  InstalledPackageResolution installed_resolution = dnf_backend_resolve_installed_package(selected);
  return pending_transaction_actions_for_resolved_selection(
      selected, upgrade_target, upgrade_generation, installed_resolution, displayed, actions);
}

// -----------------------------------------------------------------------------
// Return true when the selected row may use its install, upgrade, or downgrade action.
// -----------------------------------------------------------------------------
bool
pending_transaction_selection_allows_install_action(const PackageRow &selected,
                                                    const PendingTransactionActionRows &rows,
                                                    bool projects_upgrade_actions)
{
  if (!rows.has_install_row) {
    return false;
  }

  if (rows.install_action_from_pending) {
    return true;
  }

  if (selected.nevra == rows.install_row.nevra) {
    return true;
  }

  return projects_upgrade_actions && rows.install_is_upgrade;
}

// -----------------------------------------------------------------------------
// Return true when the selected row may use its install, upgrade, or downgrade button action.
// -----------------------------------------------------------------------------
bool
pending_transaction_selection_allows_install_button_action(const PackageRow &selected,
                                                           const PendingTransactionActionRows &rows,
                                                           bool allows_installed_upgrade_action)
{
  if (!rows.has_install_row) {
    return false;
  }

  if (rows.install_action_from_pending) {
    return true;
  }

  if (selected.nevra == rows.install_row.nevra) {
    return true;
  }

  return allows_installed_upgrade_action && rows.install_is_upgrade && rows.has_installed_row &&
      selected.nevra == rows.installed_row.nevra;
}

// -----------------------------------------------------------------------------
// Return true when the selected row may modify its installed package.
// -----------------------------------------------------------------------------
bool
pending_transaction_selection_allows_installed_action(const PackageRow &selected,
                                                      const PendingTransactionActionRows &rows,
                                                      bool compact_view)
{
  if (!rows.has_installed_row) {
    return false;
  }

  if (selected.nevra == rows.installed_row.nevra) {
    return true;
  }

  return compact_view && rows.install_is_upgrade;
}

// -----------------------------------------------------------------------------
// Add or replace one pending install, upgrade, or downgrade action from resolved rows.
// -----------------------------------------------------------------------------
bool
pending_transaction_mark_install_side_action(std::vector<PendingAction> &actions,
                                             const PendingTransactionActionRows &action_rows)
{
  if (!action_rows.has_install_row) {
    return false;
  }

  PendingAction::Type action_type = PendingAction::INSTALL;
  std::string transaction_spec = action_rows.install_row.nevra;
  if (action_rows.install_is_upgrade) {
    action_type = PendingAction::UPGRADE;
    transaction_spec = action_rows.upgrade_spec;
  } else if (action_rows.install_is_downgrade) {
    action_type = PendingAction::DOWNGRADE;
  }

  if (transaction_spec.empty()) {
    return false;
  }

  const bool keep_other_installonly_installs =
      action_type == PendingAction::INSTALL && action_rows.install_row.installonly;

  remove_pending_install_side_action_by_package_key(actions, action_rows.package_key, keep_other_installonly_installs);
  remove_pending_installed_side_action_by_package_key(actions, action_rows.package_key);
  if (action_type == PendingAction::UPGRADE) {
    remove_pending_upgrade_by_transaction_spec(actions, transaction_spec);
  }
  remove_pending_action_by_nevra(actions, action_rows.install_row.nevra);
  if (action_rows.has_installed_row) {
    remove_pending_action_by_nevra(actions, action_rows.installed_row.nevra);
  }

  actions.push_back({ action_type,
                      action_rows.install_row.nevra,
                      transaction_spec,
                      action_rows.package_key,
                      action_type == PendingAction::INSTALL && action_rows.install_row.installonly });
  return true;
}

// -----------------------------------------------------------------------------
// Add or replace one pending remove or reinstall action from resolved rows.
// -----------------------------------------------------------------------------
static bool
pending_transaction_mark_installed_side_action(std::vector<PendingAction> &actions,
                                               const PendingTransactionActionRows &action_rows,
                                               PendingAction::Type action_type)
{
  if (!action_rows.has_installed_row) {
    return false;
  }

  remove_pending_install_side_action_by_package_key(actions, action_rows.package_key, false);
  remove_pending_action_by_nevra(actions, action_rows.installed_row.nevra);

  actions.push_back(
      { action_type, action_rows.installed_row.nevra, action_rows.installed_row.nevra, action_rows.package_key });
  return true;
}

// -----------------------------------------------------------------------------
// Add or replace one pending remove action from resolved rows.
// -----------------------------------------------------------------------------
bool
pending_transaction_mark_remove_action(std::vector<PendingAction> &actions,
                                       const PendingTransactionActionRows &action_rows)
{
  return pending_transaction_mark_installed_side_action(actions, action_rows, PendingAction::REMOVE);
}

// -----------------------------------------------------------------------------
// Add or replace one pending reinstall action from resolved rows.
// -----------------------------------------------------------------------------
bool
pending_transaction_mark_reinstall_action(std::vector<PendingAction> &actions,
                                          const PendingTransactionActionRows &action_rows)
{
  return pending_transaction_mark_installed_side_action(actions, action_rows, PendingAction::REINSTALL);
}

// -----------------------------------------------------------------------------
// Mark eligible selected packages only after checking for conflicting versions.
// A failed batch leaves the pending actions unchanged.
// -----------------------------------------------------------------------------
bool
pending_transaction_mark_batch(std::vector<PendingAction> &actions,
                               const std::vector<PendingTransactionBatchAction> &selected,
                               size_t &marked_count)
{
  marked_count = 0;
  std::map<std::string, size_t> install_by_key;
  for (size_t i = 0; i < selected.size(); ++i) {
    const auto &action = selected[i];
    const bool install_side = pending_action_is_install_side(action.type);
    const bool installed_side = pending_action_is_installed_side(action.type);
    if ((!install_side && !installed_side) || action.rows.package_key.empty() ||
        (install_side && (!action.rows.has_install_row || action.rows.install_row.nevra.empty())) ||
        (installed_side && (!action.rows.has_installed_row || action.rows.installed_row.nevra.empty()))) {
      return false;
    }

    if (!install_side) {
      continue;
    }

    auto [it, inserted] = install_by_key.emplace(action.rows.package_key, i);
    if (inserted) {
      continue;
    }

    const auto &previous = selected[it->second];
    if (previous.type == action.type && previous.rows.install_row.nevra == action.rows.install_row.nevra) {
      continue;
    }

    const bool multiple_installonly_versions = previous.type == PendingAction::INSTALL &&
        action.type == PendingAction::INSTALL && previous.rows.install_row.installonly &&
        action.rows.install_row.installonly;
    if (!multiple_installonly_versions) {
      return false;
    }
  }

  std::vector<PendingAction> updated = actions;
  std::set<std::pair<PendingAction::Type, std::string>> seen_actions;
  for (const auto &action : selected) {
    const bool install_side = pending_action_is_install_side(action.type);
    const std::string &nevra = install_side ? action.rows.install_row.nevra : action.rows.installed_row.nevra;
    if (!seen_actions.emplace(action.type, nevra).second ||
        has_pending_action(updated, nevra, action.type)) {
      continue;
    }

    bool success = false;
    if (install_side) {
      success = pending_transaction_mark_install_side_action(updated, action.rows);
    } else if (action.type == PendingAction::REMOVE) {
      success = pending_transaction_mark_remove_action(updated, action.rows);
    } else if (action.type == PendingAction::REINSTALL) {
      success = pending_transaction_mark_reinstall_action(updated, action.rows);
    }
    if (!success) {
      marked_count = 0;
      return false;
    }
    ++marked_count;
  }

  actions = std::move(updated);
  return true;
}

// -----------------------------------------------------------------------------
// Add or replace one pending upgrade unless this package identity was already handled.
// -----------------------------------------------------------------------------
bool
pending_transaction_mark_unique_upgrade_action(std::vector<PendingAction> &actions,
                                               std::set<std::string> &marked_package_keys,
                                               const PackageRow &selected,
                                               const PendingTransactionActionRows &action_rows,
                                               bool projects_upgrade_actions)
{
  const bool exact_installonly_update = action_rows.state == PackageInstallState::UPGRADEABLE &&
      action_rows.has_install_row && action_rows.install_row.installonly && selected.is_newest_available &&
      selected.nevra == action_rows.install_row.nevra;

  if ((!action_rows.install_is_upgrade && !exact_installonly_update) || action_rows.package_key.empty()) {
    return false;
  }

  if (!pending_transaction_selection_allows_install_action(selected, action_rows, projects_upgrade_actions)) {
    return false;
  }

  if (marked_package_keys.count(action_rows.package_key) > 0) {
    return false;
  }

  if (!pending_transaction_mark_install_side_action(actions, action_rows)) {
    return false;
  }

  marked_package_keys.insert(action_rows.package_key);
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
// Return true when activating this visible row should use the remove action.
// -----------------------------------------------------------------------------
bool
pending_transaction_activation_should_remove(const PackageRow &selected, const PendingTransactionActionRows &rows)
{
  return rows.has_installed_row && selected.nevra == rows.installed_row.nevra && !rows.install_is_upgrade;
}

// -----------------------------------------------------------------------------
// EOF
// -----------------------------------------------------------------------------
