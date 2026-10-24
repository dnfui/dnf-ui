// -----------------------------------------------------------------------------
// test_package_table_upgrade_rows.cpp
// Daemon upgrade row construction tests without GTK or backend queries
// -----------------------------------------------------------------------------
#include <catch2/catch_test_macros.hpp>

#include "ui/package_table/package_table_upgrade_rows.hpp"

namespace {

DaemonUpgradeSnapshot
upgrade_snapshot()
{
  DaemonUpgradeTarget target {
    .name = "example",
    .arch = "x86_64",
    .epoch = "0",
    .version = "2.0",
    .release = "1.fc44",
    .nevra = "example-2.0-1.fc44.x86_64",
    .full_nevra = "example-0:2.0-1.fc44.x86_64",
    .repo_id = "daemon-updates",
  };

  DaemonUpgradeSnapshot snapshot;
  snapshot.generation = 7;
  snapshot.status = DaemonUpgradeSnapshotStatus::READY;
  snapshot.targets_by_name_arch.emplace(target.name_arch_key(), target);
  return snapshot;
}

PackageRow
upgrade_metadata()
{
  PackageRow row;
  row.name = "example";
  row.arch = "x86_64";
  row.epoch = "0";
  row.version = "2.0";
  row.release = "1.fc44";
  row.nevra = "example-2.0-1.fc44.x86_64";
  row.repo = "metadata-repo";
  row.summary = "Package summary from libdnf5";
  return row;
}

} // namespace

// -----------------------------------------------------------------------------
// Metadata alone must not create upgrades that the daemon did not report.
// -----------------------------------------------------------------------------
TEST_CASE("Upgrade rows stay empty without daemon targets", "[upgrade-rows]")
{
  DaemonUpgradeSnapshot snapshot;
  snapshot.status = DaemonUpgradeSnapshotStatus::READY;

  auto rows = package_table_rows_from_daemon_targets(snapshot, { upgrade_metadata() });

  REQUIRE(rows.empty());
}

// -----------------------------------------------------------------------------
// Missing metadata must leave a usable row with the daemon package identity.
// -----------------------------------------------------------------------------
TEST_CASE("Upgrade rows keep daemon targets when metadata is missing", "[upgrade-rows]")
{
  DaemonUpgradeSnapshot snapshot = upgrade_snapshot();
  const DaemonUpgradeTarget &target = snapshot.targets_by_name_arch.begin()->second;

  auto rows = package_table_rows_from_daemon_targets(snapshot, {});

  REQUIRE(rows.size() == 1);
  const PackageRow &row = rows.front().row;
  REQUIRE(row.nevra == target.nevra);
  REQUIRE(row.name == target.name);
  REQUIRE(row.epoch == target.epoch);
  REQUIRE(row.version == target.version);
  REQUIRE(row.release == target.release);
  REQUIRE(row.arch == target.arch);
  REQUIRE(row.repo == target.repo_id);
  REQUIRE(row.summary.empty());
  REQUIRE(rows.front().upgrade_target() != nullptr);
  REQUIRE(rows.front().upgrade_target()->full_nevra == target.full_nevra);
  REQUIRE(rows.front().upgrade_generation() == snapshot.generation);
}

// -----------------------------------------------------------------------------
// Display metadata must not replace the daemon target used by package actions.
// -----------------------------------------------------------------------------
TEST_CASE("Upgrade rows add exact metadata and retain their daemon context", "[upgrade-rows]")
{
  DaemonUpgradeSnapshot snapshot = upgrade_snapshot();
  DaemonUpgradeTarget target = snapshot.targets_by_name_arch.begin()->second;
  PackageRow metadata = upgrade_metadata();

  auto rows = package_table_rows_from_daemon_targets(snapshot, { metadata });
  snapshot.targets_by_name_arch.clear();
  snapshot.generation = 8;

  REQUIRE(rows.size() == 1);
  const PackageTableRow &row = rows.front();
  REQUIRE(row.row.nevra == metadata.nevra);
  REQUIRE(row.row.summary == metadata.summary);
  REQUIRE(row.row.repo == metadata.repo);
  REQUIRE(row.upgrade_generation() == 7);
  REQUIRE(row.upgrade_target() != nullptr);
  REQUIRE(row.upgrade_target()->name == target.name);
  REQUIRE(row.upgrade_target()->arch == target.arch);
  REQUIRE(row.upgrade_target()->epoch == target.epoch);
  REQUIRE(row.upgrade_target()->version == target.version);
  REQUIRE(row.upgrade_target()->release == target.release);
  REQUIRE(row.upgrade_target()->nevra == target.nevra);
  REQUIRE(row.upgrade_target()->full_nevra == target.full_nevra);
  REQUIRE(row.upgrade_target()->repo_id == target.repo_id);
}

