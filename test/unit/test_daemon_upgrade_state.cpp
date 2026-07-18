// -----------------------------------------------------------------------------
// test/unit/test_daemon_upgrade_state.cpp
// Shared daemon upgrade state tests
// -----------------------------------------------------------------------------
#include <catch2/catch_test_macros.hpp>

#include "upgrade/daemon_upgrade_state.hpp"

#include <optional>
#include <string>
#include <vector>

namespace {

// -----------------------------------------------------------------------------
// Build one daemon upgrade target for state publication tests.
// -----------------------------------------------------------------------------
TransactionServiceUpgradeTarget
make_target(const std::string &name, const std::string &arch, const std::string &version, const std::string &full_nevra)
{
  TransactionServiceUpgradeTarget target;
  target.name = name;
  target.arch = arch;
  target.epoch = "0";
  target.version = version;
  target.release = "1.fc44";
  target.nevra = name + "-" + version + "-1.fc44." + arch;
  target.full_nevra = full_nevra;
  target.repo_id = "updates";
  return target;
}

// -----------------------------------------------------------------------------
// Reset the process-wide state before each test.
// -----------------------------------------------------------------------------
DaemonUpgradeState &
reset_state()
{
  DaemonUpgradeState &state = DaemonUpgradeState::instance();
  state.reset_for_tests();
  return state;
}

} // namespace

// -----------------------------------------------------------------------------
// Verify the default snapshot is unknown, not an empty upgrade list.
// -----------------------------------------------------------------------------
TEST_CASE("daemon upgrade state starts as not loaded")
{
  DaemonUpgradeState &state = reset_state();

  DaemonUpgradeSnapshot snapshot = state.snapshot();
  REQUIRE(snapshot.status == DaemonUpgradeSnapshotStatus::NOT_LOADED);
  REQUIRE(snapshot.generation == 0);
  REQUIRE(snapshot.targets_by_name_arch.empty());
  REQUIRE(snapshot.error.empty());
}

// -----------------------------------------------------------------------------
// Verify that only one daemon upgrade refresh can own the shared state.
// -----------------------------------------------------------------------------
TEST_CASE("daemon upgrade state rejects overlapping refreshes")
{
  DaemonUpgradeState &state = reset_state();

  std::optional<DaemonUpgradeRefreshId> refresh_id = state.begin_refresh();
  REQUIRE(refresh_id.has_value());
  REQUIRE_FALSE(state.begin_refresh());

  DaemonUpgradeSnapshot snapshot = state.snapshot();
  REQUIRE(snapshot.status == DaemonUpgradeSnapshotStatus::REFRESHING);
  REQUIRE(snapshot.generation == 0);
  REQUIRE(snapshot.targets_by_name_arch.empty());
}

// -----------------------------------------------------------------------------
// Verify that a complete daemon result replaces the shared snapshot.
// -----------------------------------------------------------------------------
TEST_CASE("daemon upgrade state publishes successful targets")
{
  DaemonUpgradeState &state = reset_state();
  std::string error;
  std::vector<TransactionServiceUpgradeTarget> targets {
    make_target("demo", "x86_64", "2.0", "demo-0:2.0-1.fc44.x86_64"),
  };

  std::optional<DaemonUpgradeRefreshId> refresh_id = state.begin_refresh();
  REQUIRE(refresh_id.has_value());
  REQUIRE(state.publish_success(refresh_id.value(), targets, error));
  REQUIRE(error.empty());

  DaemonUpgradeSnapshot snapshot = state.snapshot();
  REQUIRE(snapshot.status == DaemonUpgradeSnapshotStatus::READY);
  REQUIRE(snapshot.generation == 1);
  REQUIRE(snapshot.error.empty());
  REQUIRE(snapshot.targets_by_name_arch.size() == 1);
  REQUIRE(snapshot.targets_by_name_arch.count("demo\nx86_64") == 1);
  REQUIRE(snapshot.targets_by_name_arch.at("demo\nx86_64").full_nevra == "demo-0:2.0-1.fc44.x86_64");
}

