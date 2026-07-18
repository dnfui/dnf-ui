// -----------------------------------------------------------------------------
// test/unit/test_daemon_upgrade_state.cpp
// Shared daemon upgrade state tests
// -----------------------------------------------------------------------------
#include <catch2/catch_test_macros.hpp>

#include "upgrade/daemon_upgrade_state.hpp"

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

  REQUIRE(state.begin_refresh());
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

  REQUIRE(state.begin_refresh());
  REQUIRE(state.publish_success(targets, error));
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

  REQUIRE(state.begin_refresh());
  REQUIRE(state.publish_success({}, error));
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

  REQUIRE(state.begin_refresh());
  state.publish_failure("daemon failed");

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

  REQUIRE(state.begin_refresh());
  REQUIRE(state.publish_success(targets, error));
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

  REQUIRE(state.begin_refresh());
  REQUIRE(state.publish_success(targets, error));

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

  REQUIRE(state.begin_refresh());
  REQUIRE(state.publish_success(first_targets, error));
  REQUIRE(state.begin_refresh());
  REQUIRE_FALSE(state.publish_success(conflicting_targets, error));
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

  REQUIRE(state.begin_refresh());
  state.publish_failure("daemon failed");
  error = "old error";
  REQUIRE(state.begin_refresh());
  REQUIRE(state.publish_success(targets, error));
  REQUIRE(error.empty());
  REQUIRE(state.snapshot().status == DaemonUpgradeSnapshotStatus::READY);
  REQUIRE(state.snapshot().generation == 1);

  state.mark_stale();
  REQUIRE(state.begin_refresh());
  REQUIRE(state.publish_success(targets, error));
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

  REQUIRE(state.begin_refresh());
  state.mark_stale();
  REQUIRE_FALSE(state.publish_success(targets, error));
  REQUIRE(error.find("no longer active") != std::string::npos);
  state.publish_failure("old failure");

  DaemonUpgradeSnapshot snapshot = state.snapshot();
  REQUIRE(snapshot.status == DaemonUpgradeSnapshotStatus::STALE);
  REQUIRE(snapshot.generation == 0);
  REQUIRE(snapshot.targets_by_name_arch.empty());
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

  REQUIRE_FALSE(state.publish_success(targets, error));
  REQUIRE(error.find("no longer active") != std::string::npos);
  state.publish_failure("old failure");

  DaemonUpgradeSnapshot snapshot = state.snapshot();
  REQUIRE(snapshot.status == DaemonUpgradeSnapshotStatus::NOT_LOADED);
  REQUIRE(snapshot.generation == 0);
  REQUIRE(snapshot.targets_by_name_arch.empty());
  REQUIRE(snapshot.error.empty());
}

// -----------------------------------------------------------------------------
// EOF
// -----------------------------------------------------------------------------
