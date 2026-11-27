// -----------------------------------------------------------------------------
// src/tui/package_query.cpp
// -----------------------------------------------------------------------------
#include "package_query.hpp"

#include "dnf_backend/base_manager.hpp"

#include <algorithm>
#include <iostream>
#include <utility>

namespace tui {

QueryResult
query_packages(PackageView view,
               const std::string &term,
               bool refresh_snapshot,
               bool refresh_metadata,
               GCancellable *cancel,
               const std::shared_ptr<std::atomic<bool>> &cancel_flag)
{
  QueryResult result;
  if (g_cancellable_is_cancelled(cancel)) {
    return result;
  }

  if (view == PackageView::UPGRADABLE) {
    return tui::query_upgrades(refresh_metadata, cancel, cancel_flag);
  }
  DaemonUpgradeState::instance().mark_stale();

  // A new Base already reads the rpmdb. On later queries, check for
  // external changes without discarding usable repository metadata.
  auto &manager = BaseManager::instance();
  std::string daemon_refresh_error;
  if (refresh_metadata) {
    daemon_refresh_error = tui::refresh_daemon_metadata(cancel);
    if (g_cancellable_is_cancelled(cancel)) {
      return result;
    }
    manager.rebuild(BaseRefreshMode::FORCE_METADATA_CHECK, cancel_flag);
  } else if (view == PackageView::UNNEEDED) {
    // Installation reasons can change without changing installed NEVRAs.
    // Reload system state using normal repository expiration rules.
    manager.rebuild(BaseRefreshMode::NORMAL, cancel_flag);
  } else if (refresh_snapshot) {
    const bool changed = dnf_backend_refresh_installed_snapshot_preserving_cached_base();
    if (g_cancellable_is_cancelled(cancel)) {
      return result;
    }
    if (changed || manager.current_repo_state() != BaseRepoState::LIVE_METADATA) {
      manager.drop_cached_base();
    }
  }

  if (g_cancellable_is_cancelled(cancel)) {
    return result;
  }
  std::vector<PackageRow> packages;
  switch (view) {
  case PackageView::INSTALLED:
    packages = dnf_backend_get_installed_package_rows_interruptible(cancel);
    break;
  case PackageView::ALL:
    packages = dnf_backend_get_browse_package_rows_interruptible({}, cancel);
    break;
  case PackageView::UNNEEDED:
    packages = dnf_backend_get_unneeded_package_rows_interruptible(cancel);
    break;
  case PackageView::SEARCH:
    packages = dnf_backend_search_package_rows_interruptible(term, {}, cancel);
    break;
  case PackageView::UPGRADABLE:
    break;
  }
  if (g_cancellable_is_cancelled(cancel)) {
    return result;
  }
  const BaseRepoState repo_state = manager.current_repo_state();
  std::sort(
      packages.begin(), packages.end(), [](const PackageRow &a, const PackageRow &b) { return a.nevra < b.nevra; });

  result.rows.reserve(packages.size());
  for (auto &package : packages) {
    if (g_cancellable_is_cancelled(cancel)) {
      return result;
    }
    const bool exact_installed = dnf_backend_resolve_installed_package(package).exact_installed;
    result.rows.push_back({ std::move(package), exact_installed });
  }
  result.notice = std::to_string(result.rows.size()) + " packages";
  if (repo_state == BaseRepoState::INSTALLED_ONLY) {
    result.notice += " (repository metadata unavailable)";
  } else if (repo_state == BaseRepoState::CACHED_METADATA) {
    result.notice += " (cached repository metadata)";
  }
  if (!daemon_refresh_error.empty()) {
    std::cerr << "dnf5daemon refresh failed: " << daemon_refresh_error << '\n';
    result.notice = "Daemon refresh failed; " + result.notice;
  }
  return result;
}

} // namespace tui

// -----------------------------------------------------------------------------
// EOF
// -----------------------------------------------------------------------------
