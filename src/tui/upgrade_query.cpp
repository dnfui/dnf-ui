// -----------------------------------------------------------------------------
// src/tui/upgrade_query.cpp
// -----------------------------------------------------------------------------
#include "upgrade_query.hpp"

#include "dnf_backend/base_manager.hpp"
#include "dnf5daemon_client/transaction_service_client.hpp"

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace tui {

std::string
refresh_daemon_metadata(GCancellable *cancel)
{
  std::string error;
  if (transaction_service_client_refresh_repositories(error, cancel)) {
    return {};
  }
  return error.empty() ? "Could not refresh dnf5daemon metadata." : error;
}

// Use the daemon's package list as the authority for upgrade targets.
// A local repository lookup must not add or remove targets from this view.
QueryResult
query_upgrades(bool refresh_metadata, GCancellable *cancel, const std::shared_ptr<std::atomic<bool>> &cancel_flag)
{
  QueryResult result;
  if (g_cancellable_is_cancelled(cancel)) {
    return result;
  }

  auto &state = DaemonUpgradeState::instance();
  const auto refresh_id = state.begin_refresh();
  if (!refresh_id) {
    throw std::runtime_error("dnf5daemon upgrade information is already being refreshed.");
  }
  DaemonUpgradeRefreshOwner owner(*refresh_id);

  dnf_backend_refresh_installed_snapshot();
  if (g_cancellable_is_cancelled(cancel)) {
    return result;
  }

  BaseRepoState local_repo_state = BaseRepoState::LIVE_METADATA;
  if (refresh_metadata) {
    const std::string error = refresh_daemon_metadata(cancel);
    if (g_cancellable_is_cancelled(cancel)) {
      return result;
    }
    if (!error.empty()) {
      state.publish_failure(owner.id());
      owner.close();
    }

    local_repo_state = BaseManager::instance().rebuild(BaseRefreshMode::FORCE_METADATA_CHECK, cancel_flag);
    if (!error.empty()) {
      throw std::runtime_error(error);
    }
    if (g_cancellable_is_cancelled(cancel)) {
      return result;
    }
  }

  std::vector<DaemonUpgradeTarget> targets;
  std::string error;
  if (!transaction_service_client_list_upgrade_targets(targets, error, cancel)) {
    if (g_cancellable_is_cancelled(cancel)) {
      return result;
    }
    state.publish_failure(owner.id());
    owner.close();
    throw std::runtime_error(error.empty() ? "Could not list upgradable packages from dnf5daemon." : error);
  }
  if (g_cancellable_is_cancelled(cancel)) {
    return result;
  }

  // Keep the locally refreshed Base for subsequent views unless installed state changed.
  if (dnf_backend_refresh_installed_snapshot_preserving_cached_base()) {
    BaseManager::instance().drop_cached_base();
    throw std::runtime_error("Installed packages changed while listing upgrades. Refresh the view.");
  }
  if (g_cancellable_is_cancelled(cancel)) {
    return result;
  }

  if (state.publish_success(owner.id(), targets, error) != DaemonUpgradePublishResult::PUBLISHED) {
    throw std::runtime_error(error.empty() ? "Could not accept dnf5daemon upgrade targets." : error);
  }
  owner.close();

  const auto snapshot = state.snapshot();
  result.rows.reserve(snapshot.targets_by_name_arch.size());
  for (const auto &entry : snapshot.targets_by_name_arch) {
    if (g_cancellable_is_cancelled(cancel)) {
      return {};
    }
    const auto &target = entry.second;
    PackageRow row;
    row.nevra = target.nevra.empty() ? target.full_nevra : target.nevra;
    row.name = target.name;
    row.epoch = target.epoch;
    row.version = target.version;
    row.release = target.release;
    row.arch = target.arch;
    row.repo = target.repo_id;
    row.summary = target.summary;
    result.rows.push_back({
        std::move(row),
        false,
        std::make_shared<UpgradeRowContext>(UpgradeRowContext { target, snapshot.generation }),
    });
  }
  std::sort(result.rows.begin(), result.rows.end(), [](const DisplayRow &a, const DisplayRow &b) {
    return a.package.nevra < b.package.nevra;
  });
  result.notice = std::to_string(result.rows.size()) + " upgrade targets (dnf5daemon)";
  if (local_repo_state == BaseRepoState::INSTALLED_ONLY) {
    result.notice += " (local metadata unavailable)";
  } else if (local_repo_state == BaseRepoState::CACHED_METADATA) {
    result.notice += " (cached local metadata)";
  }
  return result;
}

} // namespace tui

// -----------------------------------------------------------------------------
// EOF
// -----------------------------------------------------------------------------
