// -----------------------------------------------------------------------------
// Pending transaction action row resolver tests
// Covers the package IDs used by pending install, upgrade, remove, and reinstall actions.
// -----------------------------------------------------------------------------
#include <catch2/catch_test_macros.hpp>

#include "test_utils.hpp"
#include "dnf_backend/dnf_internal.hpp"
#include "upgrade/daemon_upgrade_state.hpp"
#include "ui/package_query/package_query_state.hpp"
#include "ui/transaction/pending_transaction_action_rows.hpp"
#include "ui/transaction/pending_transaction_request.hpp"

#include <optional>
#include <set>
#include <vector>

// -----------------------------------------------------------------------------
// Build one small package row for resolver tests.
// -----------------------------------------------------------------------------
static PackageRow
make_test_package_row(const char *nevra, const char *name, const char *version, const char *release, const char *arch)
{
  PackageRow row;
  row.nevra = nevra;
  row.name = name;
  row.version = version;
  row.release = release;
  row.arch = arch;
  return row;
}

// -----------------------------------------------------------------------------
// Build one installed-query result for snapshot publication tests.
// -----------------------------------------------------------------------------
static dnf_backend_internal::InstalledQueryResult
make_single_installed_query_result(const PackageRow &row)
{
  dnf_backend_internal::InstalledQueryResult installed;
  installed.rows = { row };
  installed.nevras = { row.nevra };
  installed.rows_by_name_arch.emplace(row.name_arch_key(), row);
  return installed;
}

// -----------------------------------------------------------------------------
// Build one daemon upgrade target for resolver tests.
// -----------------------------------------------------------------------------
static DaemonUpgradeTarget
make_test_upgrade_target(const char *nevra,
                         const char *name,
                         const char *version,
                         const char *release,
                         const char *arch)
{
  DaemonUpgradeTarget target;
  target.name = name;
  target.arch = arch;
  target.version = version;
  target.release = release;
  target.nevra = nevra;
  target.full_nevra = nevra;
  target.repo_id = "updates";
  return target;
}

// -----------------------------------------------------------------------------
// Verify the view rules used by upgrade action projection.
// -----------------------------------------------------------------------------
TEST_CASE("Displayed package query state separates compact rows from upgrade projection")
{
  DisplayedPackageQueryState displayed;

  displayed.kind = DisplayedPackageQueryKind::LIST_INSTALLED;
  displayed.latest_only = false;
  REQUIRE_FALSE(displayed_package_query_uses_compact_rows(displayed));
  REQUIRE(displayed_package_query_projects_upgrade_actions(displayed));
  REQUIRE(displayed_package_query_allows_installed_upgrade_action(displayed));
  REQUIRE_FALSE(displayed_package_query_uses_exact_available_rows(displayed));

  displayed.kind = DisplayedPackageQueryKind::LIST_UNNEEDED;
  REQUIRE_FALSE(displayed_package_query_uses_compact_rows(displayed));
  REQUIRE(displayed_package_query_projects_upgrade_actions(displayed));
  REQUIRE(displayed_package_query_allows_installed_upgrade_action(displayed));
  REQUIRE_FALSE(displayed_package_query_uses_exact_available_rows(displayed));

  displayed.kind = DisplayedPackageQueryKind::LIST_PACKAGES;
  displayed.latest_only = false;
  REQUIRE_FALSE(displayed_package_query_uses_compact_rows(displayed));
  REQUIRE_FALSE(displayed_package_query_projects_upgrade_actions(displayed));
  REQUIRE(displayed_package_query_allows_installed_upgrade_action(displayed));
  REQUIRE(displayed_package_query_uses_exact_available_rows(displayed));

  displayed.kind = DisplayedPackageQueryKind::LIST_PACKAGES;
  displayed.latest_only = true;
  REQUIRE(displayed_package_query_uses_compact_rows(displayed));
  REQUIRE(displayed_package_query_projects_upgrade_actions(displayed));
  REQUIRE(displayed_package_query_allows_installed_upgrade_action(displayed));
  REQUIRE_FALSE(displayed_package_query_uses_exact_available_rows(displayed));
}

// -----------------------------------------------------------------------------
// Verify that an available row with no installed match can only be installed.
// -----------------------------------------------------------------------------
TEST_CASE("Pending transaction action rows resolve plain available package")
{
  reset_backend_globals();

  PackageRow available = make_test_package_row("demo-1.0-1.x86_64", "demo", "1.0", "1", "x86_64");

  PendingTransactionActionRows rows = pending_transaction_action_rows_for_selection(available, nullptr, 0, false);

  REQUIRE(rows.state == PackageInstallState::AVAILABLE);
  REQUIRE_FALSE(rows.install_is_upgrade);
  REQUIRE(rows.has_install_row);
  REQUIRE(rows.install_row.nevra == available.nevra);
  REQUIRE_FALSE(rows.has_installed_row);
  REQUIRE_FALSE(rows.exact_reinstall_available);
  REQUIRE_FALSE(pending_transaction_selection_allows_installed_action(available, rows, false));
  REQUIRE_FALSE(pending_transaction_selection_allows_installed_action(available, rows, true));
}

// -----------------------------------------------------------------------------
// Verify that the shared action result enables only install for a plain available row.
// -----------------------------------------------------------------------------
TEST_CASE("Pending transaction selection actions resolve plain available package")
{
  reset_backend_globals();

  PackageRow available = make_test_package_row("demo-1.0-1.x86_64", "demo", "1.0", "1", "x86_64");
  DisplayedPackageQueryState displayed;
  displayed.kind = DisplayedPackageQueryKind::LIST_PACKAGES;

  PendingTransactionSelectionActions actions =
      pending_transaction_actions_for_selection(available, nullptr, 0, displayed, {});

  REQUIRE(actions.rows.has_install_row);
  REQUIRE(actions.has_install_action);
  REQUIRE(actions.can_install);
  REQUIRE_FALSE(actions.has_installed_action);
  REQUIRE_FALSE(actions.can_remove);
  REQUIRE_FALSE(actions.can_reinstall);
  REQUIRE(actions.install_action_nevra == available.nevra);
  REQUIRE(actions.installed_action_nevra.empty());
}

// -----------------------------------------------------------------------------
// Verify that normal package installs replace another version with the same name and architecture.
// -----------------------------------------------------------------------------
TEST_CASE("Pending transaction normal install marking replaces same package identity")
{
  reset_backend_globals();

  PackageRow older = make_test_package_row("demo-1.0-1.x86_64", "demo", "1.0", "1", "x86_64");
  PackageRow newer = make_test_package_row("demo-2.0-1.x86_64", "demo", "2.0", "1", "x86_64");

  PendingTransactionActionRows older_rows = pending_transaction_action_rows_for_selection(older, nullptr, 0, false);
  PendingTransactionActionRows newer_rows = pending_transaction_action_rows_for_selection(newer, nullptr, 0, false);

  std::vector<PendingAction> actions;
  REQUIRE(pending_transaction_mark_install_side_action(actions, older_rows));
  REQUIRE(pending_transaction_mark_install_side_action(actions, newer_rows));

  REQUIRE(actions.size() == 1);
  REQUIRE(actions[0].type == PendingAction::INSTALL);
  REQUIRE(actions[0].nevra == newer.nevra);
  REQUIRE(actions[0].transaction_spec == newer.nevra);
  REQUIRE(actions[0].package_key == newer.name_arch_key());
  REQUIRE_FALSE(actions[0].installonly);
}

// -----------------------------------------------------------------------------
// Verify that installonly package installs keep distinct exact versions.
// -----------------------------------------------------------------------------
TEST_CASE("Pending transaction installonly marking keeps distinct versions")
{
  reset_backend_globals();

  PackageRow older = make_test_package_row("installonly-demo-1.0-1.x86_64", "installonly-demo", "1.0", "1", "x86_64");
  PackageRow newer = make_test_package_row("installonly-demo-2.0-1.x86_64", "installonly-demo", "2.0", "1", "x86_64");
  older.installonly = true;
  newer.installonly = true;

  PendingTransactionActionRows older_rows = pending_transaction_action_rows_for_selection(older, nullptr, 0, false);
  PendingTransactionActionRows newer_rows = pending_transaction_action_rows_for_selection(newer, nullptr, 0, false);

  std::vector<PendingAction> actions;
  REQUIRE(pending_transaction_mark_install_side_action(actions, older_rows));
  REQUIRE(pending_transaction_mark_install_side_action(actions, newer_rows));

  REQUIRE(actions.size() == 2);
  REQUIRE(actions[0].type == PendingAction::INSTALL);
  REQUIRE(actions[0].nevra == older.nevra);
  REQUIRE(actions[0].package_key == older.name_arch_key());
  REQUIRE(actions[0].installonly);
  REQUIRE(actions[1].type == PendingAction::INSTALL);
  REQUIRE(actions[1].nevra == newer.nevra);
  REQUIRE(actions[1].package_key == newer.name_arch_key());
  REQUIRE(actions[1].installonly);
}

// -----------------------------------------------------------------------------
// Verify that marking the same installonly version twice does not duplicate it.
// -----------------------------------------------------------------------------
TEST_CASE("Pending transaction installonly marking replaces exact duplicate")
{
  reset_backend_globals();

  PackageRow installonly =
      make_test_package_row("installonly-demo-2.0-1.x86_64", "installonly-demo", "2.0", "1", "x86_64");
  installonly.installonly = true;

  PendingTransactionActionRows rows = pending_transaction_action_rows_for_selection(installonly, nullptr, 0, false);

  std::vector<PendingAction> actions;
  REQUIRE(pending_transaction_mark_install_side_action(actions, rows));
  REQUIRE(pending_transaction_mark_install_side_action(actions, rows));

  REQUIRE(actions.size() == 1);
  REQUIRE(actions[0].type == PendingAction::INSTALL);
  REQUIRE(actions[0].nevra == installonly.nevra);
  REQUIRE(actions[0].installonly);
}

