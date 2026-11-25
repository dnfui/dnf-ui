#include <catch2/catch_test_macros.hpp>

#include "test_utils.hpp"
#include "ui/common/widgets.hpp"
#include "ui/package_table/package_table_status.hpp"
#include "ui/package_table/package_table_view_internal.hpp"

#include <memory>

static PackageRow
make_status_test_row(const std::string &nevra,
                     const std::string &name,
                     const std::string &version,
                     const std::string &release,
                     const std::string &arch)
{
  PackageRow row;
  row.nevra = nevra;
  row.name = name;
  row.version = version;
  row.release = release;
  row.arch = arch;
  row.repo = "fedora";
  row.summary = "Test package";
  return row;
}

static void
require_stored_status_matches_pending_action(MainWindowUiState &widgets, PackageItem &item, const char *expected_text)
{
  PackageTableRow table_row {
    .row = item.row,
    .daemon_upgrade = item.daemon_upgrade,
  };

  const bool exact_installonly_actions =
      displayed_package_query_uses_exact_installonly_actions(widgets.query_state.displayed_query);
  PendingTransactionActionRows action_rows =
      pending_transaction_action_rows_for_selection_with_pending(table_row.row,
                                                                 table_row.upgrade_target(),
                                                                 table_row.upgrade_generation(),
                                                                 exact_installonly_actions,
                                                                 widgets.transaction_state.actions);
  PendingAction::Type action_type;
  REQUIRE(package_table_pending_action_for_resolved_row(&widgets, table_row, action_rows, action_type));

  InstalledPackageResolution resolution = dnf_backend_resolve_installed_package(item.row);
  PackageInstallState install_state = item.upgrade_target() ? PackageInstallState::UPGRADEABLE : resolution.state;
  REQUIRE(std::string(package_table_pending_action_status_text(action_type, install_state)) == expected_text);

  package_table_fill_item_status(&widgets, item, resolution);
  REQUIRE(item.status_text == expected_text);
  REQUIRE(package_table_column_text(item, PackageColumnKind::STATUS) == expected_text);
}

// -----------------------------------------------------------------------------
// Mark the test table as a compact List Packages view.
// -----------------------------------------------------------------------------
static void
set_compact_package_view(MainWindowUiState &widgets)
{
  widgets.query_state.displayed_query.kind = DisplayedPackageQueryKind::LIST_PACKAGES;
  widgets.query_state.displayed_query.latest_only = true;
}

// -----------------------------------------------------------------------------
// Mark the test table as an all-version List Packages view.
// -----------------------------------------------------------------------------
static void
set_all_version_package_view(MainWindowUiState &widgets)
{
  widgets.query_state.displayed_query.kind = DisplayedPackageQueryKind::LIST_PACKAGES;
  widgets.query_state.displayed_query.latest_only = false;
}

// -----------------------------------------------------------------------------
// Mark the test table as a List Installed view.
// -----------------------------------------------------------------------------
static void
set_installed_package_view(MainWindowUiState &widgets)
{
  widgets.query_state.displayed_query.kind = DisplayedPackageQueryKind::LIST_INSTALLED;
  widgets.query_state.displayed_query.latest_only = false;
}

// -----------------------------------------------------------------------------
// Verify that ordinary rows without pending actions keep their resolved Status text and sort rank.
// -----------------------------------------------------------------------------
TEST_CASE("Package table non-pending status matches resolved action rows")
{
  reset_backend_globals();

  MainWindowUiState widgets;
  set_all_version_package_view(widgets);

  PackageItem item;
  item.row = make_status_test_row("demo-1.0-1.x86_64", "demo", "1.0", "1", "x86_64");
  PackageTableRow table_row {
    .row = item.row,
    .daemon_upgrade = {},
  };

  for (PackageInstallState state : { PackageInstallState::AVAILABLE,
                                     PackageInstallState::INSTALLED,
                                     PackageInstallState::LOCAL_ONLY,
                                     PackageInstallState::INSTALLED_NEWER_THAN_REPO,
                                     PackageInstallState::DOWNGRADEABLE }) {
    InstalledPackageResolution resolution;
    resolution.state = state;
    PendingTransactionActionRows action_rows =
        pending_transaction_action_rows_for_resolved_selection(item.row, nullptr, 0, resolution, false);

    package_table_fill_item_status(&widgets, item, resolution);

    REQUIRE(item.status_text == package_table_status_text_for_resolved_row(table_row, action_rows, true));
    REQUIRE(item.status_rank == dnf_backend_get_install_state_sort_rank(state));
  }
}

