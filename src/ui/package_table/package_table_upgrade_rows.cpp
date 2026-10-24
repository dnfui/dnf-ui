// -----------------------------------------------------------------------------
// package_table_upgrade_rows.cpp
// Package table rows built from daemon upgrade targets
// -----------------------------------------------------------------------------
#include "ui/package_table/package_table_upgrade_rows.hpp"

#include <map>
#include <memory>

// -----------------------------------------------------------------------------
// Build a basic package row from one daemon upgrade target.
// libdnf5 may add display metadata later. dnf5daemon decides whether this row
// appears in List Upgradable.
// -----------------------------------------------------------------------------
static PackageRow
package_row_from_daemon_upgrade_target(const DaemonUpgradeTarget &target)
{
  PackageRow row;
  row.nevra = target.nevra.empty() ? target.full_nevra : target.nevra;
  row.name = target.name;
  row.epoch = target.epoch;
  row.version = target.version;
  row.release = target.release;
  row.arch = target.arch;
  row.repo = target.repo_id;
  return row;
}

// -----------------------------------------------------------------------------
// Build table rows from one complete daemon upgrade snapshot and optional metadata.
// Missing metadata does not hide daemon-reported upgrades.
// -----------------------------------------------------------------------------
std::vector<PackageTableRow>
package_table_rows_from_daemon_targets(const DaemonUpgradeSnapshot &snapshot,
                                       const std::vector<PackageRow> &metadata_rows)
{
  std::map<std::string, PackageRow> metadata_by_nevra;
  for (const auto &row : metadata_rows) {
    metadata_by_nevra.emplace(row.nevra, row);
  }

  std::vector<PackageTableRow> rows;
  rows.reserve(snapshot.targets_by_name_arch.size());
  for (const auto &[key, target] : snapshot.targets_by_name_arch) {
    PackageRow row = package_row_from_daemon_upgrade_target(target);
    auto metadata = metadata_by_nevra.find(row.nevra);
    if (metadata != metadata_by_nevra.end()) {
      row = metadata->second;
    }

    rows.push_back({
        .row = row,
        .daemon_upgrade = std::make_shared<DaemonUpgradeRowContext>(DaemonUpgradeRowContext {
            .target = target,
            .upgrade_generation = snapshot.generation,
        }),
    });
  }

  return rows;
}

// -----------------------------------------------------------------------------
// EOF
// -----------------------------------------------------------------------------