// -----------------------------------------------------------------------------
// Unrelated metadata must neither add rows nor fill a daemon target with the wrong package.
// -----------------------------------------------------------------------------
TEST_CASE("Upgrade rows ignore unrelated package metadata", "[upgrade-rows]")
{
  DaemonUpgradeSnapshot snapshot = upgrade_snapshot();
  PackageRow metadata = upgrade_metadata();
  metadata.name = "unrelated";
  metadata.nevra = "unrelated-2.0-1.fc44.x86_64";

  auto rows = package_table_rows_from_daemon_targets(snapshot, { metadata });

  REQUIRE(rows.size() == 1);
  REQUIRE(rows.front().row.name == "example");
  REQUIRE(rows.front().row.repo == "daemon-updates");
  REQUIRE(rows.front().row.summary.empty());
}

// -----------------------------------------------------------------------------
// Matching package names are not enough to attach metadata for another exact build.
// -----------------------------------------------------------------------------
TEST_CASE("Upgrade rows require metadata for the exact daemon NEVRA", "[upgrade-rows]")
{
  DaemonUpgradeSnapshot snapshot = upgrade_snapshot();
  PackageRow metadata = upgrade_metadata();

  SECTION("different version")
  {
    metadata.version = "1.0";
    metadata.nevra = "example-1.0-1.fc44.x86_64";
  }
  SECTION("different release")
  {
    metadata.release = "2.fc44";
    metadata.nevra = "example-2.0-2.fc44.x86_64";
  }
  SECTION("different epoch")
  {
    metadata.epoch = "1";
    metadata.nevra = "example-1:2.0-1.fc44.x86_64";
  }
  SECTION("different architecture")
  {
    metadata.arch = "aarch64";
    metadata.nevra = "example-2.0-1.fc44.aarch64";
  }

  auto rows = package_table_rows_from_daemon_targets(snapshot, { metadata });

  REQUIRE(rows.size() == 1);
  REQUIRE(rows.front().row.nevra == "example-2.0-1.fc44.x86_64");
  REQUIRE(rows.front().row.repo == "daemon-updates");
  REQUIRE(rows.front().row.summary.empty());
}

// -----------------------------------------------------------------------------
// The full daemon NEVRA supplies both row identity and metadata lookup when needed.
// -----------------------------------------------------------------------------
TEST_CASE("Upgrade rows use full NEVRA when the normal NEVRA is missing", "[upgrade-rows]")
{
  DaemonUpgradeSnapshot snapshot = upgrade_snapshot();
  DaemonUpgradeTarget &target = snapshot.targets_by_name_arch.begin()->second;
  target.nevra.clear();
  std::vector<PackageRow> metadata_rows;

  SECTION("without metadata")
  {
  }
  SECTION("with matching metadata")
  {
    PackageRow metadata = upgrade_metadata();
    metadata.nevra = target.full_nevra;
    metadata_rows.push_back(metadata);
  }

  auto rows = package_table_rows_from_daemon_targets(snapshot, metadata_rows);

  REQUIRE(rows.size() == 1);
  REQUIRE(rows.front().row.nevra == target.full_nevra);
  REQUIRE(rows.front().upgrade_target() != nullptr);
  REQUIRE(rows.front().upgrade_target()->nevra.empty());
  REQUIRE(rows.front().upgrade_target()->full_nevra == target.full_nevra);
  REQUIRE(rows.front().upgrade_generation() == snapshot.generation);
  if (metadata_rows.empty()) {
    REQUIRE(rows.front().row.summary.empty());
  } else {
    REQUIRE(rows.front().row.summary == metadata_rows.front().summary);
  }
}

// -----------------------------------------------------------------------------
// EOF
// -----------------------------------------------------------------------------
