// -----------------------------------------------------------------------------
// package_table_upgrade_rows.hpp
// Package table rows built from daemon upgrade targets
// -----------------------------------------------------------------------------
#pragma once

#include "ui/package_table/package_table_view.hpp"
#include "upgrade/daemon_upgrade_state.hpp"

#include <vector>

// -----------------------------------------------------------------------------
// Build rows from an accepted daemon snapshot and optional exact-version metadata.
// The caller handles snapshot publication and checks that its status is READY.
// -----------------------------------------------------------------------------
std::vector<PackageTableRow> package_table_rows_from_daemon_targets(const DaemonUpgradeSnapshot &snapshot,
                                                                    const std::vector<PackageRow> &metadata_rows);

// -----------------------------------------------------------------------------
// EOF
// -----------------------------------------------------------------------------