// -----------------------------------------------------------------------------
// Verify that exact all-version installonly rows install even when another version is installed.
// -----------------------------------------------------------------------------
TEST_CASE("Pending transaction all-version installonly row uses exact install")
{
  reset_backend_globals();

  PackageRow installed =
      make_test_package_row("installonly-demo-1.0-1.x86_64", "installonly-demo", "1.0", "1", "x86_64");
  PackageRow newer = make_test_package_row("installonly-demo-2.0-1.x86_64", "installonly-demo", "2.0", "1", "x86_64");
  newer.installonly = true;
  newer.is_newest_available = true;

  dnf_backend_testonly_replace_installed_snapshot_rows({ installed });

  PendingTransactionActionRows compact_rows = pending_transaction_action_rows_for_selection(newer, nullptr, 0, false);
  REQUIRE(compact_rows.state == PackageInstallState::UPGRADEABLE);
  REQUIRE(compact_rows.install_is_upgrade);
  REQUIRE_FALSE(compact_rows.install_is_downgrade);
  REQUIRE(compact_rows.has_install_row);
  REQUIRE(compact_rows.upgrade_spec == "installonly-demo.x86_64");

  PendingTransactionActionRows exact_rows = pending_transaction_action_rows_for_selection(newer, nullptr, 0, true);
  REQUIRE(exact_rows.state == PackageInstallState::UPGRADEABLE);
  REQUIRE_FALSE(exact_rows.install_is_upgrade);
  REQUIRE_FALSE(exact_rows.install_is_downgrade);
  REQUIRE(exact_rows.has_install_row);
  REQUIRE(exact_rows.install_row.nevra == newer.nevra);

  std::vector<PendingAction> actions;
  REQUIRE(pending_transaction_mark_install_side_action(actions, exact_rows));

  REQUIRE(actions.size() == 1);
  REQUIRE(actions[0].type == PendingAction::INSTALL);
  REQUIRE(actions[0].nevra == newer.nevra);
  REQUIRE(actions[0].transaction_spec == newer.nevra);
  REQUIRE(actions[0].installonly);
}

// -----------------------------------------------------------------------------
// Verify that an exact install action keeps install meaning after switching to a compact view.
// -----------------------------------------------------------------------------
TEST_CASE("Pending transaction pending exact installonly action remains install in compact view")
{
  reset_backend_globals();

  PackageRow installed =
      make_test_package_row("installonly-demo-1.0-1.x86_64", "installonly-demo", "1.0", "1", "x86_64");
  PackageRow newer = make_test_package_row("installonly-demo-2.0-1.x86_64", "installonly-demo", "2.0", "1", "x86_64");
  installed.installonly = true;
  installed.repo_candidate_relation = PackageRepoCandidateRelation::NEWER;
  installed.repo_candidate_nevra = newer.nevra;
  installed.repo_candidate_is_newest_available = true;
  newer.installonly = true;
  newer.is_newest_available = true;

  dnf_backend_testonly_replace_installed_snapshot_rows({ installed });

  PendingTransactionActionRows exact_rows = pending_transaction_action_rows_for_selection(newer, nullptr, 0, true);

  std::vector<PendingAction> actions;
  REQUIRE(pending_transaction_mark_install_side_action(actions, exact_rows));
  REQUIRE(actions[0].type == PendingAction::INSTALL);

  PendingTransactionActionRows compact_rows =
      pending_transaction_action_rows_for_selection_with_pending(installed, nullptr, 0, false, actions);

  REQUIRE(compact_rows.state == PackageInstallState::UPGRADEABLE);
  REQUIRE_FALSE(compact_rows.install_is_upgrade);
  REQUIRE_FALSE(compact_rows.install_is_downgrade);
  REQUIRE(compact_rows.has_install_row);
  REQUIRE(compact_rows.install_action_from_pending);
  REQUIRE(compact_rows.install_row.nevra == newer.nevra);
  REQUIRE(pending_transaction_selection_allows_install_button_action(installed, compact_rows, true));

  actions.clear();
  PendingTransactionActionRows normal_compact_rows =
      pending_transaction_action_rows_for_selection_with_pending(installed, nullptr, 0, false, actions);
  REQUIRE(normal_compact_rows.install_is_upgrade);
  REQUIRE(normal_compact_rows.has_install_row);
}

// -----------------------------------------------------------------------------
// Verify that a pending compact upgrade keeps upgrade meaning in an exact view.
// -----------------------------------------------------------------------------
TEST_CASE("Pending transaction pending installonly upgrade remains upgrade in all-version view")
{
  reset_backend_globals();

  PackageRow installed =
      make_test_package_row("installonly-demo-1.0-1.x86_64", "installonly-demo", "1.0", "1", "x86_64");
  PackageRow newer = make_test_package_row("installonly-demo-2.0-1.x86_64", "installonly-demo", "2.0", "1", "x86_64");
  installed.installonly = true;
  installed.repo_candidate_relation = PackageRepoCandidateRelation::NEWER;
  installed.repo_candidate_nevra = newer.nevra;
  installed.repo_candidate_is_newest_available = true;
  newer.installonly = true;
  newer.is_newest_available = true;

  dnf_backend_testonly_replace_installed_snapshot_rows({ installed });

  PendingTransactionActionRows compact_rows =
      pending_transaction_action_rows_for_selection(installed, nullptr, 0, false);

  std::vector<PendingAction> actions;
  REQUIRE(pending_transaction_mark_install_side_action(actions, compact_rows));
  REQUIRE(actions[0].type == PendingAction::UPGRADE);

  PendingTransactionActionRows exact_rows =
      pending_transaction_action_rows_for_selection_with_pending(newer, nullptr, 0, true, actions);

  REQUIRE(exact_rows.state == PackageInstallState::UPGRADEABLE);
  REQUIRE(exact_rows.install_is_upgrade);
  REQUIRE_FALSE(exact_rows.install_is_downgrade);
  REQUIRE(exact_rows.has_install_row);
  REQUIRE(exact_rows.install_action_from_pending);
  REQUIRE(exact_rows.install_row.nevra == newer.nevra);
  REQUIRE(exact_rows.upgrade_spec == "installonly-demo.x86_64");
  REQUIRE(pending_transaction_selection_allows_install_button_action(newer, exact_rows, true));

  actions.clear();
  PendingTransactionActionRows normal_exact_rows =
      pending_transaction_action_rows_for_selection_with_pending(newer, nullptr, 0, true, actions);
  REQUIRE_FALSE(normal_exact_rows.install_is_upgrade);
  REQUIRE(normal_exact_rows.has_install_row);
}

// -----------------------------------------------------------------------------
// Verify that a pending installonly upgrade does not project onto exact installed rows.
// -----------------------------------------------------------------------------
TEST_CASE("Pending transaction pending installonly upgrade stays off exact installed row")
{
  reset_backend_globals();

  PackageRow installed =
      make_test_package_row("installonly-demo-1.0-1.x86_64", "installonly-demo", "1.0", "1", "x86_64");
  PackageRow newer = make_test_package_row("installonly-demo-2.0-1.x86_64", "installonly-demo", "2.0", "1", "x86_64");
  installed.installonly = true;
  installed.repo_candidate_relation = PackageRepoCandidateRelation::NEWER;
  installed.repo_candidate_nevra = newer.nevra;
  installed.repo_candidate_is_newest_available = true;
  newer.installonly = true;
  newer.is_newest_available = true;

  dnf_backend_testonly_replace_installed_snapshot_rows({ installed });

  PendingTransactionActionRows compact_rows =
      pending_transaction_action_rows_for_selection(installed, nullptr, 0, false);

  std::vector<PendingAction> actions;
  REQUIRE(pending_transaction_mark_install_side_action(actions, compact_rows));
  REQUIRE(actions[0].type == PendingAction::UPGRADE);

  PendingTransactionActionRows exact_installed_rows =
      pending_transaction_action_rows_for_selection_with_pending(installed, nullptr, 0, true, actions);

  REQUIRE(exact_installed_rows.state == PackageInstallState::UPGRADEABLE);
  REQUIRE_FALSE(exact_installed_rows.install_is_upgrade);
  REQUIRE_FALSE(exact_installed_rows.install_is_downgrade);
  REQUIRE_FALSE(exact_installed_rows.has_install_row);
  REQUIRE_FALSE(exact_installed_rows.install_action_from_pending);
  REQUIRE_FALSE(pending_transaction_selection_allows_install_button_action(installed, exact_installed_rows, true));
}

// -----------------------------------------------------------------------------
// Verify that a pending exact package view preserves installonly install semantics.
// -----------------------------------------------------------------------------
TEST_CASE("Pending transaction exact installonly action context uses exact install")
{
  reset_backend_globals();

  DisplayedPackageQueryState displayed;
  displayed.exact_installonly_action = true;
  REQUIRE(displayed_package_query_uses_exact_installonly_actions(displayed));

  PackageRow installed =
      make_test_package_row("installonly-demo-2.0-1.x86_64", "installonly-demo", "2.0", "1", "x86_64");
  PackageRow older = make_test_package_row("installonly-demo-1.0-1.x86_64", "installonly-demo", "1.0", "1", "x86_64");
  older.installonly = true;

  dnf_backend_testonly_replace_installed_snapshot_rows({ installed });

  PendingTransactionActionRows rows = pending_transaction_action_rows_for_selection(
      older, nullptr, 0, displayed_package_query_uses_exact_installonly_actions(displayed));

  REQUIRE(rows.state == PackageInstallState::DOWNGRADEABLE);
  REQUIRE_FALSE(rows.install_is_upgrade);
  REQUIRE_FALSE(rows.install_is_downgrade);
  REQUIRE(rows.has_install_row);
  REQUIRE(rows.install_row.nevra == older.nevra);
}

