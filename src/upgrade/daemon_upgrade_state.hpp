// -----------------------------------------------------------------------------
// src/upgrade/daemon_upgrade_state.hpp
// Shared daemon upgrade snapshot state
// Stores the latest complete upgrade-target result reported by dnf5daemon.
// -----------------------------------------------------------------------------
#pragma once

#include "dnf5daemon_client/transaction_service_client.hpp"

#include <cstdint>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

enum class DaemonUpgradeSnapshotStatus {
  NOT_LOADED,
  REFRESHING,
  READY,
  STALE,
  ERROR,
};

struct DaemonUpgradeSnapshot {
  uint64_t generation = 0;
  DaemonUpgradeSnapshotStatus status = DaemonUpgradeSnapshotStatus::NOT_LOADED;
  std::map<std::string, TransactionServiceUpgradeTarget> targets_by_name_arch;
  std::string error;
};

using DaemonUpgradeRefreshId = uint64_t;

class DaemonUpgradeState {
  public:
  static DaemonUpgradeState &instance();

  DaemonUpgradeSnapshot snapshot() const;
  std::optional<DaemonUpgradeRefreshId> begin_refresh();
  bool publish_success(DaemonUpgradeRefreshId refresh_id,
                       const std::vector<TransactionServiceUpgradeTarget> &targets,
                       std::string &error_out);
  void publish_failure(DaemonUpgradeRefreshId refresh_id, const std::string &error);
  bool abandon_refresh(DaemonUpgradeRefreshId refresh_id);
  void mark_stale();

#ifdef DNFUI_BUILD_TESTS
  void reset_for_tests();
#endif

  private:
  DaemonUpgradeState() = default;
  DaemonUpgradeState(const DaemonUpgradeState &) = delete;
  DaemonUpgradeState &operator=(const DaemonUpgradeState &) = delete;

  mutable std::mutex mutex;
  DaemonUpgradeSnapshot current;
  DaemonUpgradeRefreshId next_refresh_id = 1;
  std::optional<DaemonUpgradeRefreshId> active_refresh_id;
};

// -----------------------------------------------------------------------------
// EOF
// -----------------------------------------------------------------------------
