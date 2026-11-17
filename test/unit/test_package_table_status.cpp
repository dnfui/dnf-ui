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
// Compact package rows can display a resolved version different from the row.
// -----------------------------------------------------------------------------
TEST_CASE("Package table compact rows show additional resolved actions")
{
  reset_backend_globals();
  MainWindowUiState widgets;
  set_compact_package_view(widgets);
  PackageItem item;
  item.row = make_status_test_row("dependency-1-1.x86_64", "dependency", "1", "1", "x86_64");
  widgets.transaction_state.resolved_actions_by_nevra["dependency-2-1.x86_64"] = PendingAction::INSTALL;
  widgets.transaction_state.resolved_actions_by_name_arch[item.row.name_arch_key()] = PendingAction::INSTALL;

  package_table_fill_item_status(&widgets, item, dnf_backend_resolve_installed_package(item.row));
  REQUIRE(item.status_text == "Additional Install");
}

// -----------------------------------------------------------------------------
// An exact package list must never mark another available version.
// -----------------------------------------------------------------------------
TEST_CASE("Package table exact rows show only the resolved version")
{
  reset_backend_globals();
  MainWindowUiState widgets;
  set_all_version_package_view(widgets);
  widgets.transaction_state.resolved_actions_by_nevra["dependency-2-1.x86_64"] = PendingAction::INSTALL;
  widgets.transaction_state.resolved_actions_by_name_arch["dependency\nx86_64"] = PendingAction::INSTALL;

  PackageItem older;
  older.row = make_status_test_row("dependency-1-1.x86_64", "dependency", "1", "1", "x86_64");
  package_table_fill_item_status(&widgets, older, dnf_backend_resolve_installed_package(older.row));
  REQUIRE(older.status_text == "Available");

  PackageItem target;
  target.row = make_status_test_row("dependency-2-1.x86_64", "dependency", "2", "1", "x86_64");
  package_table_fill_item_status(&widgets, target, dnf_backend_resolve_installed_package(target.row));
  REQUIRE(target.status_text == "Additional Install");
}

// -----------------------------------------------------------------------------
// Show daemon-selected removals without adding them to explicit marks.
// -----------------------------------------------------------------------------
TEST_CASE("Package table shows additional resolved removals")
{
  reset_backend_globals();

  PackageRow installed = make_status_test_row("dependent-1-1.x86_64", "dependent", "1", "1", "x86_64");
  dnf_backend_testonly_replace_installed_snapshot_rows({ installed });

  MainWindowUiState widgets;
  set_installed_package_view(widgets);
  widgets.transaction_state.resolved_actions_by_nevra[installed.nevra] = PendingAction::REMOVE;

  PackageItem item;
  item.row = installed;
  package_table_fill_item_status(&widgets, item, dnf_backend_resolve_installed_package(item.row));

  REQUIRE(item.status_text == "Additional Removal");
  REQUIRE(widgets.transaction_state.actions.empty());
}

// -----------------------------------------------------------------------------
// An installed package row represents an unambiguous upgrade target.
// -----------------------------------------------------------------------------
TEST_CASE("Package table installed rows show additional resolved upgrades")
{
  reset_backend_globals();

  PackageRow installed = make_status_test_row("library-1-1.x86_64", "library", "1", "1", "x86_64");
  dnf_backend_testonly_replace_installed_snapshot_rows({ installed });

  MainWindowUiState widgets;
  set_installed_package_view(widgets);
  widgets.transaction_state.resolved_actions_by_nevra["library-2-1.x86_64"] = PendingAction::UPGRADE;
  widgets.transaction_state.resolved_actions_by_name_arch[installed.name_arch_key()] = PendingAction::UPGRADE;

  PackageItem item;
  item.row = installed;
  package_table_fill_item_status(&widgets, item, dnf_backend_resolve_installed_package(item.row));

  REQUIRE(item.status_text == "Additional Upgrade");
}