// -----------------------------------------------------------------------------
// Verify that installed installonly rows do not start stream upgrades in exact-version views.
// -----------------------------------------------------------------------------
TEST_CASE("Pending transaction all-version installed installonly row does not start upgrade")
{
  reset_backend_globals();

  PackageRow installed =
      make_test_package_row("installonly-demo-1.0-1.x86_64", "installonly-demo", "1.0", "1", "x86_64");
  installed.installonly = true;
  installed.repo_candidate_relation = PackageRepoCandidateRelation::NEWER;
  installed.repo_candidate_nevra = "installonly-demo-2.0-1.x86_64";
  installed.repo_candidate_is_newest_available = true;

  dnf_backend_testonly_replace_installed_snapshot_rows({ installed });

  PendingTransactionActionRows compact_rows =
      pending_transaction_action_rows_for_selection(installed, nullptr, 0, false);
  REQUIRE(compact_rows.install_is_upgrade);
  REQUIRE(compact_rows.has_install_row);

  PendingTransactionActionRows exact_rows = pending_transaction_action_rows_for_selection(installed, nullptr, 0, true);
  REQUIRE(exact_rows.state == PackageInstallState::UPGRADEABLE);
  REQUIRE_FALSE(exact_rows.install_is_upgrade);
  REQUIRE_FALSE(exact_rows.install_is_downgrade);
  REQUIRE_FALSE(exact_rows.has_install_row);
  REQUIRE(exact_rows.has_installed_row);
}

// -----------------------------------------------------------------------------
// Verify that exact older installonly rows install instead of becoming downgrades.
// -----------------------------------------------------------------------------
TEST_CASE("Pending transaction all-version older installonly row uses exact install")
{
  reset_backend_globals();

  PackageRow installed =
      make_test_package_row("installonly-demo-2.0-1.x86_64", "installonly-demo", "2.0", "1", "x86_64");
  PackageRow older = make_test_package_row("installonly-demo-1.0-1.x86_64", "installonly-demo", "1.0", "1", "x86_64");
  older.installonly = true;

  dnf_backend_testonly_replace_installed_snapshot_rows({ installed });

  PendingTransactionActionRows exact_rows = pending_transaction_action_rows_for_selection(older, nullptr, 0, true);
  REQUIRE(exact_rows.state == PackageInstallState::DOWNGRADEABLE);
  REQUIRE_FALSE(exact_rows.install_is_upgrade);
  REQUIRE_FALSE(exact_rows.install_is_downgrade);
  REQUIRE(exact_rows.has_install_row);
  REQUIRE(exact_rows.install_row.nevra == older.nevra);

  std::vector<PendingAction> actions;
  REQUIRE(pending_transaction_mark_install_side_action(actions, exact_rows));

  REQUIRE(actions.size() == 1);
  REQUIRE(actions[0].type == PendingAction::INSTALL);
  REQUIRE(actions[0].nevra == older.nevra);
  REQUIRE(actions[0].transaction_spec == older.nevra);
  REQUIRE(actions[0].installonly);
}

// -----------------------------------------------------------------------------
// Verify that two exact installonly rows can be requested when another version is installed.
// -----------------------------------------------------------------------------
TEST_CASE("Pending transaction all-version installonly actions build one install request")
{
  reset_backend_globals();

  PackageRow installed =
      make_test_package_row("installonly-demo-2.0-1.x86_64", "installonly-demo", "2.0", "1", "x86_64");
  PackageRow older = make_test_package_row("installonly-demo-1.0-1.x86_64", "installonly-demo", "1.0", "1", "x86_64");
  PackageRow newer = make_test_package_row("installonly-demo-3.0-1.x86_64", "installonly-demo", "3.0", "1", "x86_64");
  older.installonly = true;
  newer.installonly = true;
  newer.is_newest_available = true;

  dnf_backend_testonly_replace_installed_snapshot_rows({ installed });

  PendingTransactionActionRows older_rows = pending_transaction_action_rows_for_selection(older, nullptr, 0, true);
  PendingTransactionActionRows newer_rows = pending_transaction_action_rows_for_selection(newer, nullptr, 0, true);

  std::vector<PendingAction> actions;
  REQUIRE(pending_transaction_mark_install_side_action(actions, older_rows));
  REQUIRE(pending_transaction_mark_install_side_action(actions, newer_rows));

  TransactionRequest request;
  std::string error;
  REQUIRE(pending_transaction_build_request(actions, request, error));
  REQUIRE(error.empty());
  REQUIRE(request.install ==
          std::vector<std::string> {
              older.nevra,
              newer.nevra,
          });
}

// -----------------------------------------------------------------------------
// Verify that an installed row with a stored repo candidate upgrades that candidate.
// -----------------------------------------------------------------------------
TEST_CASE("Pending transaction action rows resolve upgrade from installed package row")
{
  reset_backend_globals();

  PackageRow installed = make_test_package_row("demo-1.0-1.x86_64", "demo", "1.0", "1", "x86_64");
  installed.repo_candidate_relation = PackageRepoCandidateRelation::NEWER;
  installed.repo_candidate_nevra = "demo-2.0-1.x86_64";
  installed.repo_candidate_is_newest_available = true;

  dnf_backend_testonly_replace_installed_snapshot_rows({ installed });

  PendingTransactionActionRows rows = pending_transaction_action_rows_for_selection(installed, nullptr, 0, false);

  REQUIRE(rows.state == PackageInstallState::UPGRADEABLE);
  REQUIRE(rows.install_is_upgrade);
  REQUIRE(rows.has_install_row);
  REQUIRE(rows.install_row.nevra == installed.repo_candidate_nevra);
  REQUIRE(rows.upgrade_spec == "demo.x86_64");
  REQUIRE(rows.has_installed_row);
  REQUIRE(rows.installed_row.nevra == installed.nevra);
  REQUIRE_FALSE(rows.exact_reinstall_available);
  REQUIRE_FALSE(pending_transaction_selection_allows_install_action(installed, rows, false));
  REQUIRE(pending_transaction_selection_allows_install_button_action(installed, rows, true));

  std::vector<PendingAction> actions;
  REQUIRE(pending_transaction_mark_install_side_action(actions, rows));

  REQUIRE(actions.size() == 1);
  REQUIRE(actions[0].type == PendingAction::UPGRADE);
  REQUIRE(actions[0].nevra == installed.repo_candidate_nevra);
  REQUIRE(actions[0].transaction_spec == "demo.x86_64");
}

// -----------------------------------------------------------------------------
// Verify that an installed row cannot upgrade to a hidden intermediate candidate.
// -----------------------------------------------------------------------------
TEST_CASE("Pending transaction action rows reject installed row with non-newest candidate")
{
  reset_backend_globals();

  PackageRow installed = make_test_package_row("demo-1.0-1.x86_64", "demo", "1.0", "1", "x86_64");
  installed.repo_candidate_relation = PackageRepoCandidateRelation::NEWER;
  installed.repo_candidate_nevra = "demo-1.5-1.x86_64";
  installed.repo_candidate_is_newest_available = false;

  dnf_backend_testonly_replace_installed_snapshot_rows({ installed });

  PendingTransactionActionRows rows = pending_transaction_action_rows_for_selection(installed, nullptr, 0, false);

  REQUIRE(rows.state == PackageInstallState::UPGRADEABLE);
  REQUIRE(rows.install_is_upgrade);
  REQUIRE_FALSE(rows.has_install_row);
  REQUIRE(rows.has_installed_row);
  REQUIRE(rows.installed_row.nevra == installed.nevra);
  REQUIRE_FALSE(pending_transaction_activation_should_remove(installed, rows));
  REQUIRE_FALSE(pending_transaction_selection_allows_install_button_action(installed, rows, true));
}

// -----------------------------------------------------------------------------
// Verify that self-protection blocks installs but still allows normal upgrades.
// -----------------------------------------------------------------------------
TEST_CASE("Pending transaction action rows allow protected upgrade action")
{
  PendingTransactionActionRows install_rows;
  install_rows.install_is_upgrade = false;

  REQUIRE(pending_transaction_install_action_blocked_by_self_protection(install_rows, true));

  PendingTransactionActionRows upgrade_rows;
  upgrade_rows.install_is_upgrade = true;

  REQUIRE_FALSE(pending_transaction_install_action_blocked_by_self_protection(upgrade_rows, true));
  REQUIRE_FALSE(pending_transaction_install_action_blocked_by_self_protection(upgrade_rows, false));

  PendingTransactionActionRows downgrade_rows;
  downgrade_rows.install_is_downgrade = true;

  REQUIRE(pending_transaction_install_action_blocked_by_self_protection(downgrade_rows, true));
}

// -----------------------------------------------------------------------------
// Verify that an update candidate exposes installed actions only in compact views.
// -----------------------------------------------------------------------------
TEST_CASE("Pending transaction action rows resolve upgrade from available update row")
{
  reset_backend_globals();

  PackageRow installed = make_test_package_row("demo-1.0-1.x86_64", "demo", "1.0", "1", "x86_64");
  PackageRow update = make_test_package_row("demo-2.0-1.x86_64", "demo", "2.0", "1", "x86_64");
  update.is_newest_available = true;

  dnf_backend_testonly_replace_installed_snapshot_rows({ installed });

  PendingTransactionActionRows rows = pending_transaction_action_rows_for_selection(update, nullptr, 0, false);

  REQUIRE(rows.state == PackageInstallState::UPGRADEABLE);
  REQUIRE(rows.install_is_upgrade);
  REQUIRE(rows.has_install_row);
  REQUIRE(rows.install_row.nevra == update.nevra);
  REQUIRE(rows.upgrade_spec == "demo.x86_64");
  REQUIRE(rows.has_installed_row);
  REQUIRE(rows.installed_row.nevra == installed.nevra);
  REQUIRE_FALSE(rows.exact_reinstall_available);
  REQUIRE(pending_transaction_selection_allows_install_button_action(update, rows, false));
  REQUIRE_FALSE(pending_transaction_selection_allows_installed_action(update, rows, false));
  REQUIRE(pending_transaction_selection_allows_installed_action(update, rows, true));
}