// -----------------------------------------------------------------------------
// Verify that status calculation also works without a window state.
// -----------------------------------------------------------------------------
TEST_CASE("Package table non-pending status works without window state")
{
  PackageItem item;
  InstalledPackageResolution resolution;
  resolution.state = PackageInstallState::AVAILABLE;

  package_table_fill_item_status(nullptr, item, resolution);

  REQUIRE(item.status_text == "Available");
  REQUIRE(item.status_rank == dnf_backend_get_install_state_sort_rank(PackageInstallState::AVAILABLE));
}

// -----------------------------------------------------------------------------
// Verify that pending installs use the same Status text in stored and visible paths.
// -----------------------------------------------------------------------------
TEST_CASE("Package table pending install status is shared")
{
  reset_backend_globals();

  MainWindowUiState widgets;
  PackageItem item;
  item.row = make_status_test_row("demo-1.0-1.x86_64", "demo", "1.0", "1", "x86_64");
  widgets.transaction_state.actions = {
    { PendingAction::INSTALL, item.row.nevra, item.row.nevra },
  };

  require_stored_status_matches_pending_action(widgets, item, "Pending Install");
}

// -----------------------------------------------------------------------------
// Verify that an exact install action does not borrow upgrade wording from the row state.
// -----------------------------------------------------------------------------
TEST_CASE("Package table installonly update action shows pending install status")
{
  reset_backend_globals();

  PackageRow installed =
      make_status_test_row("installonly-demo-1.0-1.x86_64", "installonly-demo", "1.0", "1", "x86_64");
  PackageRow update = make_status_test_row("installonly-demo-2.0-1.x86_64", "installonly-demo", "2.0", "1", "x86_64");
  update.installonly = true;
  update.is_newest_available = true;
  dnf_backend_testonly_replace_installed_snapshot_rows({ installed });

  MainWindowUiState widgets;
  set_all_version_package_view(widgets);

  PackageItem item;
  item.row = update;

  PendingTransactionActionRows action_rows = pending_transaction_action_rows_for_selection(item.row, nullptr, 0, true);
  REQUIRE(pending_transaction_mark_install_side_action(widgets.transaction_state.actions, action_rows));

  InstalledPackageResolution resolution = dnf_backend_resolve_installed_package(item.row);
  REQUIRE(resolution.state == PackageInstallState::UPGRADEABLE);

  package_table_fill_item_status(&widgets, item, resolution);
  REQUIRE(item.status_text == "Pending Install");
  REQUIRE(package_table_column_text(item, PackageColumnKind::STATUS) == "Pending Install");
}

// -----------------------------------------------------------------------------
// Verify that compact rows keep pending exact installonly install status.
// -----------------------------------------------------------------------------
TEST_CASE("Package table compact installonly row preserves pending exact install status")
{
  reset_backend_globals();

  PackageRow installed =
      make_status_test_row("installonly-demo-1.0-1.x86_64", "installonly-demo", "1.0", "1", "x86_64");
  PackageRow update = make_status_test_row("installonly-demo-2.0-1.x86_64", "installonly-demo", "2.0", "1", "x86_64");
  installed.installonly = true;
  installed.repo_candidate_relation = PackageRepoCandidateRelation::NEWER;
  installed.set_repo_candidate({ .nevra = update.nevra, .is_newest_available = true });
  update.installonly = true;
  update.is_newest_available = true;
  dnf_backend_testonly_replace_installed_snapshot_rows({ installed });

  MainWindowUiState widgets;
  set_all_version_package_view(widgets);

  PendingTransactionActionRows exact_rows = pending_transaction_action_rows_for_selection(update, nullptr, 0, true);
  REQUIRE(pending_transaction_mark_install_side_action(widgets.transaction_state.actions, exact_rows));

  set_compact_package_view(widgets);
  PackageItem item;
  item.row = installed;

  InstalledPackageResolution resolution = dnf_backend_resolve_installed_package(item.row);
  package_table_fill_item_status(&widgets, item, resolution);

  REQUIRE(item.status_text == "Pending Install");
  REQUIRE(package_table_column_text(item, PackageColumnKind::STATUS) == "Pending Install");
}