// -----------------------------------------------------------------------------
// Verify that an empty daemon result is a valid ready snapshot.
// -----------------------------------------------------------------------------
TEST_CASE("daemon upgrade state publishes empty successful targets")
{
  DaemonUpgradeState &state = reset_state();
  std::string error;

  std::optional<DaemonUpgradeRefreshId> refresh_id = state.begin_refresh();
  REQUIRE(refresh_id.has_value());
  REQUIRE(state.publish_success(refresh_id.value(), {}, error));
  REQUIRE(error.empty());

  DaemonUpgradeSnapshot snapshot = state.snapshot();
  REQUIRE(snapshot.status == DaemonUpgradeSnapshotStatus::READY);
  REQUIRE(snapshot.generation == 1);
  REQUIRE(snapshot.targets_by_name_arch.empty());
  REQUIRE(snapshot.error.empty());
}

// -----------------------------------------------------------------------------
// Verify that failures do not look like a successful empty daemon result.
// -----------------------------------------------------------------------------
TEST_CASE("daemon upgrade state publishes failure")
{
  DaemonUpgradeState &state = reset_state();

  std::optional<DaemonUpgradeRefreshId> refresh_id = state.begin_refresh();
  REQUIRE(refresh_id.has_value());
  state.publish_failure(refresh_id.value(), "daemon failed");

  DaemonUpgradeSnapshot snapshot = state.snapshot();
  REQUIRE(snapshot.status == DaemonUpgradeSnapshotStatus::ERROR);
  REQUIRE(snapshot.generation == 0);
  REQUIRE(snapshot.targets_by_name_arch.empty());
  REQUIRE(snapshot.error == "daemon failed");
}

// -----------------------------------------------------------------------------
// Verify that stale state is not returned as current upgrade information.
// -----------------------------------------------------------------------------
TEST_CASE("daemon upgrade state marks snapshots stale")
{
  DaemonUpgradeState &state = reset_state();
  std::string error;
  std::vector<TransactionServiceUpgradeTarget> targets {
    make_target("demo", "x86_64", "2.0", "demo-0:2.0-1.fc44.x86_64"),
  };

  std::optional<DaemonUpgradeRefreshId> refresh_id = state.begin_refresh();
  REQUIRE(refresh_id.has_value());
  REQUIRE(state.publish_success(refresh_id.value(), targets, error));
  state.mark_stale();

  DaemonUpgradeSnapshot snapshot = state.snapshot();
  REQUIRE(snapshot.status == DaemonUpgradeSnapshotStatus::STALE);
  REQUIRE(snapshot.generation == 1);
  REQUIRE(snapshot.targets_by_name_arch.empty());
  REQUIRE(snapshot.error.empty());
}

// -----------------------------------------------------------------------------
// Verify that exact duplicate daemon targets are collapsed.
// -----------------------------------------------------------------------------
TEST_CASE("daemon upgrade state collapses exact duplicate targets")
{
  DaemonUpgradeState &state = reset_state();
  std::string error;
  std::vector<TransactionServiceUpgradeTarget> targets {
    make_target("demo", "x86_64", "2.0", "demo-0:2.0-1.fc44.x86_64"),
    make_target("demo", "x86_64", "2.0", "demo-0:2.0-1.fc44.x86_64"),
  };

  std::optional<DaemonUpgradeRefreshId> refresh_id = state.begin_refresh();
  REQUIRE(refresh_id.has_value());
  REQUIRE(state.publish_success(refresh_id.value(), targets, error));

  DaemonUpgradeSnapshot snapshot = state.snapshot();
  REQUIRE(snapshot.status == DaemonUpgradeSnapshotStatus::READY);
  REQUIRE(snapshot.generation == 1);
  REQUIRE(snapshot.targets_by_name_arch.size() == 1);
}