// -----------------------------------------------------------------------------
// Verify that the shared action result applies compact-view installed action rules.
// -----------------------------------------------------------------------------
TEST_CASE("Pending transaction selection actions resolve compact update row")
{
  reset_backend_globals();

  PackageRow installed = make_test_package_row("demo-1.0-1.x86_64", "demo", "1.0", "1", "x86_64");
  installed.repo_candidate_exact_available = true;
  PackageRow update = make_test_package_row("demo-2.0-1.x86_64", "demo", "2.0", "1", "x86_64");
  update.is_newest_available = true;

  dnf_backend_testonly_replace_installed_snapshot_rows({ installed });

  DisplayedPackageQueryState displayed;
  displayed.kind = DisplayedPackageQueryKind::LIST_PACKAGES;
  displayed.latest_only = true;

  PendingTransactionSelectionActions actions =
      pending_transaction_actions_for_selection(update, nullptr, 0, displayed, {});

  REQUIRE(actions.rows.install_is_upgrade);
  REQUIRE(actions.has_install_action);
  REQUIRE(actions.has_installed_action);
  REQUIRE(actions.can_install);
  REQUIRE(actions.can_remove);
  REQUIRE(actions.can_reinstall);
  REQUIRE(actions.install_action_nevra == update.nevra);
  REQUIRE(actions.installed_action_nevra == installed.nevra);

  std::vector<PendingAction> pending_actions = {
    { PendingAction::UPGRADE, update.nevra, "demo.x86_64", update.name_arch_key() },
    { PendingAction::REINSTALL, installed.nevra, installed.nevra, installed.name_arch_key() },
  };
  PendingTransactionSelectionActions pending =
      pending_transaction_actions_for_selection(update, nullptr, 0, displayed, pending_actions);

  REQUIRE(pending.install_is_pending);
  REQUIRE_FALSE(pending.remove_is_pending);
  REQUIRE(pending.reinstall_is_pending);
}

// -----------------------------------------------------------------------------
// Verify that compact update rows inherit exact reinstall availability from the installed package.
// -----------------------------------------------------------------------------
TEST_CASE("Pending transaction action rows allow compact update reinstall when installed exact is available")
{
  reset_backend_globals();

  PackageRow installed = make_test_package_row("demo-1.0-1.x86_64", "demo", "1.0", "1", "x86_64");
  installed.repo_candidate_exact_available = true;
  PackageRow update = make_test_package_row("demo-2.0-1.x86_64", "demo", "2.0", "1", "x86_64");
  update.is_newest_available = true;

  dnf_backend_testonly_replace_installed_snapshot_rows({ installed });

  PendingTransactionActionRows rows = pending_transaction_action_rows_for_selection(update, nullptr, 0, false);

  REQUIRE(rows.state == PackageInstallState::UPGRADEABLE);
  REQUIRE(rows.has_installed_row);
  REQUIRE(rows.installed_row.nevra == installed.nevra);
  REQUIRE(rows.exact_reinstall_available);
  REQUIRE(pending_transaction_selection_allows_installed_action(update, rows, true));
}

// -----------------------------------------------------------------------------
// Verify that local installed refresh keeps exact reinstall availability for the same installed package.
// -----------------------------------------------------------------------------
TEST_CASE("Pending transaction action rows keep reinstall after local installed refresh")
{
  reset_backend_globals();

  PackageRow installed = make_test_package_row("demo-1.0-1.x86_64", "demo", "1.0", "1", "x86_64");
  installed.repo_candidate_exact_available = true;
  REQUIRE(dnf_backend_internal::publish_installed_snapshot(make_single_installed_query_result(installed), {}));

  PackageRow local_installed = installed;
  local_installed.repo_candidate_exact_available = false;
  REQUIRE_FALSE(
      dnf_backend_internal::publish_local_installed_snapshot(make_single_installed_query_result(local_installed), {}));

  PackageRow update = make_test_package_row("demo-2.0-1.x86_64", "demo", "2.0", "1", "x86_64");
  update.is_newest_available = true;

  PendingTransactionActionRows rows = pending_transaction_action_rows_for_selection(update, nullptr, 0, false);

  REQUIRE(rows.state == PackageInstallState::UPGRADEABLE);
  REQUIRE(rows.has_installed_row);
  REQUIRE(rows.installed_row.nevra == installed.nevra);
  REQUIRE(rows.exact_reinstall_available);
}

// -----------------------------------------------------------------------------
// Verify that local installed refresh does not copy exact availability to a new installed package.
// -----------------------------------------------------------------------------
TEST_CASE("Pending transaction action rows do not inherit reinstall availability after local installed replacement")
{
  reset_backend_globals();

  PackageRow installed = make_test_package_row("demo-1.0-1.x86_64", "demo", "1.0", "1", "x86_64");
  installed.repo_candidate_exact_available = true;
  REQUIRE(dnf_backend_internal::publish_installed_snapshot(make_single_installed_query_result(installed), {}));

  PackageRow replacement = make_test_package_row("demo-1.1-1.x86_64", "demo", "1.1", "1", "x86_64");
  REQUIRE(dnf_backend_internal::publish_local_installed_snapshot(make_single_installed_query_result(replacement), {}));

  PackageRow update = make_test_package_row("demo-2.0-1.x86_64", "demo", "2.0", "1", "x86_64");
  update.is_newest_available = true;

  PendingTransactionActionRows rows = pending_transaction_action_rows_for_selection(update, nullptr, 0, false);

  REQUIRE(rows.state == PackageInstallState::UPGRADEABLE);
  REQUIRE(rows.has_installed_row);
  REQUIRE(rows.installed_row.nevra == replacement.nevra);
  REQUIRE_FALSE(rows.exact_reinstall_available);
}

// -----------------------------------------------------------------------------
// Verify that an intermediate newer row is visible but cannot be marked as an upgrade.
// -----------------------------------------------------------------------------
TEST_CASE("Pending transaction action rows reject intermediate update rows")
{
  reset_backend_globals();

  PackageRow installed = make_test_package_row("demo-1.0-1.x86_64", "demo", "1.0", "1", "x86_64");
  PackageRow intermediate = make_test_package_row("demo-2.0-1.x86_64", "demo", "2.0", "1", "x86_64");

  dnf_backend_testonly_replace_installed_snapshot_rows({ installed });

  PendingTransactionActionRows rows = pending_transaction_action_rows_for_selection(intermediate, nullptr, 0, false);

  REQUIRE(rows.state == PackageInstallState::UPGRADEABLE);
  REQUIRE(rows.install_is_upgrade);
  REQUIRE_FALSE(rows.has_install_row);
  REQUIRE(rows.has_installed_row);
  REQUIRE(rows.installed_row.nevra == installed.nevra);
  REQUIRE_FALSE(pending_transaction_activation_should_remove(intermediate, rows));
  REQUIRE_FALSE(pending_transaction_selection_allows_install_button_action(intermediate, rows, true));
}

// -----------------------------------------------------------------------------
// Verify that activation can remove only the exact installed row.
// -----------------------------------------------------------------------------
TEST_CASE("Pending transaction activation removes exact installed row only")
{
  reset_backend_globals();

  PackageRow installed = make_test_package_row("demo-1.0-1.x86_64", "demo", "1.0", "1", "x86_64");

  dnf_backend_testonly_replace_installed_snapshot_rows({ installed });

  PendingTransactionActionRows rows = pending_transaction_action_rows_for_selection(installed, nullptr, 0, false);

  REQUIRE(rows.has_installed_row);
  REQUIRE(rows.installed_row.nevra == installed.nevra);
  REQUIRE(pending_transaction_activation_should_remove(installed, rows));
  REQUIRE(pending_transaction_selection_allows_installed_action(installed, rows, false));
}

// -----------------------------------------------------------------------------
// Verify that an older available row can be downgraded but cannot modify the installed row.
// -----------------------------------------------------------------------------
TEST_CASE("Pending transaction action rows resolve downgrade from available row")
{
  reset_backend_globals();

  PackageRow installed = make_test_package_row("demo-2.0-1.x86_64", "demo", "2.0", "1", "x86_64");
  PackageRow older = make_test_package_row("demo-1.0-1.x86_64", "demo", "1.0", "1", "x86_64");

  dnf_backend_testonly_replace_installed_snapshot_rows({ installed });

  PendingTransactionActionRows rows = pending_transaction_action_rows_for_selection(older, nullptr, 0, false);

  REQUIRE(rows.state == PackageInstallState::DOWNGRADEABLE);
  REQUIRE_FALSE(rows.install_is_upgrade);
  REQUIRE(rows.install_is_downgrade);
  REQUIRE(rows.has_install_row);
  REQUIRE(rows.install_row.nevra == older.nevra);
  REQUIRE(rows.has_installed_row);
  REQUIRE(rows.installed_row.nevra == installed.nevra);

  std::vector<PendingAction> actions;
  REQUIRE(pending_transaction_mark_install_side_action(actions, rows));

  REQUIRE(actions.size() == 1);
  REQUIRE(actions[0].type == PendingAction::DOWNGRADE);
  REQUIRE(actions[0].nevra == older.nevra);
  REQUIRE(actions[0].transaction_spec == older.nevra);
  REQUIRE(actions[0].package_key == older.name_arch_key());
  REQUIRE_FALSE(pending_transaction_selection_allows_installed_action(older, rows, false));
  REQUIRE_FALSE(pending_transaction_selection_allows_installed_action(older, rows, true));
}

