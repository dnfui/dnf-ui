// -----------------------------------------------------------------------------
// src/upgrade/daemon_upgrade_state.cpp
// Shared daemon upgrade snapshot state
// -----------------------------------------------------------------------------
#include "upgrade/daemon_upgrade_state.hpp"

// -----------------------------------------------------------------------------
// Return the process-wide daemon upgrade state.
// -----------------------------------------------------------------------------
DaemonUpgradeState &
DaemonUpgradeState::instance()
{
  static DaemonUpgradeState state;
  return state;
}

// -----------------------------------------------------------------------------
// Return the current daemon upgrade snapshot.
// -----------------------------------------------------------------------------
DaemonUpgradeSnapshot
DaemonUpgradeState::snapshot() const
{
  std::lock_guard<std::mutex> lock(mutex);
  return current;
}

// -----------------------------------------------------------------------------
// Mark the shared snapshot as refreshing.
// -----------------------------------------------------------------------------
bool
DaemonUpgradeState::begin_refresh()
{
  std::lock_guard<std::mutex> lock(mutex);
  if (current.status == DaemonUpgradeSnapshotStatus::REFRESHING) {
    return false;
  }

  current.status = DaemonUpgradeSnapshotStatus::REFRESHING;
  current.targets_by_name_arch.clear();
  current.error.clear();
  return true;
}

// -----------------------------------------------------------------------------
// Replace the shared snapshot with one complete daemon result.
// -----------------------------------------------------------------------------
bool
DaemonUpgradeState::publish_success(const std::vector<TransactionServiceUpgradeTarget> &targets, std::string &error_out)
{
  error_out.clear();
  std::map<std::string, TransactionServiceUpgradeTarget> next_targets;

  for (const auto &target : targets) {
    const std::string key = target.name_arch_key();
    auto existing = next_targets.find(key);

    if (existing == next_targets.end()) {
      next_targets.emplace(key, target);
      continue;
    }

    if (existing->second.full_nevra == target.full_nevra) {
      continue;
    }

    error_out = "dnf5daemon returned more than one upgrade target for " + target.upgrade_spec() + ".";
    std::lock_guard<std::mutex> lock(mutex);
    if (current.status == DaemonUpgradeSnapshotStatus::REFRESHING) {
      current.status = DaemonUpgradeSnapshotStatus::ERROR;
      current.targets_by_name_arch.clear();
      current.error = error_out;
    }
    return false;
  }

  std::lock_guard<std::mutex> lock(mutex);
  if (current.status != DaemonUpgradeSnapshotStatus::REFRESHING) {
    error_out = "dnf5daemon upgrade refresh is no longer active.";
    return false;
  }

  current.generation += 1;
  current.status = DaemonUpgradeSnapshotStatus::READY;
  current.targets_by_name_arch = std::move(next_targets);
  current.error.clear();
  return true;
}

// -----------------------------------------------------------------------------
// Store a failed daemon upgrade snapshot request.
// -----------------------------------------------------------------------------
void
DaemonUpgradeState::publish_failure(const std::string &error)
{
  std::lock_guard<std::mutex> lock(mutex);
  if (current.status != DaemonUpgradeSnapshotStatus::REFRESHING) {
    return;
  }

  current.status = DaemonUpgradeSnapshotStatus::ERROR;
  current.targets_by_name_arch.clear();
  current.error = error;
}

// -----------------------------------------------------------------------------
// Mark the snapshot stale when package or repository state may have changed.
// -----------------------------------------------------------------------------
void
DaemonUpgradeState::mark_stale()
{
  std::lock_guard<std::mutex> lock(mutex);
  current.status = DaemonUpgradeSnapshotStatus::STALE;
  current.targets_by_name_arch.clear();
  current.error.clear();
}

#ifdef DNFUI_BUILD_TESTS
// -----------------------------------------------------------------------------
// Reset the shared state for unit tests.
// -----------------------------------------------------------------------------
void
DaemonUpgradeState::reset_for_tests()
{
  std::lock_guard<std::mutex> lock(mutex);
  current = DaemonUpgradeSnapshot();
}
#endif

// -----------------------------------------------------------------------------
// EOF
// -----------------------------------------------------------------------------