// -----------------------------------------------------------------------------
// Verify that conflicting daemon targets reject the whole result.
// -----------------------------------------------------------------------------
TEST_CASE("daemon upgrade state rejects conflicting duplicate targets")
{
  DaemonUpgradeState &state = reset_state();
  std::string error;
  std::vector<TransactionServiceUpgradeTarget> first_targets {
    make_target("demo", "x86_64", "2.0", "demo-0:2.0-1.fc44.x86_64"),
  };
  std::vector<TransactionServiceUpgradeTarget> conflicting_targets {
    make_target("demo", "x86_64", "2.0", "demo-0:2.0-1.fc44.x86_64"),
    make_target("demo", "x86_64", "3.0", "demo-0:3.0-1.fc44.x86_64"),
  };

  std::optional<DaemonUpgradeRefreshId> first_refresh_id = state.begin_refresh();
  REQUIRE(first_refresh_id.has_value());
  REQUIRE(state.publish_success(first_refresh_id.value(), first_targets, error));
  std::optional<DaemonUpgradeRefreshId> second_refresh_id = state.begin_refresh();
  REQUIRE(second_refresh_id.has_value());
  REQUIRE_FALSE(state.publish_success(second_refresh_id.value(), conflicting_targets, error));
  REQUIRE(error.find("demo.x86_64") != std::string::npos);

  DaemonUpgradeSnapshot snapshot = state.snapshot();
  REQUIRE(snapshot.status == DaemonUpgradeSnapshotStatus::ERROR);
  REQUIRE(snapshot.generation == 1);
  REQUIRE(snapshot.targets_by_name_arch.empty());
  REQUIRE(snapshot.error.find("demo.x86_64") != std::string::npos);
}

// -----------------------------------------------------------------------------
// Verify that successful publication recovers from error and stale states.
// -----------------------------------------------------------------------------
TEST_CASE("daemon upgrade state recovers after error and stale states")
{
  DaemonUpgradeState &state = reset_state();
  std::string error;
  std::vector<TransactionServiceUpgradeTarget> targets {
    make_target("demo", "x86_64", "2.0", "demo-0:2.0-1.fc44.x86_64"),
  };

  std::optional<DaemonUpgradeRefreshId> failed_refresh_id = state.begin_refresh();
  REQUIRE(failed_refresh_id.has_value());
  state.publish_failure(failed_refresh_id.value(), "daemon failed");
  error = "old error";
  std::optional<DaemonUpgradeRefreshId> error_recovery_id = state.begin_refresh();
  REQUIRE(error_recovery_id.has_value());
  REQUIRE(state.publish_success(error_recovery_id.value(), targets, error));
  REQUIRE(error.empty());
  REQUIRE(state.snapshot().status == DaemonUpgradeSnapshotStatus::READY);
  REQUIRE(state.snapshot().generation == 1);

  state.mark_stale();
  std::optional<DaemonUpgradeRefreshId> stale_recovery_id = state.begin_refresh();
  REQUIRE(stale_recovery_id.has_value());
  REQUIRE(state.publish_success(stale_recovery_id.value(), targets, error));
  REQUIRE(state.snapshot().status == DaemonUpgradeSnapshotStatus::READY);
  REQUIRE(state.snapshot().generation == 2);
}

// -----------------------------------------------------------------------------
// Verify that old daemon results cannot publish after refresh ownership is lost.
// -----------------------------------------------------------------------------
TEST_CASE("daemon upgrade state rejects publication after stale")
{
  DaemonUpgradeState &state = reset_state();
  std::string error;
  std::vector<TransactionServiceUpgradeTarget> targets {
    make_target("demo", "x86_64", "2.0", "demo-0:2.0-1.fc44.x86_64"),
  };

  std::optional<DaemonUpgradeRefreshId> refresh_id = state.begin_refresh();
  REQUIRE(refresh_id.has_value());
  state.mark_stale();
  REQUIRE_FALSE(state.publish_success(refresh_id.value(), targets, error));
  REQUIRE(error.find("no longer active") != std::string::npos);
  state.publish_failure(refresh_id.value(), "old failure");

  DaemonUpgradeSnapshot snapshot = state.snapshot();
  REQUIRE(snapshot.status == DaemonUpgradeSnapshotStatus::STALE);
  REQUIRE(snapshot.generation == 0);
  REQUIRE(snapshot.targets_by_name_arch.empty());
  REQUIRE(snapshot.error.empty());
}