// -----------------------------------------------------------------------------
// A name and architecture fallback cannot identify one of several installed versions.
// -----------------------------------------------------------------------------
TEST_CASE("Package table does not project an additional upgrade across installed versions")
{
  reset_backend_globals();

  PackageRow old_version = make_status_test_row("library-1-1.x86_64", "library", "1", "1", "x86_64");
  PackageRow new_version = make_status_test_row("library-2-1.x86_64", "library", "2", "1", "x86_64");
  dnf_backend_testonly_replace_installed_snapshot_rows({ old_version, new_version });

  MainWindowUiState widgets;
  set_installed_package_view(widgets);
  widgets.transaction_state.resolved_actions_by_nevra["library-3-1.x86_64"] = PendingAction::UPGRADE;
  widgets.transaction_state.resolved_actions_by_name_arch[old_version.name_arch_key()] = PendingAction::UPGRADE;

  PackageItem old_item;
  old_item.row = old_version;
  package_table_fill_item_status(&widgets, old_item, dnf_backend_resolve_installed_package(old_item.row));
  REQUIRE(old_item.status_text != "Additional Upgrade");

  PackageItem new_item;
  new_item.row = new_version;
  package_table_fill_item_status(&widgets, new_item, dnf_backend_resolve_installed_package(new_item.row));
  REQUIRE(new_item.status_text != "Additional Upgrade");

  widgets.transaction_state.resolved_actions_by_nevra[old_version.nevra] = PendingAction::REMOVE;
  package_table_fill_item_status(&widgets, old_item, dnf_backend_resolve_installed_package(old_item.row));
  REQUIRE(old_item.status_text == "Additional Removal");
  package_table_fill_item_status(&widgets, new_item, dnf_backend_resolve_installed_package(new_item.row));
  REQUIRE(new_item.status_text != "Additional Removal");
}

// -----------------------------------------------------------------------------
// Compact rows cannot identify which installed version is being removed or reinstalled.
// -----------------------------------------------------------------------------
TEST_CASE("Package table compact rows do not borrow actions for another installed version")
{
  reset_backend_globals();

  PackageRow old_version = make_status_test_row("library-1-1.x86_64", "library", "1", "1", "x86_64");
  PackageRow new_version = make_status_test_row("library-2-1.x86_64", "library", "2", "1", "x86_64");
  dnf_backend_testonly_replace_installed_snapshot_rows({ old_version, new_version });

  for (const PendingAction::Type action_type : { PendingAction::REMOVE, PendingAction::REINSTALL,
                                                 PendingAction::UPGRADE, PendingAction::DOWNGRADE }) {
    MainWindowUiState widgets;
    set_compact_package_view(widgets);
    widgets.transaction_state.resolved_actions_by_name_arch[new_version.name_arch_key()] = action_type;

    PackageItem item;
    item.row = new_version;
    package_table_fill_item_status(&widgets, item, dnf_backend_resolve_installed_package(item.row));
    REQUIRE(item.status_text != package_table_resolved_action_status_text(action_type));
  }

  MainWindowUiState explicit_widgets;
  set_compact_package_view(explicit_widgets);
  explicit_widgets.transaction_state.actions = {
    { PendingAction::REMOVE, old_version.nevra, old_version.nevra, old_version.name_arch_key() },
  };
  explicit_widgets.transaction_state.resolved_actions_by_nevra[old_version.nevra] = PendingAction::REMOVE;
  explicit_widgets.transaction_state.resolved_actions_by_name_arch[old_version.name_arch_key()] = PendingAction::REMOVE;

  PackageItem visible;
  visible.row = new_version;
  package_table_fill_item_status(&explicit_widgets, visible, dnf_backend_resolve_installed_package(visible.row));
  REQUIRE(visible.status_text != "Additional Removal");
}