// -----------------------------------------------------------------------------
// Verify that exact rows keep pending compact installonly upgrade status.
// -----------------------------------------------------------------------------
TEST_CASE("Package table exact installonly row preserves pending upgrade status")
{
  reset_backend_globals();

  PackageRow installed =
      make_status_test_row("installonly-demo-1.0-1.x86_64", "installonly-demo", "1.0", "1", "x86_64");
  PackageRow update = make_status_test_row("installonly-demo-2.0-1.x86_64", "installonly-demo", "2.0", "1", "x86_64");
  installed.installonly = true;
  installed.repo_candidate_relation = PackageRepoCandidateRelation::NEWER;
  installed.set_repo_candidate({ .nevra = update.nevra, .is_newest_available = true });
  update.installonly = true;
  update.is_newest_available = true;
  dnf_backend_testonly_replace_installed_snapshot_rows({ installed });

  MainWindowUiState widgets;
  set_compact_package_view(widgets);

  PendingTransactionActionRows compact_rows =
      pending_transaction_action_rows_for_selection(installed, nullptr, 0, false);
  REQUIRE(pending_transaction_mark_install_side_action(widgets.transaction_state.actions, compact_rows));

  set_all_version_package_view(widgets);
  PackageItem item;
  item.row = update;

  InstalledPackageResolution resolution = dnf_backend_resolve_installed_package(item.row);
  package_table_fill_item_status(&widgets, item, resolution);

  REQUIRE(item.status_text == "Pending Upgrade");
  REQUIRE(package_table_column_text(item, PackageColumnKind::STATUS) == "Pending Upgrade");
}

// -----------------------------------------------------------------------------
// Verify that downgradeable rows have their own Status text.
// -----------------------------------------------------------------------------
TEST_CASE("Package table downgradeable status is shared")
{
  reset_backend_globals();

  PackageRow installed = make_status_test_row("demo-2.0-1.x86_64", "demo", "2.0", "1", "x86_64");
  PackageRow older = make_status_test_row("demo-1.0-1.x86_64", "demo", "1.0", "1", "x86_64");
  dnf_backend_testonly_replace_installed_snapshot_rows({ installed });

  PackageItem item;
  item.row = older;

  MainWindowUiState widgets;
  InstalledPackageResolution resolution = dnf_backend_resolve_installed_package(item.row);
  package_table_fill_item_status(&widgets, item, resolution);

  REQUIRE(item.status_text == "Older in repository");
  REQUIRE(package_table_column_text(item, PackageColumnKind::STATUS) == "Older in repository");
}

// -----------------------------------------------------------------------------
// Verify that an installed row can show a pending upgrade queued for its install row.
// -----------------------------------------------------------------------------
TEST_CASE("Package table pending upgrade status uses resolved install row")
{
  reset_backend_globals();

  PackageRow installed = make_status_test_row("demo-1.0-1.x86_64", "demo", "1.0", "1", "x86_64");
  installed.repo_candidate_relation = PackageRepoCandidateRelation::NEWER;
  installed.set_repo_candidate({ .nevra = "demo-2.0-1.x86_64", .is_newest_available = true });
  dnf_backend_testonly_replace_installed_snapshot_rows({ installed });

  MainWindowUiState widgets;
  set_compact_package_view(widgets);
  widgets.transaction_state.actions = {
    { PendingAction::UPGRADE, installed.repo_candidate->nevra, "demo.x86_64" },
  };

  PackageItem item;
  item.row = installed;

  require_stored_status_matches_pending_action(widgets, item, "Pending Upgrade");
}