// -----------------------------------------------------------------------------
// Verify that two downgrade targets for the same package name and architecture replace each other.
// -----------------------------------------------------------------------------
TEST_CASE("Pending transaction downgrade marking replaces same architecture target")
{
  reset_backend_globals();

  PackageRow installed = make_test_package_row("demo-3.0-1.x86_64", "demo", "3.0", "1", "x86_64");
  PackageRow older = make_test_package_row("demo-1.0-1.x86_64", "demo", "1.0", "1", "x86_64");
  PackageRow newer_downgrade = make_test_package_row("demo-2.0-1.x86_64", "demo", "2.0", "1", "x86_64");

  dnf_backend_testonly_replace_installed_snapshot_rows({ installed });

  PendingTransactionActionRows older_rows = pending_transaction_action_rows_for_selection(older, nullptr, 0, false);
  PendingTransactionActionRows newer_downgrade_rows =
      pending_transaction_action_rows_for_selection(newer_downgrade, nullptr, 0, false);

  std::vector<PendingAction> actions;
  REQUIRE(pending_transaction_mark_install_side_action(actions, older_rows));
  REQUIRE(pending_transaction_mark_install_side_action(actions, newer_downgrade_rows));

  REQUIRE(actions.size() == 1);
  REQUIRE(actions[0].type == PendingAction::DOWNGRADE);
  REQUIRE(actions[0].nevra == newer_downgrade.nevra);
  REQUIRE(actions[0].transaction_spec == newer_downgrade.nevra);
  REQUIRE(actions[0].package_key == newer_downgrade.name_arch_key());
}

// -----------------------------------------------------------------------------
// Verify that downgrade targets for different architectures stay independent.
// -----------------------------------------------------------------------------
TEST_CASE("Pending transaction downgrade marking keeps different architecture targets")
{
  reset_backend_globals();

  PackageRow installed_x86_64 = make_test_package_row("demo-3.0-1.x86_64", "demo", "3.0", "1", "x86_64");
  PackageRow installed_i686 = make_test_package_row("demo-3.0-1.i686", "demo", "3.0", "1", "i686");
  PackageRow older_x86_64 = make_test_package_row("demo-2.0-1.x86_64", "demo", "2.0", "1", "x86_64");
  PackageRow older_i686 = make_test_package_row("demo-2.0-1.i686", "demo", "2.0", "1", "i686");

  dnf_backend_testonly_replace_installed_snapshot_rows({ installed_x86_64, installed_i686 });

  PendingTransactionActionRows x86_64_rows =
      pending_transaction_action_rows_for_selection(older_x86_64, nullptr, 0, false);
  PendingTransactionActionRows i686_rows = pending_transaction_action_rows_for_selection(older_i686, nullptr, 0, false);

  std::vector<PendingAction> actions;
  REQUIRE(pending_transaction_mark_install_side_action(actions, x86_64_rows));
  REQUIRE(pending_transaction_mark_install_side_action(actions, i686_rows));

  REQUIRE(actions.size() == 2);
  REQUIRE(actions[0].type == PendingAction::DOWNGRADE);
  REQUIRE(actions[0].nevra == older_x86_64.nevra);
  REQUIRE(actions[0].package_key == older_x86_64.name_arch_key());
  REQUIRE(actions[1].type == PendingAction::DOWNGRADE);
  REQUIRE(actions[1].nevra == older_i686.nevra);
  REQUIRE(actions[1].package_key == older_i686.name_arch_key());
}

// -----------------------------------------------------------------------------
// Verify that exact installed rows keep their own NEVRA when another installed EVR exists.
// -----------------------------------------------------------------------------
TEST_CASE("Pending transaction action rows keep exact installed row")
{
  reset_backend_globals();

  PackageRow older = make_test_package_row("demo-1.0-1.x86_64", "demo", "1.0", "1", "x86_64");
  PackageRow newer = make_test_package_row("demo-2.0-1.x86_64", "demo", "2.0", "1", "x86_64");

  dnf_backend_testonly_replace_installed_snapshot_rows({ older, newer });

  PendingTransactionActionRows rows = pending_transaction_action_rows_for_selection(older, nullptr, 0, false);

  REQUIRE(rows.has_installed_row);
  REQUIRE(rows.installed_row.nevra == older.nevra);
}

// -----------------------------------------------------------------------------
// Verify that daemon upgrade targets are used only while their snapshot is current.
// -----------------------------------------------------------------------------
TEST_CASE("Pending transaction action rows resolve daemon upgrade target")
{
  reset_backend_globals();
  DaemonUpgradeState &state = DaemonUpgradeState::instance();
  state.reset_for_tests();

  PackageRow installed = make_test_package_row("demo-1.0-1.x86_64", "demo", "1.0", "1", "x86_64");
  PackageRow update = make_test_package_row("demo-2.0-1.x86_64", "demo", "2.0", "1", "x86_64");
  DaemonUpgradeTarget target = make_test_upgrade_target("demo-2.0-1.x86_64", "demo", "2.0", "1", "x86_64");

  dnf_backend_testonly_replace_installed_snapshot_rows({ installed });

  std::optional<DaemonUpgradeRefreshId> refresh_id = state.begin_refresh();
  REQUIRE(refresh_id.has_value());

  std::string error;
  REQUIRE(state.publish_success(refresh_id.value(), { target }, error) == DaemonUpgradePublishResult::PUBLISHED);
  DaemonUpgradeSnapshot snapshot = state.snapshot();

  PendingTransactionActionRows rows =
      pending_transaction_action_rows_for_selection(update, &target, snapshot.generation, false);

  REQUIRE(rows.state == PackageInstallState::UPGRADEABLE);
  REQUIRE(rows.install_is_upgrade);
  REQUIRE(rows.has_install_row);
  REQUIRE(rows.install_row.nevra == target.nevra);
  REQUIRE(rows.upgrade_spec == "demo.x86_64");
  REQUIRE(rows.has_installed_row);
  REQUIRE(rows.installed_row.nevra == installed.nevra);
}

// -----------------------------------------------------------------------------
// Verify that daemon target context is enough to identify an upgrade row.
// -----------------------------------------------------------------------------
TEST_CASE("Pending transaction action rows resolve daemon target without installed metadata")
{
  reset_backend_globals();
  DaemonUpgradeState &state = DaemonUpgradeState::instance();
  state.reset_for_tests();

  PackageRow update = make_test_package_row("demo-2.0-1.x86_64", "demo", "2.0", "1", "x86_64");
  DaemonUpgradeTarget target = make_test_upgrade_target("demo-2.0-1.x86_64", "demo", "2.0", "1", "x86_64");

  std::optional<DaemonUpgradeRefreshId> refresh_id = state.begin_refresh();
  REQUIRE(refresh_id.has_value());

  std::string error;
  REQUIRE(state.publish_success(refresh_id.value(), { target }, error) == DaemonUpgradePublishResult::PUBLISHED);
  DaemonUpgradeSnapshot snapshot = state.snapshot();

  PendingTransactionActionRows rows =
      pending_transaction_action_rows_for_selection(update, &target, snapshot.generation, false);

  REQUIRE(rows.state == PackageInstallState::UPGRADEABLE);
  REQUIRE(rows.install_is_upgrade);
  REQUIRE(rows.has_install_row);
  REQUIRE(rows.install_row.nevra == target.nevra);
  REQUIRE(rows.upgrade_spec == "demo.x86_64");
  REQUIRE_FALSE(rows.has_installed_row);
}

// -----------------------------------------------------------------------------
// Verify that stale daemon upgrade targets cannot create pending actions.
// -----------------------------------------------------------------------------
TEST_CASE("Pending transaction action rows reject stale daemon upgrade target")
{
  reset_backend_globals();
  DaemonUpgradeState &state = DaemonUpgradeState::instance();
  state.reset_for_tests();

  PackageRow installed = make_test_package_row("demo-1.0-1.x86_64", "demo", "1.0", "1", "x86_64");
  PackageRow update = make_test_package_row("demo-2.0-1.x86_64", "demo", "2.0", "1", "x86_64");
  DaemonUpgradeTarget target = make_test_upgrade_target("demo-2.0-1.x86_64", "demo", "2.0", "1", "x86_64");

  dnf_backend_testonly_replace_installed_snapshot_rows({ installed });

  std::optional<DaemonUpgradeRefreshId> refresh_id = state.begin_refresh();
  REQUIRE(refresh_id.has_value());

  std::string error;
  REQUIRE(state.publish_success(refresh_id.value(), { target }, error) == DaemonUpgradePublishResult::PUBLISHED);
  DaemonUpgradeSnapshot snapshot = state.snapshot();
  state.mark_stale();

  PendingTransactionActionRows rows =
      pending_transaction_action_rows_for_selection(update, &target, snapshot.generation, false);

  REQUIRE(rows.state == PackageInstallState::UPGRADEABLE);
  REQUIRE(rows.install_is_upgrade);
  REQUIRE_FALSE(rows.has_install_row);

  std::vector<PendingAction> actions;
  REQUIRE_FALSE(pending_transaction_mark_install_side_action(actions, rows));
  REQUIRE(actions.empty());
}