// -----------------------------------------------------------------------------
// Verify that cancellation can abandon the refresh it owns.
// -----------------------------------------------------------------------------
TEST_CASE("daemon upgrade state abandons active refresh")
{
  DaemonUpgradeState &state = reset_state();

  std::optional<DaemonUpgradeRefreshId> refresh_id = state.begin_refresh();
  REQUIRE(refresh_id.has_value());
  REQUIRE(state.abandon_refresh(refresh_id.value()));

  DaemonUpgradeSnapshot snapshot = state.snapshot();
  REQUIRE(snapshot.status == DaemonUpgradeSnapshotStatus::NOT_LOADED);
  REQUIRE(snapshot.generation == 0);
  REQUIRE(snapshot.targets_by_name_arch.empty());
  REQUIRE(snapshot.error.empty());
}

// -----------------------------------------------------------------------------
// Verify that abandoning a refresh releases ownership for the next refresh.
// -----------------------------------------------------------------------------
TEST_CASE("daemon upgrade state allows refresh after abandon")
{
  DaemonUpgradeState &state = reset_state();

  std::optional<DaemonUpgradeRefreshId> refresh_id = state.begin_refresh();
  REQUIRE(refresh_id.has_value());
  REQUIRE(state.abandon_refresh(refresh_id.value()));

  std::optional<DaemonUpgradeRefreshId> next_refresh_id = state.begin_refresh();
  REQUIRE(next_refresh_id.has_value());
  REQUIRE(next_refresh_id.value() != refresh_id.value());
  REQUIRE(state.snapshot().status == DaemonUpgradeSnapshotStatus::REFRESHING);
}

// -----------------------------------------------------------------------------
// Verify that abandoning a later refresh of a ready snapshot makes it stale.
// -----------------------------------------------------------------------------
TEST_CASE("daemon upgrade state abandons later refresh as stale")
{
  DaemonUpgradeState &state = reset_state();
  std::string error;
  std::vector<TransactionServiceUpgradeTarget> targets {
    make_target("demo", "x86_64", "2.0", "demo-0:2.0-1.fc44.x86_64"),
  };

  std::optional<DaemonUpgradeRefreshId> first_refresh_id = state.begin_refresh();
  REQUIRE(first_refresh_id.has_value());
  REQUIRE(state.publish_success(first_refresh_id.value(), targets, error));

  std::optional<DaemonUpgradeRefreshId> second_refresh_id = state.begin_refresh();
  REQUIRE(second_refresh_id.has_value());
  REQUIRE(state.abandon_refresh(second_refresh_id.value()));

  DaemonUpgradeSnapshot snapshot = state.snapshot();
  REQUIRE(snapshot.status == DaemonUpgradeSnapshotStatus::STALE);
  REQUIRE(snapshot.generation == 1);
  REQUIRE(snapshot.targets_by_name_arch.empty());
  REQUIRE(snapshot.error.empty());
}

// -----------------------------------------------------------------------------
// Verify that an old refresh cannot abandon a newer refresh.
// -----------------------------------------------------------------------------
TEST_CASE("daemon upgrade state rejects old abandon during newer refresh")
{
  DaemonUpgradeState &state = reset_state();

  std::optional<DaemonUpgradeRefreshId> old_refresh_id = state.begin_refresh();
  REQUIRE(old_refresh_id.has_value());
  state.mark_stale();

  std::optional<DaemonUpgradeRefreshId> new_refresh_id = state.begin_refresh();
  REQUIRE(new_refresh_id.has_value());

  REQUIRE_FALSE(state.abandon_refresh(old_refresh_id.value()));
  REQUIRE(state.snapshot().status == DaemonUpgradeSnapshotStatus::REFRESHING);
}