// -----------------------------------------------------------------------------
// Verify that all-version rows show pending upgrade only on the exact target row.
// -----------------------------------------------------------------------------
TEST_CASE("Package table all-version pending upgrade status uses exact row")
{
  reset_backend_globals();

  PackageRow installed = make_status_test_row("demo-1.0-1.x86_64", "demo", "1.0", "1", "x86_64");
  installed.repo_candidate_relation = PackageRepoCandidateRelation::NEWER;
  installed.set_repo_candidate({ .nevra = "demo-2.0-1.x86_64", .is_newest_available = true });

  PackageRow update = make_status_test_row("demo-2.0-1.x86_64", "demo", "2.0", "1", "x86_64");
  update.is_newest_available = true;

  dnf_backend_testonly_replace_installed_snapshot_rows({ installed });

  MainWindowUiState widgets;
  set_all_version_package_view(widgets);
  widgets.transaction_state.actions = {
    { PendingAction::UPGRADE, update.nevra, "demo.x86_64", update.name_arch_key() },
  };

  PackageTableRow installed_table_row {
    .row = installed,
    .daemon_upgrade = {},
  };
  PendingTransactionActionRows installed_action_rows =
      pending_transaction_action_rows_for_selection_with_pending(installed_table_row.row,
                                                                 installed_table_row.upgrade_target(),
                                                                 installed_table_row.upgrade_generation(),
                                                                 false,
                                                                 widgets.transaction_state.actions);

  PendingAction::Type action_type;
  REQUIRE_FALSE(
      package_table_pending_action_for_resolved_row(&widgets, installed_table_row, installed_action_rows, action_type));

  PackageItem installed_item;
  installed_item.row = installed;
  package_table_fill_item_status(&widgets, installed_item, dnf_backend_resolve_installed_package(installed));
  REQUIRE(installed_item.status_text == "Installed, update available");

  PackageItem update_item;
  update_item.row = update;
  require_stored_status_matches_pending_action(widgets, update_item, "Pending Upgrade");
}

// -----------------------------------------------------------------------------
// Verify that a pending upgrade does not leak onto an older available row.
// -----------------------------------------------------------------------------
TEST_CASE("Package table pending upgrade does not match downgrade row")
{
  reset_backend_globals();

  PackageRow installed = make_status_test_row("demo-2.0-1.x86_64", "demo", "2.0", "1", "x86_64");
  PackageRow older = make_status_test_row("demo-1.0-1.x86_64", "demo", "1.0", "1", "x86_64");
  dnf_backend_testonly_replace_installed_snapshot_rows({ installed });

  MainWindowUiState widgets;
  set_all_version_package_view(widgets);
  widgets.transaction_state.actions = {
    { PendingAction::UPGRADE, installed.nevra, "demo.x86_64", installed.name_arch_key() },
  };

  PackageTableRow older_table_row {
    .row = older,
    .daemon_upgrade = {},
  };
  PendingTransactionActionRows older_action_rows = pending_transaction_action_rows_for_selection(
      older_table_row.row, older_table_row.upgrade_target(), older_table_row.upgrade_generation(), false);

  REQUIRE(older_action_rows.install_is_downgrade);

  PendingAction::Type action_type;
  REQUIRE_FALSE(
      package_table_pending_action_for_resolved_row(&widgets, older_table_row, older_action_rows, action_type));
}

// -----------------------------------------------------------------------------
// Verify that all-version update rows have distinct non-pending Status labels.
// -----------------------------------------------------------------------------
TEST_CASE("Package table all-version upgrade status separates installed and available rows")
{
  reset_backend_globals();

  PackageRow installed = make_status_test_row("demo-1.0-1.x86_64", "demo", "1.0", "1", "x86_64");
  installed.repo_candidate_relation = PackageRepoCandidateRelation::NEWER;
  installed.set_repo_candidate({ .nevra = "demo-2.0-1.x86_64", .is_newest_available = true });

  PackageRow update = make_status_test_row("demo-2.0-1.x86_64", "demo", "2.0", "1", "x86_64");
  update.is_newest_available = true;

  dnf_backend_testonly_replace_installed_snapshot_rows({ installed });

  MainWindowUiState widgets;
  set_all_version_package_view(widgets);

  PackageItem installed_item;
  installed_item.row = installed;
  package_table_fill_item_status(&widgets, installed_item, dnf_backend_resolve_installed_package(installed));

  PackageItem update_item;
  update_item.row = update;
  package_table_fill_item_status(&widgets, update_item, dnf_backend_resolve_installed_package(update));

  REQUIRE(installed_item.status_text == "Installed, update available");
  REQUIRE(update_item.status_text == "Available update");
}

// -----------------------------------------------------------------------------
// Verify that compact update rows keep the single stream Status label.
// -----------------------------------------------------------------------------
TEST_CASE("Package table compact upgrade status keeps stream label")
{
  reset_backend_globals();

  PackageRow installed = make_status_test_row("demo-1.0-1.x86_64", "demo", "1.0", "1", "x86_64");
  installed.repo_candidate_relation = PackageRepoCandidateRelation::NEWER;
  installed.set_repo_candidate({ .nevra = "demo-2.0-1.x86_64", .is_newest_available = true });

  dnf_backend_testonly_replace_installed_snapshot_rows({ installed });

  MainWindowUiState widgets;
  set_compact_package_view(widgets);

  PackageItem item;
  item.row = installed;
  package_table_fill_item_status(&widgets, item, dnf_backend_resolve_installed_package(installed));

  REQUIRE(item.status_text == "Newer in repository");
}