// -----------------------------------------------------------------------------
// Verify that daemon upgrade marking uses the daemon target ID and transaction spec.
// -----------------------------------------------------------------------------
TEST_CASE("Pending transaction upgrade marking uses daemon target")
{
  reset_backend_globals();
  DaemonUpgradeState &state = DaemonUpgradeState::instance();
  state.reset_for_tests();

  PackageRow installed = make_test_package_row("demo-1.0-1.x86_64", "demo", "1.0", "1", "x86_64");
  PackageRow update_metadata = make_test_package_row("demo-2.0-1.x86_64", "demo", "2.0", "1", "x86_64");
  DaemonUpgradeTarget target = make_test_upgrade_target("demo-2.0-1.x86_64", "demo", "2.0", "1", "x86_64");

  dnf_backend_testonly_replace_installed_snapshot_rows({ installed });

  std::optional<DaemonUpgradeRefreshId> refresh_id = state.begin_refresh();
  REQUIRE(refresh_id.has_value());

  std::string error;
  REQUIRE(state.publish_success(refresh_id.value(), { target }, error) == DaemonUpgradePublishResult::PUBLISHED);
  DaemonUpgradeSnapshot snapshot = state.snapshot();

  PendingTransactionActionRows rows =
      pending_transaction_action_rows_for_selection(update_metadata, &target, snapshot.generation, false);

  std::vector<PendingAction> actions;
  REQUIRE(pending_transaction_mark_install_side_action(actions, rows));

  REQUIRE(actions.size() == 1);
  REQUIRE(actions[0].type == PendingAction::UPGRADE);
  REQUIRE(actions[0].nevra == target.nevra);
  REQUIRE(actions[0].transaction_spec == target.upgrade_spec());
  REQUIRE(actions[0].package_key == update_metadata.name_arch_key());
}

// -----------------------------------------------------------------------------
// Verify that install, upgrade, and downgrade replace each other for one package name and architecture.
// -----------------------------------------------------------------------------
TEST_CASE("Pending transaction install-side marking replaces package action")
{
  reset_backend_globals();

  PackageRow installed = make_test_package_row("demo-2.0-1.x86_64", "demo", "2.0", "1", "x86_64");
  PackageRow older = make_test_package_row("demo-1.0-1.x86_64", "demo", "1.0", "1", "x86_64");
  PackageRow other = make_test_package_row("other-1.0-1.x86_64", "other", "1.0", "1", "x86_64");

  dnf_backend_testonly_replace_installed_snapshot_rows({ installed });

  std::vector<PendingAction> actions = {
    { PendingAction::INSTALL, installed.nevra, installed.nevra, installed.name_arch_key() },
    { PendingAction::REMOVE, other.nevra, other.nevra, other.name_arch_key() },
    { PendingAction::REINSTALL, "demo-3.0-1.x86_64", "demo-3.0-1.x86_64", installed.name_arch_key() },
  };

  PendingTransactionActionRows rows = pending_transaction_action_rows_for_selection(older, nullptr, 0, false);
  REQUIRE(pending_transaction_mark_install_side_action(actions, rows));

  REQUIRE(actions.size() == 2);
  REQUIRE(actions[0].type == PendingAction::REMOVE);
  REQUIRE(actions[0].nevra == other.nevra);
  REQUIRE(actions[1].type == PendingAction::DOWNGRADE);
  REQUIRE(actions[1].nevra == older.nevra);
}

// -----------------------------------------------------------------------------
// Verify that downgrade and remove replace each other for one package name and architecture.
// -----------------------------------------------------------------------------
TEST_CASE("Pending transaction downgrade and remove replacement is order independent")
{
  reset_backend_globals();

  PackageRow installed = make_test_package_row("demo-2.0-1.x86_64", "demo", "2.0", "1", "x86_64");
  PackageRow older = make_test_package_row("demo-1.0-1.x86_64", "demo", "1.0", "1", "x86_64");

  dnf_backend_testonly_replace_installed_snapshot_rows({ installed });

  PendingTransactionActionRows downgrade_rows = pending_transaction_action_rows_for_selection(older, nullptr, 0, false);
  PendingTransactionActionRows installed_rows =
      pending_transaction_action_rows_for_selection(installed, nullptr, 0, false);

  std::vector<PendingAction> downgrade_then_remove;
  REQUIRE(pending_transaction_mark_install_side_action(downgrade_then_remove, downgrade_rows));
  REQUIRE(pending_transaction_mark_remove_action(downgrade_then_remove, installed_rows));

  REQUIRE(downgrade_then_remove.size() == 1);
  REQUIRE(downgrade_then_remove[0].type == PendingAction::REMOVE);
  REQUIRE(downgrade_then_remove[0].nevra == installed.nevra);
  REQUIRE(downgrade_then_remove[0].package_key == installed.name_arch_key());

  std::vector<PendingAction> remove_then_downgrade;
  REQUIRE(pending_transaction_mark_remove_action(remove_then_downgrade, installed_rows));
  REQUIRE(pending_transaction_mark_install_side_action(remove_then_downgrade, downgrade_rows));

  REQUIRE(remove_then_downgrade.size() == 1);
  REQUIRE(remove_then_downgrade[0].type == PendingAction::DOWNGRADE);
  REQUIRE(remove_then_downgrade[0].nevra == older.nevra);
  REQUIRE(remove_then_downgrade[0].package_key == older.name_arch_key());
}

// -----------------------------------------------------------------------------
// Verify that downgrade and reinstall replace each other for one package name and architecture.
// -----------------------------------------------------------------------------
TEST_CASE("Pending transaction downgrade and reinstall replacement is order independent")
{
  reset_backend_globals();

  PackageRow installed = make_test_package_row("demo-2.0-1.x86_64", "demo", "2.0", "1", "x86_64");
  PackageRow older = make_test_package_row("demo-1.0-1.x86_64", "demo", "1.0", "1", "x86_64");

  dnf_backend_testonly_replace_installed_snapshot_rows({ installed });

  PendingTransactionActionRows downgrade_rows = pending_transaction_action_rows_for_selection(older, nullptr, 0, false);
  PendingTransactionActionRows installed_rows =
      pending_transaction_action_rows_for_selection(installed, nullptr, 0, false);

  std::vector<PendingAction> downgrade_then_reinstall;
  REQUIRE(pending_transaction_mark_install_side_action(downgrade_then_reinstall, downgrade_rows));
  REQUIRE(pending_transaction_mark_reinstall_action(downgrade_then_reinstall, installed_rows));

  REQUIRE(downgrade_then_reinstall.size() == 1);
  REQUIRE(downgrade_then_reinstall[0].type == PendingAction::REINSTALL);
  REQUIRE(downgrade_then_reinstall[0].nevra == installed.nevra);
  REQUIRE(downgrade_then_reinstall[0].package_key == installed.name_arch_key());

  std::vector<PendingAction> reinstall_then_downgrade;
  REQUIRE(pending_transaction_mark_reinstall_action(reinstall_then_downgrade, installed_rows));
  REQUIRE(pending_transaction_mark_install_side_action(reinstall_then_downgrade, downgrade_rows));

  REQUIRE(reinstall_then_downgrade.size() == 1);
  REQUIRE(reinstall_then_downgrade[0].type == PendingAction::DOWNGRADE);
  REQUIRE(reinstall_then_downgrade[0].nevra == older.nevra);
  REQUIRE(reinstall_then_downgrade[0].package_key == older.name_arch_key());
}

// -----------------------------------------------------------------------------
// Verify that bulk marking only queues visible upgrade candidates.
// -----------------------------------------------------------------------------
TEST_CASE("Pending transaction bulk upgrade marking ignores non upgrade rows")
{
  reset_backend_globals();

  PackageRow installed = make_test_package_row("demo-1.0-1.x86_64", "demo", "1.0", "1", "x86_64");
  PackageRow intermediate = make_test_package_row("demo-1.5-1.x86_64", "demo", "1.5", "1", "x86_64");
  PackageRow update = make_test_package_row("demo-2.0-1.x86_64", "demo", "2.0", "1", "x86_64");
  PackageRow downgrade = make_test_package_row("demo-0.9-1.x86_64", "demo", "0.9", "1", "x86_64");
  PackageRow available = make_test_package_row("other-1.0-1.x86_64", "other", "1.0", "1", "x86_64");
  update.is_newest_available = true;

  dnf_backend_testonly_replace_installed_snapshot_rows({ installed });

  std::vector<PendingAction> actions;
  std::set<std::string> marked_package_keys;

  PendingTransactionActionRows update_rows = pending_transaction_action_rows_for_selection(update, nullptr, 0, false);
  PendingTransactionActionRows intermediate_rows =
      pending_transaction_action_rows_for_selection(intermediate, nullptr, 0, false);
  PendingTransactionActionRows downgrade_rows =
      pending_transaction_action_rows_for_selection(downgrade, nullptr, 0, false);
  PendingTransactionActionRows available_rows =
      pending_transaction_action_rows_for_selection(available, nullptr, 0, false);

  REQUIRE(pending_transaction_mark_unique_upgrade_action(actions, marked_package_keys, update, update_rows, false));
  REQUIRE_FALSE(pending_transaction_mark_unique_upgrade_action(
      actions, marked_package_keys, intermediate, intermediate_rows, false));
  REQUIRE_FALSE(
      pending_transaction_mark_unique_upgrade_action(actions, marked_package_keys, downgrade, downgrade_rows, false));
  REQUIRE_FALSE(
      pending_transaction_mark_unique_upgrade_action(actions, marked_package_keys, available, available_rows, false));

  REQUIRE(actions.size() == 1);
  REQUIRE(actions[0].type == PendingAction::UPGRADE);
  REQUIRE(actions[0].nevra == update.nevra);
  REQUIRE(actions[0].transaction_spec == "demo.x86_64");
}