// -----------------------------------------------------------------------------
// -----------------------------------------------------------------------------
// Background status updates must not re-evaluate explicit marks or unrelated rows.
// -----------------------------------------------------------------------------
TEST_CASE("Package table updates only daemon-selected additional statuses")
{
  reset_backend_globals();

  MainWindowUiState widgets;
  set_all_version_package_view(widgets);

  PackageItem additional;
  additional.row = make_status_test_row("dependency-1-1.x86_64", "dependency", "1", "1", "x86_64");
  package_table_fill_item_status(&widgets, additional, dnf_backend_resolve_installed_package(additional.row));
  REQUIRE(additional.status_text == "Available");
  REQUIRE_FALSE(additional.status_is_explicit_pending);

  PackageItem unrelated;
  unrelated.row = make_status_test_row("unrelated-1-1.x86_64", "unrelated", "1", "1", "x86_64");
  package_table_fill_item_status(&widgets, unrelated, dnf_backend_resolve_installed_package(unrelated.row));
  REQUIRE(unrelated.status_text == "Available");

  widgets.transaction_state.resolved_actions_by_nevra[additional.row.nevra] = PendingAction::INSTALL;
  package_table_fill_additional_item_status(&widgets, additional);
  package_table_fill_additional_item_status(&widgets, unrelated);
  REQUIRE(additional.status_text == "Additional Install");
  REQUIRE(unrelated.status_text == "Available");

  widgets.transaction_state.actions.push_back(
      { PendingAction::INSTALL, additional.row.nevra, additional.row.nevra });
  package_table_fill_item_status(&widgets, additional, dnf_backend_resolve_installed_package(additional.row));
  REQUIRE(additional.status_text == "Pending Install");
  REQUIRE(additional.status_is_explicit_pending);

  package_table_fill_additional_item_status(&widgets, additional);
  REQUIRE(additional.status_text == "Pending Install");
}

// -----------------------------------------------------------------------------
// Multiple installed versions must not inherit an unrelated upgrade.
// -----------------------------------------------------------------------------
TEST_CASE("Package table installed installonly rows do not project resolved upgrades")
{
  reset_backend_globals();

  PackageRow installed = make_status_test_row("kernel-1-1.x86_64", "kernel", "1", "1", "x86_64");
  installed.installonly = true;
  dnf_backend_testonly_replace_installed_snapshot_rows({ installed });

  MainWindowUiState widgets;
  set_installed_package_view(widgets);
  widgets.transaction_state.resolved_actions_by_nevra["kernel-2-1.x86_64"] = PendingAction::UPGRADE;
  widgets.transaction_state.resolved_actions_by_name_arch[installed.name_arch_key()] = PendingAction::UPGRADE;

  PackageItem item;
  item.row = installed;
  package_table_fill_item_status(&widgets, item, dnf_backend_resolve_installed_package(item.row));

  REQUIRE(item.status_text != "Additional Upgrade");
}