// -----------------------------------------------------------------------------
// Verify that List Installed rows keep upgrade status and pending upgrade handling.
// -----------------------------------------------------------------------------
TEST_CASE("Package table list installed upgrade status uses installed row")
{
  reset_backend_globals();

  PackageRow installed = make_status_test_row("demo-1.0-1.x86_64", "demo", "1.0", "1", "x86_64");
  installed.repo_candidate_relation = PackageRepoCandidateRelation::NEWER;
  installed.set_repo_candidate({ .nevra = "demo-2.0-1.x86_64", .is_newest_available = true });

  dnf_backend_testonly_replace_installed_snapshot_rows({ installed });

  MainWindowUiState widgets;
  set_installed_package_view(widgets);

  PackageItem item;
  item.row = installed;
  package_table_fill_item_status(&widgets, item, dnf_backend_resolve_installed_package(installed));
  REQUIRE(item.status_text == "Newer in repository");

  widgets.transaction_state.actions = {
    { PendingAction::UPGRADE, installed.repo_candidate->nevra, "demo.x86_64", installed.name_arch_key() },
  };

  require_stored_status_matches_pending_action(widgets, item, "Pending Upgrade");
}

// -----------------------------------------------------------------------------
// Verify that compact update rows show pending removal queued for the installed row.
// -----------------------------------------------------------------------------
TEST_CASE("Package table compact update row shows pending removal")
{
  reset_backend_globals();

  PackageRow installed = make_status_test_row("demo-1.0-1.x86_64", "demo", "1.0", "1", "x86_64");
  PackageRow update = make_status_test_row("demo-2.0-1.x86_64", "demo", "2.0", "1", "x86_64");
  dnf_backend_testonly_replace_installed_snapshot_rows({ installed });

  MainWindowUiState widgets;
  set_compact_package_view(widgets);
  widgets.transaction_state.actions = {
    { PendingAction::REMOVE, installed.nevra, installed.nevra },
  };

  PackageItem item;
  item.row = update;

  require_stored_status_matches_pending_action(widgets, item, "Pending Removal");
  item.row = installed;
  require_stored_status_matches_pending_action(widgets, item, "Pending Removal");
}

// -----------------------------------------------------------------------------
// Verify that compact update rows show pending reinstall queued for the installed row.
// -----------------------------------------------------------------------------
TEST_CASE("Package table compact update row shows pending reinstall")
{
  reset_backend_globals();

  PackageRow installed = make_status_test_row("demo-1.0-1.x86_64", "demo", "1.0", "1", "x86_64");
  PackageRow update = make_status_test_row("demo-2.0-1.x86_64", "demo", "2.0", "1", "x86_64");
  dnf_backend_testonly_replace_installed_snapshot_rows({ installed });

  MainWindowUiState widgets;
  set_compact_package_view(widgets);
  widgets.transaction_state.actions = {
    { PendingAction::REINSTALL, installed.nevra, installed.nevra },
  };

  PackageItem item;
  item.row = update;

  require_stored_status_matches_pending_action(widgets, item, "Pending Reinstall");
  item.row = installed;
  require_stored_status_matches_pending_action(widgets, item, "Pending Reinstall");
}

// -----------------------------------------------------------------------------
// Verify that pending status does not leak between two exact installed EVRs.
// -----------------------------------------------------------------------------
TEST_CASE("Package table pending status keeps exact installed NEVRAs separate")
{
  reset_backend_globals();

  PackageRow older = make_status_test_row("demo-1.0-1.x86_64", "demo", "1.0", "1", "x86_64");
  PackageRow newer = make_status_test_row("demo-2.0-1.x86_64", "demo", "2.0", "1", "x86_64");
  dnf_backend_testonly_replace_installed_snapshot_rows({ older, newer });

  MainWindowUiState widgets;
  widgets.transaction_state.actions = {
    { PendingAction::REMOVE, newer.nevra, newer.nevra },
  };

  PackageTableRow table_row {
    .row = older,
    .daemon_upgrade = {},
  };

  PendingTransactionActionRows action_rows = pending_transaction_action_rows_for_selection(
      table_row.row, table_row.upgrade_target(), table_row.upgrade_generation(), false);
  PendingAction::Type action_type;
  REQUIRE_FALSE(package_table_pending_action_for_resolved_row(&widgets, table_row, action_rows, action_type));
}