// -----------------------------------------------------------------------------
// Verify that bulk marking replaces stale pending actions for the same package.
// -----------------------------------------------------------------------------
TEST_CASE("Pending transaction bulk upgrade marking replaces existing package action")
{
  reset_backend_globals();

  PackageRow installed = make_test_package_row("demo-1.0-1.x86_64", "demo", "1.0", "1", "x86_64");
  PackageRow update = make_test_package_row("demo-2.0-1.x86_64", "demo", "2.0", "1", "x86_64");
  update.is_newest_available = true;

  dnf_backend_testonly_replace_installed_snapshot_rows({ installed });

  std::vector<PendingAction> actions = {
    { PendingAction::REMOVE, installed.nevra, installed.nevra, installed.name_arch_key() },
  };
  std::set<std::string> marked_package_keys;

  PendingTransactionActionRows rows = pending_transaction_action_rows_for_selection(update, nullptr, 0, false);

  REQUIRE(pending_transaction_mark_unique_upgrade_action(actions, marked_package_keys, update, rows, false));

  REQUIRE(actions.size() == 1);
  REQUIRE(actions[0].type == PendingAction::UPGRADE);
  REQUIRE(actions[0].nevra == update.nevra);
  REQUIRE(actions[0].transaction_spec == "demo.x86_64");
}

// -----------------------------------------------------------------------------
// Verify that upgrade marking replaces an older upgrade candidate after metadata changes.
// -----------------------------------------------------------------------------
TEST_CASE("Pending transaction upgrade marking replaces stale upgrade candidate")
{
  reset_backend_globals();

  PackageRow installed = make_test_package_row("demo-1.0-1.x86_64", "demo", "1.0", "1", "x86_64");
  PackageRow old_update = make_test_package_row("demo-2.0-1.x86_64", "demo", "2.0", "1", "x86_64");
  PackageRow new_update = make_test_package_row("demo-2.1-1.x86_64", "demo", "2.1", "1", "x86_64");
  new_update.is_newest_available = true;

  dnf_backend_testonly_replace_installed_snapshot_rows({ installed });

  std::vector<PendingAction> actions = {
    { PendingAction::UPGRADE, old_update.nevra, "demo.x86_64" },
  };

  PendingTransactionActionRows rows = pending_transaction_action_rows_for_selection(new_update, nullptr, 0, false);

  REQUIRE(pending_transaction_mark_install_side_action(actions, rows));

  REQUIRE(actions.size() == 1);
  REQUIRE(actions[0].type == PendingAction::UPGRADE);
  REQUIRE(actions[0].nevra == new_update.nevra);
  REQUIRE(actions[0].transaction_spec == "demo.x86_64");
}

// -----------------------------------------------------------------------------
// Verify that bulk marking counts one package identity once.
// -----------------------------------------------------------------------------
TEST_CASE("Pending transaction bulk upgrade marking deduplicates package identity")
{
  reset_backend_globals();

  PackageRow installed = make_test_package_row("demo-1.0-1.x86_64", "demo", "1.0", "1", "x86_64");
  installed.repo_candidate_relation = PackageRepoCandidateRelation::NEWER;
  installed.repo_candidate_nevra = "demo-2.0-1.x86_64";
  installed.repo_candidate_is_newest_available = true;

  PackageRow update = make_test_package_row("demo-2.0-1.x86_64", "demo", "2.0", "1", "x86_64");
  update.is_newest_available = true;

  dnf_backend_testonly_replace_installed_snapshot_rows({ installed });

  PendingTransactionActionRows installed_rows =
      pending_transaction_action_rows_for_selection(installed, nullptr, 0, false);
  PendingTransactionActionRows update_rows = pending_transaction_action_rows_for_selection(update, nullptr, 0, false);

  std::vector<PendingAction> actions;
  std::set<std::string> marked_package_keys;
  size_t marked_count = 0;

  if (pending_transaction_mark_unique_upgrade_action(actions, marked_package_keys, installed, installed_rows, true)) {
    ++marked_count;
  }
  if (pending_transaction_mark_unique_upgrade_action(actions, marked_package_keys, update, update_rows, true)) {
    ++marked_count;
  }

  REQUIRE(marked_count == 1);
  REQUIRE(marked_package_keys.size() == 1);
  REQUIRE(actions.size() == 1);
  REQUIRE(actions[0].type == PendingAction::UPGRADE);
  REQUIRE(actions[0].nevra == update.nevra);
  REQUIRE(actions[0].transaction_spec == "demo.x86_64");
  REQUIRE(actions[0].package_key == installed.name_arch_key());
}

// -----------------------------------------------------------------------------
// Verify that all-version bulk marking only uses the exact available upgrade row.
// -----------------------------------------------------------------------------
TEST_CASE("Pending transaction all-version bulk upgrade marking uses exact row")
{
  reset_backend_globals();

  PackageRow installed = make_test_package_row("demo-1.0-1.x86_64", "demo", "1.0", "1", "x86_64");
  installed.repo_candidate_relation = PackageRepoCandidateRelation::NEWER;
  installed.repo_candidate_nevra = "demo-2.0-1.x86_64";
  installed.repo_candidate_is_newest_available = true;

  PackageRow update = make_test_package_row("demo-2.0-1.x86_64", "demo", "2.0", "1", "x86_64");
  update.is_newest_available = true;

  dnf_backend_testonly_replace_installed_snapshot_rows({ installed });

  PendingTransactionActionRows installed_rows =
      pending_transaction_action_rows_for_selection(installed, nullptr, 0, false);
  PendingTransactionActionRows update_rows = pending_transaction_action_rows_for_selection(update, nullptr, 0, false);

  std::vector<PendingAction> actions;
  std::set<std::string> marked_package_keys;
  size_t marked_count = 0;

  if (pending_transaction_mark_unique_upgrade_action(actions, marked_package_keys, installed, installed_rows, false)) {
    ++marked_count;
  }
  if (pending_transaction_mark_unique_upgrade_action(actions, marked_package_keys, update, update_rows, false)) {
    ++marked_count;
  }

  REQUIRE(marked_count == 1);
  REQUIRE(marked_package_keys.size() == 1);
  REQUIRE(actions.size() == 1);
  REQUIRE(actions[0].type == PendingAction::UPGRADE);
  REQUIRE(actions[0].nevra == update.nevra);
  REQUIRE(actions[0].transaction_spec == "demo.x86_64");
}

// -----------------------------------------------------------------------------
// Verify that all-version bulk marking does not process the installed row for a pending upgrade.
// -----------------------------------------------------------------------------
TEST_CASE("Pending transaction all-version bulk marking keeps pending upgrade on exact row")
{
  reset_backend_globals();

  PackageRow installed = make_test_package_row("demo-1.0-1.x86_64", "demo", "1.0", "1", "x86_64");
  installed.repo_candidate_relation = PackageRepoCandidateRelation::NEWER;
  installed.repo_candidate_nevra = "demo-2.0-1.x86_64";
  installed.repo_candidate_is_newest_available = true;

  PackageRow update = make_test_package_row("demo-2.0-1.x86_64", "demo", "2.0", "1", "x86_64");
  update.is_newest_available = true;

  dnf_backend_testonly_replace_installed_snapshot_rows({ installed });

  std::vector<PendingAction> actions = {
    { PendingAction::UPGRADE, update.nevra, "demo.x86_64", update.name_arch_key() },
  };
  std::set<std::string> marked_package_keys;
  size_t marked_count = 0;

  PendingTransactionActionRows installed_rows =
      pending_transaction_action_rows_for_selection_with_pending(installed, nullptr, 0, false, actions);
  PendingTransactionActionRows update_rows =
      pending_transaction_action_rows_for_selection_with_pending(update, nullptr, 0, false, actions);

  REQUIRE(installed_rows.install_is_upgrade);
  REQUIRE_FALSE(installed_rows.install_action_from_pending);
  REQUIRE(update_rows.install_is_upgrade);
  REQUIRE_FALSE(update_rows.install_action_from_pending);

  if (pending_transaction_mark_unique_upgrade_action(actions, marked_package_keys, installed, installed_rows, false)) {
    ++marked_count;
  }
  if (pending_transaction_mark_unique_upgrade_action(actions, marked_package_keys, update, update_rows, false)) {
    ++marked_count;
  }

  REQUIRE(marked_count == 1);
  REQUIRE(marked_package_keys.size() == 1);
  REQUIRE(actions.size() == 1);
  REQUIRE(actions[0].type == PendingAction::UPGRADE);
  REQUIRE(actions[0].nevra == update.nevra);
  REQUIRE(actions[0].transaction_spec == "demo.x86_64");
}

// -----------------------------------------------------------------------------
// Verify that all-version bulk marking installs the newest installonly update exactly.
// -----------------------------------------------------------------------------
TEST_CASE("Pending transaction all-version bulk marking uses exact installonly update")
{
  reset_backend_globals();

  PackageRow installed =
      make_test_package_row("installonly-demo-1.0-1.x86_64", "installonly-demo", "1.0", "1", "x86_64");
  installed.installonly = true;
  installed.repo_candidate_relation = PackageRepoCandidateRelation::NEWER;
  installed.repo_candidate_nevra = "installonly-demo-2.0-1.x86_64";
  installed.repo_candidate_is_newest_available = true;

  PackageRow older = make_test_package_row("installonly-demo-0.9-1.x86_64", "installonly-demo", "0.9", "1", "x86_64");
  PackageRow update = make_test_package_row("installonly-demo-2.0-1.x86_64", "installonly-demo", "2.0", "1", "x86_64");
  older.installonly = true;
  update.installonly = true;
  update.is_newest_available = true;

  dnf_backend_testonly_replace_installed_snapshot_rows({ installed });

  PendingTransactionActionRows installed_rows =
      pending_transaction_action_rows_for_selection(installed, nullptr, 0, true);
  PendingTransactionActionRows older_rows = pending_transaction_action_rows_for_selection(older, nullptr, 0, true);
  PendingTransactionActionRows update_rows = pending_transaction_action_rows_for_selection(update, nullptr, 0, true);

  std::vector<PendingAction> actions;
  std::set<std::string> marked_package_keys;
  size_t marked_count = 0;

  if (pending_transaction_mark_unique_upgrade_action(actions, marked_package_keys, installed, installed_rows, false)) {
    ++marked_count;
  }
  if (pending_transaction_mark_unique_upgrade_action(actions, marked_package_keys, older, older_rows, false)) {
    ++marked_count;
  }
  if (pending_transaction_mark_unique_upgrade_action(actions, marked_package_keys, update, update_rows, false)) {
    ++marked_count;
  }

  REQUIRE(marked_count == 1);
  REQUIRE(marked_package_keys.size() == 1);
  REQUIRE(actions.size() == 1);
  REQUIRE(actions[0].type == PendingAction::INSTALL);
  REQUIRE(actions[0].nevra == update.nevra);
  REQUIRE(actions[0].transaction_spec == update.nevra);
  REQUIRE(actions[0].installonly);
}