// -----------------------------------------------------------------------------
// Verify that an old successful result cannot replace a newer refresh.
// -----------------------------------------------------------------------------
TEST_CASE("daemon upgrade state rejects old success during newer refresh")
{
  DaemonUpgradeState &state = reset_state();
  std::string error;
  std::vector<TransactionServiceUpgradeTarget> old_targets {
    make_target("demo", "x86_64", "2.0", "demo-0:2.0-1.fc44.x86_64"),
  };
  std::vector<TransactionServiceUpgradeTarget> new_targets {
    make_target("demo", "x86_64", "3.0", "demo-0:3.0-1.fc44.x86_64"),
  };

  std::optional<DaemonUpgradeRefreshId> old_refresh_id = state.begin_refresh();
  REQUIRE(old_refresh_id.has_value());
  state.mark_stale();

  std::optional<DaemonUpgradeRefreshId> new_refresh_id = state.begin_refresh();
  REQUIRE(new_refresh_id.has_value());

  REQUIRE_FALSE(state.publish_success(old_refresh_id.value(), old_targets, error));
  REQUIRE(error.find("no longer active") != std::string::npos);
  REQUIRE(state.snapshot().status == DaemonUpgradeSnapshotStatus::REFRESHING);

  REQUIRE(state.publish_success(new_refresh_id.value(), new_targets, error));

  DaemonUpgradeSnapshot snapshot = state.snapshot();
  REQUIRE(snapshot.status == DaemonUpgradeSnapshotStatus::READY);
  REQUIRE(snapshot.generation == 1);
  REQUIRE(snapshot.targets_by_name_arch.at("demo\nx86_64").full_nevra == "demo-0:3.0-1.fc44.x86_64");
}

// -----------------------------------------------------------------------------
// Verify that an old failure cannot replace a newer refresh.
// -----------------------------------------------------------------------------
TEST_CASE("daemon upgrade state ignores old failure during newer refresh")
{
  DaemonUpgradeState &state = reset_state();
  std::string error;
  std::vector<TransactionServiceUpgradeTarget> targets {
    make_target("demo", "x86_64", "3.0", "demo-0:3.0-1.fc44.x86_64"),
  };

  std::optional<DaemonUpgradeRefreshId> old_refresh_id = state.begin_refresh();
  REQUIRE(old_refresh_id.has_value());
  state.mark_stale();

  std::optional<DaemonUpgradeRefreshId> new_refresh_id = state.begin_refresh();
  REQUIRE(new_refresh_id.has_value());

  state.publish_failure(old_refresh_id.value(), "old failure");
  REQUIRE(state.snapshot().status == DaemonUpgradeSnapshotStatus::REFRESHING);

  REQUIRE(state.publish_success(new_refresh_id.value(), targets, error));

  DaemonUpgradeSnapshot snapshot = state.snapshot();
  REQUIRE(snapshot.status == DaemonUpgradeSnapshotStatus::READY);
  REQUIRE(snapshot.generation == 1);
  REQUIRE(snapshot.error.empty());
  REQUIRE(snapshot.targets_by_name_arch.at("demo\nx86_64").full_nevra == "demo-0:3.0-1.fc44.x86_64");
}

