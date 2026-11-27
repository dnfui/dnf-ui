// -----------------------------------------------------------------------------
// src/tui/upgrade_query.hpp
// -----------------------------------------------------------------------------
#pragma once

#include "dnf_backend/dnf_backend.hpp"
#include "upgrade/daemon_upgrade_state.hpp"

#include <atomic>
#include <memory>
#include <string>
#include <vector>

namespace tui {

struct UpgradeRowContext {
  DaemonUpgradeTarget target;
  uint64_t generation;
};

struct DisplayRow {
  PackageRow package;
  bool installed;
  std::shared_ptr<const UpgradeRowContext> upgrade {};
};

struct QueryResult {
  std::vector<DisplayRow> rows;
  std::string details;
  std::string notice;
};

// Refresh daemon metadata and return an error message on failure.
std::string refresh_daemon_metadata(GCancellable *cancel);

QueryResult
query_upgrades(bool refresh_metadata, GCancellable *cancel, const std::shared_ptr<std::atomic<bool>> &cancel_flag);

} // namespace tui

// -----------------------------------------------------------------------------
// EOF
// -----------------------------------------------------------------------------