// -----------------------------------------------------------------------------
// User marks take priority over markings from a previous daemon resolve.
// -----------------------------------------------------------------------------
TEST_CASE("Package table preserves explicitly marked actions")
{
  reset_backend_globals();
  MainWindowUiState widgets;
  PackageItem item;
  item.row = make_status_test_row("dependency-1-1.x86_64", "dependency", "1", "1", "x86_64");
  widgets.transaction_state.actions = {
    { PendingAction::INSTALL, item.row.nevra, item.row.nevra },
  };
  widgets.transaction_state.resolved_actions_by_nevra[item.row.nevra] = PendingAction::INSTALL;

  package_table_fill_item_status(&widgets, item, dnf_backend_resolve_installed_package(item.row));
  REQUIRE(item.status_text == "Pending Install");
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
  installed.repo_candidate_nevra = update.nevra;
  installed.repo_candidate_is_newest_available = true;
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
  installed.repo_candidate_nevra = update.nevra;
  installed.repo_candidate_is_newest_available = true;
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
  installed.repo_candidate_nevra = "demo-2.0-1.x86_64";
  installed.repo_candidate_is_newest_available = true;
  dnf_backend_testonly_replace_installed_snapshot_rows({ installed });

  MainWindowUiState widgets;
  set_compact_package_view(widgets);
  widgets.transaction_state.actions = {
    { PendingAction::UPGRADE, installed.repo_candidate_nevra, "demo.x86_64" },
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
  installed.repo_candidate_nevra = "demo-2.0-1.x86_64";
  installed.repo_candidate_is_newest_available = true;

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
  installed.repo_candidate_nevra = "demo-2.0-1.x86_64";
  installed.repo_candidate_is_newest_available = true;

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
  installed.repo_candidate_nevra = "demo-2.0-1.x86_64";
  installed.repo_candidate_is_newest_available = true;

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
  installed.repo_candidate_nevra = "demo-2.0-1.x86_64";
  installed.repo_candidate_is_newest_available = true;

  dnf_backend_testonly_replace_installed_snapshot_rows({ installed });

  MainWindowUiState widgets;
  set_installed_package_view(widgets);

  PackageItem item;
  item.row = installed;
  package_table_fill_item_status(&widgets, item, dnf_backend_resolve_installed_package(installed));
  REQUIRE(item.status_text == "Newer in repository");

  widgets.transaction_state.actions = {
    { PendingAction::UPGRADE, installed.repo_candidate_nevra, "demo.x86_64", installed.name_arch_key() },
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
// An installed row with a daemon target must use that exact target for cross-architecture upgrades.
// -----------------------------------------------------------------------------
TEST_CASE("Package table daemon upgrade target shows additional cross-architecture action")
{
  reset_backend_globals();

  PackageRow installed = make_status_test_row("demo-1-1.noarch", "demo", "1", "1", "noarch");
  dnf_backend_testonly_replace_installed_snapshot_rows({ installed });

  DaemonUpgradeTarget target;
  target.name = "demo";
  target.arch = "x86_64";
  target.version = "2";
  target.release = "1";
  target.nevra = "demo-2-1.x86_64";
  target.full_nevra = target.nevra;
  target.repo_id = "updates";

  MainWindowUiState widgets;
  widgets.query_state.displayed_query.kind = DisplayedPackageQueryKind::LIST_UPGRADEABLE;
  widgets.transaction_state.resolved_actions_by_nevra[target.nevra] = PendingAction::UPGRADE;

  PackageItem item;
  item.row = installed;
  item.daemon_upgrade = std::make_shared<DaemonUpgradeRowContext>(DaemonUpgradeRowContext {
      .target = target,
  });

  package_table_fill_item_status(&widgets, item, dnf_backend_resolve_installed_package(item.row));
  REQUIRE(item.status_text == "Additional Upgrade");
  REQUIRE(package_table_column_text(item, PackageColumnKind::STATUS) == "Additional Upgrade");
}

// -----------------------------------------------------------------------------
// A daemon target does not turn unrelated resolved installs into upgrades.
// -----------------------------------------------------------------------------
TEST_CASE("Package table daemon upgrade target does not borrow install actions")
{
  reset_backend_globals();

  DaemonUpgradeTarget target;
  target.name = "demo";
  target.arch = "x86_64";
  target.version = "2";
  target.release = "1";
  target.nevra = "demo-2-1.x86_64";
  target.full_nevra = target.nevra;

  MainWindowUiState widgets;
  widgets.query_state.displayed_query.kind = DisplayedPackageQueryKind::LIST_UPGRADEABLE;
  widgets.transaction_state.resolved_actions_by_nevra[target.nevra] = PendingAction::INSTALL;

  PackageItem item;
  item.row = make_status_test_row("demo-1-1.noarch", "demo", "1", "1", "noarch");
  item.daemon_upgrade = std::make_shared<DaemonUpgradeRowContext>(DaemonUpgradeRowContext {
      .target = target,
  });

  package_table_fill_item_status(&widgets, item, dnf_backend_resolve_installed_package(item.row));
  REQUIRE(item.status_text != "Additional Install");
}

// -----------------------------------------------------------------------------
// EOF
// -----------------------------------------------------------------------------
