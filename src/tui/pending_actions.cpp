// -----------------------------------------------------------------------------
// src/tui/pending_actions.cpp
// -----------------------------------------------------------------------------
#include "pending_actions.hpp"

#include "ui/package_query/package_query_state.hpp"
#include "dnf_backend/base_manager.hpp"

#include <algorithm>
#include <iostream>
#include <stdexcept>
#include <utility>

namespace tui {

PendingTransactionSelectionActions
pending_choices(const DisplayRow &row)
{
  const auto *target = row.upgrade ? &row.upgrade->target : nullptr;
  const uint64_t generation = row.upgrade ? row.upgrade->generation : 0;
  // The TUI acts on exact visible rows, not projected actions on another version.
  const DisplayedPackageQueryState displayed;
  auto choices = pending_transaction_actions_for_selection(row.package, target, generation, displayed, {});

  // Upgrades are offered only for targets reported by dnf5daemon.
  if (choices.rows.install_is_upgrade && !target) {
    choices.can_install = false;
  }
  return choices;
}

PendingAction::Type
install_side_type(const PendingTransactionActionRows &rows)
{
  if (rows.install_is_upgrade) {
    return PendingAction::UPGRADE;
  }
  if (rows.install_is_downgrade) {
    return PendingAction::DOWNGRADE;
  }
  return PendingAction::INSTALL;
}

const char *
pending_action_name(PendingAction::Type type)
{
  switch (type) {
  case PendingAction::INSTALL:
    return "Install";
  case PendingAction::UPGRADE:
    return "Upgrade";
  case PendingAction::DOWNGRADE:
    return "Downgrade";
  case PendingAction::REMOVE:
    return "Remove";
  case PendingAction::REINSTALL:
    return "Reinstall";
  }
  return "Unknown";
}

bool
PendingMark::has_issue() const
{
  return !issue.empty() || !preview_issue.empty();
}

std::string
PendingMark::issue_text() const
{
  if (issue.empty()) {
    return preview_issue;
  }
  if (preview_issue.empty()) {
    return issue;
  }
  return issue + "\n\n" + preview_issue;
}

void
update_pending_preview_issues(std::vector<PendingMark> &marks, std::vector<std::string> issues)
{
  if (issues.size() != marks.size()) {
    return;
  }
  for (size_t i = 0; i < marks.size(); ++i) {
    marks[i].preview_issue = std::move(issues[i]);
  }
}

bool
mark_pending_action(std::vector<PendingMark> &marks, const DisplayRow &row, PendingAction::Type type)
{
  const auto choices = pending_choices(row);
  std::vector<PendingAction> actions;
  actions.reserve(marks.size());
  for (const auto &mark : marks) {
    actions.push_back(mark.action);
  }

  bool marked = false;
  switch (type) {
  case PendingAction::INSTALL:
  case PendingAction::UPGRADE:
  case PendingAction::DOWNGRADE:
    marked = choices.can_install && type == install_side_type(choices.rows) &&
        pending_transaction_mark_install_side_action(actions, choices.rows);
    break;
  case PendingAction::REMOVE:
    marked = choices.can_remove && pending_transaction_mark_remove_action(actions, choices.rows);
    break;
  case PendingAction::REINSTALL:
    marked = choices.can_reinstall && pending_transaction_mark_reinstall_action(actions, choices.rows);
    break;
  }
  if (!marked) {
    return false;
  }

  // The shared marking helpers append the new action after removing conflicts.
  // Retain the original source and validation status of every surviving mark.
  std::vector<PendingMark> next;
  next.reserve(actions.size());
  for (size_t i = 0; i + 1 < actions.size(); ++i) {
    const auto &action = actions[i];
    const auto existing = std::find_if(marks.begin(), marks.end(), [&](const PendingMark &mark) {
      return mark.action.nevra == action.nevra && mark.action.type == action.type;
    });
    if (existing == marks.end()) {
      throw std::logic_error("Pending action context is missing.");
    }
    next.push_back(*existing);
  }
  next.push_back({ std::move(actions.back()), row, {} });
  marks = std::move(next);
  return true;
}

std::string
pending_snapshot_issue(const PendingMark &mark, const DaemonUpgradeSnapshot &snapshot)
{
  const auto &action = mark.action;
  const auto &row = mark.source;
  const auto installed = dnf_backend_resolve_installed_package(row.package);
  switch (action.type) {
  case PendingAction::INSTALL:
    if (installed.exact_installed || (installed.has_installed_row && !action.installonly)) {
      return "Invalid: package is already installed.";
    }
    break;
  case PendingAction::REMOVE:
  case PendingAction::REINSTALL:
    if (!installed.exact_installed) {
      return "Invalid: marked version is no longer installed.";
    }
    if (installed.self_protected) {
      return "Invalid: running application is protected.";
    }
    break;
  case PendingAction::DOWNGRADE:
    if (installed.self_protected || installed.state != PackageInstallState::DOWNGRADEABLE) {
      return "Invalid: downgrade is no longer available.";
    }
    break;
  case PendingAction::UPGRADE: {
    if (installed.exact_installed || !row.upgrade) {
      return "Invalid: marked upgrade is no longer applicable.";
    }
    if (snapshot.status != DaemonUpgradeSnapshotStatus::READY) {
      return "Unchecked: current daemon targets are needed.";
    }
    // A new generation is acceptable only if the original target still matches.
    if (!DaemonUpgradeState::instance().is_current_target(row.upgrade->target, snapshot.generation)) {
      return "Invalid: daemon upgrade target changed.";
    }
    return {};
  }
  }
  return {};
}

std::vector<std::string>
validate_pending_marks(const std::vector<PendingMark> &marks,
                       GCancellable *cancel,
                       const std::shared_ptr<std::atomic<bool>> &cancel_flag)
{
  std::vector<std::string> issues;
  if (marks.empty() || g_cancellable_is_cancelled(cancel)) {
    return issues;
  }

  auto &manager = BaseManager::instance();
  auto &state = DaemonUpgradeState::instance();
  state.mark_stale();
  if (dnf_backend_refresh_installed_snapshot_preserving_cached_base()) {
    manager.drop_cached_base();
  }
  if (g_cancellable_is_cancelled(cancel)) {
    return {};
  }

  std::vector<std::string> nevras;
  bool needs_upgrades = false;
  for (const auto &mark : marks) {
    if (mark.action.type == PendingAction::UPGRADE) {
      needs_upgrades = true;
    } else if (mark.action.type != PendingAction::REMOVE) {
      nevras.push_back(mark.action.nevra);
    }
  }
  if (needs_upgrades) {
    try {
      query_upgrades(false, cancel, cancel_flag);
    } catch (const std::exception &error) {
      // Local actions can still be checked when the daemon is unavailable.
      std::cerr << "Pending upgrade validation failed: " << error.what() << '\n';
    }
  }
  if (g_cancellable_is_cancelled(cancel)) {
    return {};
  }

  std::vector<PackageRow> available;
  bool repository_checked = false;
  if (!nevras.empty()) {
    try {
      manager.rebuild(BaseRefreshMode::NORMAL, cancel_flag);
      available = dnf_backend_get_available_package_metadata_by_nevras_interruptible(nevras, cancel);
      repository_checked = manager.current_repo_state() == BaseRepoState::LIVE_METADATA;
    } catch (const std::exception &error) {
      // Missing metadata must not be mistaken for a missing package.
      std::cerr << "Pending repository validation failed: " << error.what() << '\n';
    }
  }
  if (g_cancellable_is_cancelled(cancel)) {
    return {};
  }

  // Repository loading may take time. Do not accept a daemon result if
  // installed identities changed while the other checks were running.
  if (dnf_backend_refresh_installed_snapshot_preserving_cached_base()) {
    state.mark_stale();
    manager.drop_cached_base();
    repository_checked = false;
  }
  if (g_cancellable_is_cancelled(cancel)) {
    return {};
  }

  const auto snapshot = state.snapshot();
  issues.reserve(marks.size());
  for (const auto &mark : marks) {
    std::string issue = pending_snapshot_issue(mark, snapshot);
    const bool needs_repository =
        mark.action.type != PendingAction::REMOVE && mark.action.type != PendingAction::UPGRADE;
    if (issue.empty() && needs_repository && !repository_checked) {
      issue = "Unchecked: repository availability needs checking.";
    } else if (issue.empty() && needs_repository) {
      const auto found = std::find_if(
          available.begin(), available.end(), [&](const PackageRow &row) { return row.nevra == mark.action.nevra; });
      if (found == available.end()) {
        issue = "Invalid: marked version is no longer available.";
      } else if (mark.action.type == PendingAction::INSTALL && found->installonly != mark.action.installonly) {
        issue = "Invalid: installonly classification changed.";
      } else {
        issue.clear();
      }
    }
    issues.push_back(std::move(issue));
  }
  return issues;
}

} // namespace tui

// -----------------------------------------------------------------------------
// EOF
// -----------------------------------------------------------------------------
