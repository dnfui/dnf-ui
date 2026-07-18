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
};

using DaemonUpgradeRefreshId = uint64_t;

// The shared state holds the last List Upgradable result accepted by GTK.
// It does not fetch data from dnf5daemon. Workers load targets, then GTK
// publishes the result only after cancellation and state checks pass.
//
// Table rows keep the snapshot generation they were built from. Later action
// handling uses that generation to reject stale rows instead of marking an
// upgrade from an old daemon result.
//
// Refresh IDs let only the worker that started the current refresh publish or
// fail it. Cancelled workers abandon their refresh instead of marking newer work stale.
//
// Own one active daemon refresh until the GTK completion either publishes it or
// rejects it. Destruction abandons an unclosed refresh so a cancelled worker
// cannot keep the shared state locked in REFRESHING.
class DaemonUpgradeRefreshOwner {
  public:
  explicit DaemonUpgradeRefreshOwner(DaemonUpgradeRefreshId refresh_id = 0);
  ~DaemonUpgradeRefreshOwner();

  DaemonUpgradeRefreshOwner(const DaemonUpgradeRefreshOwner &) = delete;
  DaemonUpgradeRefreshOwner &operator=(const DaemonUpgradeRefreshOwner &) = delete;
  DaemonUpgradeRefreshOwner(DaemonUpgradeRefreshOwner &&other) noexcept;
  DaemonUpgradeRefreshOwner &operator=(DaemonUpgradeRefreshOwner &&other) noexcept;

  DaemonUpgradeRefreshId id() const;
  void close();

  private:
  DaemonUpgradeRefreshId refresh_id = 0;
  bool closed = true;
};

class DaemonUpgradeState {
  public:
  static DaemonUpgradeState &instance();

  DaemonUpgradeSnapshot snapshot() const;
  bool is_current_target(const TransactionServiceUpgradeTarget &target, uint64_t generation) const;
  std::optional<DaemonUpgradeRefreshId> begin_refresh();
  bool publish_success(DaemonUpgradeRefreshId refresh_id,
                       const std::vector<TransactionServiceUpgradeTarget> &targets,
                       std::string &error_out);
  void publish_failure(DaemonUpgradeRefreshId refresh_id);
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