// -----------------------------------------------------------------------------
// Verify that removal status does not leak onto all-version repository rows.
// -----------------------------------------------------------------------------
TEST_CASE("Package table pending removal status does not leak to exact available versions")
{
  reset_backend_globals();

  PackageRow installed = make_status_test_row("demo-3.0-1.x86_64", "demo", "3.0", "1", "x86_64");
  PackageRow older = make_status_test_row("demo-1.0-1.x86_64", "demo", "1.0", "1", "x86_64");
  PackageRow intermediate = make_status_test_row("demo-2.0-1.x86_64", "demo", "2.0", "1", "x86_64");
  dnf_backend_testonly_replace_installed_snapshot_rows({ installed });

  MainWindowUiState widgets;
  set_all_version_package_view(widgets);
  widgets.transaction_state.actions = {
    { PendingAction::REMOVE, installed.nevra, installed.nevra, installed.name_arch_key() },
  };

  for (const auto &row : { older, intermediate }) {
    PackageTableRow table_row {
      .row = row,
      .daemon_upgrade = {},
    };

    PendingTransactionActionRows action_rows = pending_transaction_action_rows_for_selection(
        table_row.row, table_row.upgrade_target(), table_row.upgrade_generation(), false);
    PendingAction::Type action_type;
    REQUIRE_FALSE(package_table_pending_action_for_resolved_row(&widgets, table_row, action_rows, action_type));
  }
}

// -----------------------------------------------------------------------------
// Verify that reinstall status does not leak onto all-version repository rows.
// -----------------------------------------------------------------------------
TEST_CASE("Package table pending reinstall status does not leak to exact available versions")
{
  reset_backend_globals();

  PackageRow installed = make_status_test_row("demo-3.0-1.x86_64", "demo", "3.0", "1", "x86_64");
  PackageRow older = make_status_test_row("demo-1.0-1.x86_64", "demo", "1.0", "1", "x86_64");
  PackageRow intermediate = make_status_test_row("demo-2.0-1.x86_64", "demo", "2.0", "1", "x86_64");
  dnf_backend_testonly_replace_installed_snapshot_rows({ installed });

  MainWindowUiState widgets;
  set_all_version_package_view(widgets);
  widgets.transaction_state.actions = {
    { PendingAction::REINSTALL, installed.nevra, installed.nevra, installed.name_arch_key() },
  };

  for (const auto &row : { older, intermediate }) {
    PackageTableRow table_row {
      .row = row,
      .daemon_upgrade = {},
    };

    PendingTransactionActionRows action_rows = pending_transaction_action_rows_for_selection(
        table_row.row, table_row.upgrade_target(), table_row.upgrade_generation(), false);
    PendingAction::Type action_type;
    REQUIRE_FALSE(package_table_pending_action_for_resolved_row(&widgets, table_row, action_rows, action_type));
  }
}

// -----------------------------------------------------------------------------
// Verify that daemon upgrade rows use the same Status text for stored table data.
// -----------------------------------------------------------------------------
TEST_CASE("Package table daemon upgrade pending status is shared")
{
  reset_backend_globals();

  DaemonUpgradeTarget target;
  target.name = "demo";
  target.arch = "x86_64";
  target.version = "2.0";
  target.release = "1";
  target.nevra = "demo-2.0-1.x86_64";
  target.full_nevra = target.nevra;
  target.repo_id = "updates";

  MainWindowUiState widgets;
  widgets.transaction_state.actions = {
    { PendingAction::UPGRADE, target.nevra, target.upgrade_spec() },
  };

  PackageItem item;
  item.row = make_status_test_row("demo-2.0-1.x86_64", "demo", "2.0", "1", "x86_64");
  item.daemon_upgrade = std::make_shared<DaemonUpgradeRowContext>(DaemonUpgradeRowContext {
      .target = target,
  });

  require_stored_status_matches_pending_action(widgets, item, "Pending Upgrade");
}

// -----------------------------------------------------------------------------
// EOF
// -----------------------------------------------------------------------------