// -----------------------------------------------------------------------------
// Verify that an old conflicting result reports lost ownership, not target conflict.
// -----------------------------------------------------------------------------
TEST_CASE("daemon upgrade state reports inactive old conflict during newer refresh")
{
  DaemonUpgradeState &state = reset_state();
  std::string error;
  std::vector<TransactionServiceUpgradeTarget> conflicting_targets {
    make_target("demo", "x86_64", "2.0", "demo-0:2.0-1.fc44.x86_64"),
    make_target("demo", "x86_64", "3.0", "demo-0:3.0-1.fc44.x86_64"),
  };

  std::optional<DaemonUpgradeRefreshId> old_refresh_id = state.begin_refresh();
  REQUIRE(old_refresh_id.has_value());
  state.mark_stale();

  std::optional<DaemonUpgradeRefreshId> new_refresh_id = state.begin_refresh();
  REQUIRE(new_refresh_id.has_value());

  REQUIRE_FALSE(state.publish_success(old_refresh_id.value(), conflicting_targets, error));
  REQUIRE(error.find("no longer active") != std::string::npos);
  REQUIRE(error.find("more than one upgrade target") == std::string::npos);

  DaemonUpgradeSnapshot snapshot = state.snapshot();
  REQUIRE(snapshot.status == DaemonUpgradeSnapshotStatus::REFRESHING);
  REQUIRE(snapshot.generation == 0);
  REQUIRE(snapshot.error.empty());
}

// -----------------------------------------------------------------------------
// Verify that publishing without refresh ownership is rejected.
// -----------------------------------------------------------------------------
TEST_CASE("daemon upgrade state rejects publication without refresh")
{
  DaemonUpgradeState &state = reset_state();
  std::string error;
  std::vector<TransactionServiceUpgradeTarget> targets {
    make_target("demo", "x86_64", "2.0", "demo-0:2.0-1.fc44.x86_64"),
  };

  REQUIRE_FALSE(state.publish_success(1, targets, error));
  REQUIRE(error.find("no longer active") != std::string::npos);
  state.publish_failure(1, "old failure");

  DaemonUpgradeSnapshot snapshot = state.snapshot();
  REQUIRE(snapshot.status == DaemonUpgradeSnapshotStatus::NOT_LOADED);
  REQUIRE(snapshot.generation == 0);
  REQUIRE(snapshot.targets_by_name_arch.empty());
  REQUIRE(snapshot.error.empty());
}

// -----------------------------------------------------------------------------
// Verify that a discarded refresh owner abandons the active daemon refresh.
// -----------------------------------------------------------------------------
TEST_CASE("daemon upgrade refresh owner abandons discarded results")
{
  DaemonUpgradeState &state = reset_state();

  std::optional<DaemonUpgradeRefreshId> refresh_id = state.begin_refresh();
  REQUIRE(refresh_id.has_value());
  {
    DaemonUpgradeRefreshOwner owner(refresh_id.value());
  }

  DaemonUpgradeSnapshot snapshot = state.snapshot();
  REQUIRE(snapshot.status == DaemonUpgradeSnapshotStatus::NOT_LOADED);
  REQUIRE(state.begin_refresh().has_value());
}

// -----------------------------------------------------------------------------
// Verify that a published refresh owner does not abandon the ready snapshot.
// -----------------------------------------------------------------------------
TEST_CASE("daemon upgrade refresh owner keeps accepted results")
{
  DaemonUpgradeState &state = reset_state();
  std::string error;
  std::vector<TransactionServiceUpgradeTarget> targets {
    make_target("demo", "x86_64", "2.0", "demo-0:2.0-1.fc44.x86_64"),
  };

  std::optional<DaemonUpgradeRefreshId> refresh_id = state.begin_refresh();
  REQUIRE(refresh_id.has_value());
  {
    DaemonUpgradeRefreshOwner owner(refresh_id.value());
    REQUIRE(state.publish_success(owner.id(), targets, error));
    owner.close();
  }

  DaemonUpgradeSnapshot snapshot = state.snapshot();
  REQUIRE(snapshot.status == DaemonUpgradeSnapshotStatus::READY);
  REQUIRE(snapshot.generation == 1);
  REQUIRE(snapshot.targets_by_name_arch.size() == 1);
}

// -----------------------------------------------------------------------------
// EOF
// -----------------------------------------------------------------------------