// -----------------------------------------------------------------------------
// Verify that List Installed bulk marking can use the installed row's upgrade target.
// -----------------------------------------------------------------------------
TEST_CASE("Pending transaction list installed bulk upgrade marking uses installed row")
{
  reset_backend_globals();

  PackageRow installed = make_test_package_row("demo-1.0-1.x86_64", "demo", "1.0", "1", "x86_64");
  installed.repo_candidate_relation = PackageRepoCandidateRelation::NEWER;
  installed.repo_candidate_nevra = "demo-2.0-1.x86_64";
  installed.repo_candidate_is_newest_available = true;

  dnf_backend_testonly_replace_installed_snapshot_rows({ installed });

  PendingTransactionActionRows rows = pending_transaction_action_rows_for_selection(installed, nullptr, 0, false);

  std::vector<PendingAction> actions;
  std::set<std::string> marked_package_keys;

  REQUIRE(pending_transaction_mark_unique_upgrade_action(actions, marked_package_keys, installed, rows, true));

  REQUIRE(actions.size() == 1);
  REQUIRE(actions[0].type == PendingAction::UPGRADE);
  REQUIRE(actions[0].nevra == installed.repo_candidate_nevra);
  REQUIRE(actions[0].transaction_spec == "demo.x86_64");
  REQUIRE(actions[0].package_key == installed.name_arch_key());
}

// -----------------------------------------------------------------------------
// Verify that local-only installed packages cannot be reinstalled from repositories.
// -----------------------------------------------------------------------------
TEST_CASE("Pending transaction action rows reject reinstall for local only installed package")
{
  reset_backend_globals();

  PackageRow installed = make_test_package_row("demo-1.0-1.x86_64", "demo", "1.0", "1", "x86_64");
  installed.repo_candidate_relation = PackageRepoCandidateRelation::NONE;

  dnf_backend_testonly_replace_installed_snapshot_rows({ installed });

  PendingTransactionActionRows rows = pending_transaction_action_rows_for_selection(installed, nullptr, 0, false);

  REQUIRE(rows.state == PackageInstallState::LOCAL_ONLY);
  REQUIRE_FALSE(rows.has_install_row);
  REQUIRE(rows.has_installed_row);
  REQUIRE(rows.installed_row.nevra == installed.nevra);
  REQUIRE_FALSE(rows.exact_reinstall_available);
}

// -----------------------------------------------------------------------------
// Verify that reinstall is offered when the exact installed NEVRA is available.
// -----------------------------------------------------------------------------
TEST_CASE("Pending transaction action rows allow reinstall for exact available installed package")
{
  reset_backend_globals();

  PackageRow installed = make_test_package_row("demo-1.0-1.x86_64", "demo", "1.0", "1", "x86_64");
  installed.repo_candidate_relation = PackageRepoCandidateRelation::SAME;
  installed.repo_candidate_nevra = installed.nevra;
  installed.repo_candidate_exact_available = true;

  dnf_backend_testonly_replace_installed_snapshot_rows({ installed });

  PendingTransactionActionRows rows = pending_transaction_action_rows_for_selection(installed, nullptr, 0, false);

  REQUIRE(rows.state == PackageInstallState::INSTALLED);
  REQUIRE(rows.has_installed_row);
  REQUIRE(rows.installed_row.nevra == installed.nevra);
  REQUIRE(rows.exact_reinstall_available);
}

// -----------------------------------------------------------------------------
// Verify that compact package rows can reinstall when the visible row is the repository copy.
// -----------------------------------------------------------------------------
TEST_CASE("Pending transaction action rows allow reinstall from exact repository row")
{
  reset_backend_globals();

  PackageRow installed = make_test_package_row("demo-1.0-1.x86_64", "demo", "1.0", "1", "x86_64");
  installed.repo_candidate_relation = PackageRepoCandidateRelation::SAME;
  installed.repo_candidate_nevra = installed.nevra;
  installed.repo_candidate_exact_available = true;

  PackageRow repo_row = installed;
  repo_row.repo = "updates";
  repo_row.repo_candidate_exact_available = false;
  repo_row.is_newest_available = true;

  dnf_backend_testonly_replace_installed_snapshot_rows({ installed });

  PendingTransactionActionRows rows = pending_transaction_action_rows_for_selection(repo_row, nullptr, 0, false);

  REQUIRE(rows.state == PackageInstallState::INSTALLED);
  REQUIRE(rows.has_installed_row);
  REQUIRE(rows.installed_row.nevra == installed.nevra);
  REQUIRE(rows.exact_reinstall_available);
}

// -----------------------------------------------------------------------------
// Verify that reinstall is not offered when only a newer repository candidate exists.
// -----------------------------------------------------------------------------
TEST_CASE("Pending transaction action rows reject reinstall without exact available package")
{
  reset_backend_globals();

  PackageRow installed = make_test_package_row("demo-1.0-1.x86_64", "demo", "1.0", "1", "x86_64");
  installed.repo_candidate_relation = PackageRepoCandidateRelation::NEWER;
  installed.repo_candidate_nevra = "demo-2.0-1.x86_64";
  installed.repo_candidate_is_newest_available = true;

  dnf_backend_testonly_replace_installed_snapshot_rows({ installed });

  PendingTransactionActionRows rows = pending_transaction_action_rows_for_selection(installed, nullptr, 0, false);

  REQUIRE(rows.state == PackageInstallState::UPGRADEABLE);
  REQUIRE(rows.has_installed_row);
  REQUIRE(rows.installed_row.nevra == installed.nevra);
  REQUIRE_FALSE(rows.exact_reinstall_available);
}

// -----------------------------------------------------------------------------
// Verify that parallel installed versions use each exact NEVRA's repository availability.
// -----------------------------------------------------------------------------
TEST_CASE("Pending transaction action rows isolate reinstall availability between installed versions")
{
  reset_backend_globals();

  PackageRow older = make_test_package_row("demo-1.0-1.x86_64", "demo", "1.0", "1", "x86_64");
  older.repo_candidate_relation = PackageRepoCandidateRelation::NEWER;
  older.repo_candidate_nevra = "demo-2.0-1.x86_64";
  older.repo_candidate_is_newest_available = true;

  PackageRow newer = make_test_package_row("demo-2.0-1.x86_64", "demo", "2.0", "1", "x86_64");
  newer.repo_candidate_relation = PackageRepoCandidateRelation::SAME;
  newer.repo_candidate_nevra = newer.nevra;
  newer.repo_candidate_exact_available = true;

  dnf_backend_testonly_replace_installed_snapshot_rows({ older, newer });

  PendingTransactionActionRows older_rows = pending_transaction_action_rows_for_selection(older, nullptr, 0, false);
  PendingTransactionActionRows newer_rows = pending_transaction_action_rows_for_selection(newer, nullptr, 0, false);

  REQUIRE(older_rows.state == PackageInstallState::INSTALLED);
  REQUIRE(older_rows.has_installed_row);
  REQUIRE(older_rows.installed_row.nevra == older.nevra);
  REQUIRE_FALSE(older_rows.exact_reinstall_available);

  REQUIRE(newer_rows.state == PackageInstallState::INSTALLED);
  REQUIRE(newer_rows.has_installed_row);
  REQUIRE(newer_rows.installed_row.nevra == newer.nevra);
  REQUIRE(newer_rows.exact_reinstall_available);
}

// -----------------------------------------------------------------------------
// Verify that older parallel installed versions can be reinstalled when their exact NEVRA is available.
// -----------------------------------------------------------------------------
TEST_CASE("Pending transaction action rows allow reinstall for older exact available installed version")
{
  reset_backend_globals();

  PackageRow older = make_test_package_row("demo-1.0-1.x86_64", "demo", "1.0", "1", "x86_64");
  older.repo_candidate_relation = PackageRepoCandidateRelation::NEWER;
  older.repo_candidate_nevra = "demo-2.0-1.x86_64";
  older.repo_candidate_is_newest_available = true;
  older.repo_candidate_exact_available = true;

  PackageRow newer = make_test_package_row("demo-2.0-1.x86_64", "demo", "2.0", "1", "x86_64");
  newer.repo_candidate_relation = PackageRepoCandidateRelation::SAME;
  newer.repo_candidate_nevra = newer.nevra;
  newer.repo_candidate_exact_available = true;

  dnf_backend_testonly_replace_installed_snapshot_rows({ older, newer });

  PendingTransactionActionRows older_rows = pending_transaction_action_rows_for_selection(older, nullptr, 0, false);

  REQUIRE(older_rows.state == PackageInstallState::INSTALLED);
  REQUIRE(older_rows.has_installed_row);
  REQUIRE(older_rows.installed_row.nevra == older.nevra);
  REQUIRE(older_rows.exact_reinstall_available);
}

// -----------------------------------------------------------------------------
// EOF
// -----------------------------------------------------------------------------
