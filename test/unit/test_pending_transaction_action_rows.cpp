// -----------------------------------------------------------------------------
// Pending transaction action row resolver tests
// Covers the package IDs used by pending install, upgrade, remove, and reinstall actions.
// -----------------------------------------------------------------------------
#include <catch2/catch_test_macros.hpp>

#include "test_utils.hpp"
#include "upgrade/daemon_upgrade_state.hpp"
#include "ui/transaction/pending_transaction_action_rows.hpp"

#include <optional>
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
// Build one daemon upgrade target for resolver tests.
// -----------------------------------------------------------------------------
static TransactionServiceUpgradeTarget
make_test_upgrade_target(const char *nevra,
                         const char *name,
                         const char *version,
                         const char *release,
                         const char *arch)
{
  TransactionServiceUpgradeTarget target;
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
// Verify that an available row with no installed match can only be installed.
// -----------------------------------------------------------------------------
TEST_CASE("Pending transaction action rows resolve plain available package")
{
  reset_backend_globals();

  PackageRow available = make_test_package_row("demo-1.0-1.x86_64", "demo", "1.0", "1", "x86_64");

  PendingTransactionActionRows rows = pending_transaction_action_rows_for_selection(available, nullptr, 0);

  REQUIRE(rows.state == PackageInstallState::AVAILABLE);
  REQUIRE_FALSE(rows.install_is_upgrade);
  REQUIRE(rows.has_install_row);
  REQUIRE(rows.install_row.nevra == available.nevra);
  REQUIRE_FALSE(rows.has_installed_row);
  REQUIRE_FALSE(rows.can_try_reinstall);
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

  dnf_backend_testonly_replace_installed_snapshot_rows({ installed });

  PendingTransactionActionRows rows = pending_transaction_action_rows_for_selection(installed, nullptr, 0);

  REQUIRE(rows.state == PackageInstallState::UPGRADEABLE);
  REQUIRE(rows.install_is_upgrade);
  REQUIRE(rows.has_install_row);
  REQUIRE(rows.install_row.nevra == installed.repo_candidate_nevra);
  REQUIRE(rows.upgrade_spec == "demo.x86_64");
  REQUIRE(rows.has_installed_row);
  REQUIRE(rows.installed_row.nevra == installed.nevra);
  REQUIRE(rows.can_try_reinstall);
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
}

// -----------------------------------------------------------------------------
// Verify that an update candidate upgrades itself but removes the installed row.
// -----------------------------------------------------------------------------
TEST_CASE("Pending transaction action rows resolve upgrade from available update row")
{
  reset_backend_globals();

  PackageRow installed = make_test_package_row("demo-1.0-1.x86_64", "demo", "1.0", "1", "x86_64");
  PackageRow update = make_test_package_row("demo-2.0-1.x86_64", "demo", "2.0", "1", "x86_64");

  dnf_backend_testonly_replace_installed_snapshot_rows({ installed });

  PendingTransactionActionRows rows = pending_transaction_action_rows_for_selection(update, nullptr, 0);

  REQUIRE(rows.state == PackageInstallState::UPGRADEABLE);
  REQUIRE(rows.install_is_upgrade);
  REQUIRE(rows.has_install_row);
  REQUIRE(rows.install_row.nevra == update.nevra);
  REQUIRE(rows.upgrade_spec == "demo.x86_64");
  REQUIRE(rows.has_installed_row);
  REQUIRE(rows.installed_row.nevra == installed.nevra);
  REQUIRE(rows.can_try_reinstall);
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
  TransactionServiceUpgradeTarget target = make_test_upgrade_target("demo-2.0-1.x86_64", "demo", "2.0", "1", "x86_64");

  dnf_backend_testonly_replace_installed_snapshot_rows({ installed });

  std::optional<DaemonUpgradeRefreshId> refresh_id = state.begin_refresh();
  REQUIRE(refresh_id.has_value());

  std::string error;
  REQUIRE(state.publish_success(refresh_id.value(), { target }, error));
  DaemonUpgradeSnapshot snapshot = state.snapshot();

  PendingTransactionActionRows rows =
      pending_transaction_action_rows_for_selection(update, &target, snapshot.generation);

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
  TransactionServiceUpgradeTarget target = make_test_upgrade_target("demo-2.0-1.x86_64", "demo", "2.0", "1", "x86_64");

  std::optional<DaemonUpgradeRefreshId> refresh_id = state.begin_refresh();
  REQUIRE(refresh_id.has_value());

  std::string error;
  REQUIRE(state.publish_success(refresh_id.value(), { target }, error));
  DaemonUpgradeSnapshot snapshot = state.snapshot();

  PendingTransactionActionRows rows =
      pending_transaction_action_rows_for_selection(update, &target, snapshot.generation);

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
  TransactionServiceUpgradeTarget target = make_test_upgrade_target("demo-2.0-1.x86_64", "demo", "2.0", "1", "x86_64");

  dnf_backend_testonly_replace_installed_snapshot_rows({ installed });

  std::optional<DaemonUpgradeRefreshId> refresh_id = state.begin_refresh();
  REQUIRE(refresh_id.has_value());

  std::string error;
  REQUIRE(state.publish_success(refresh_id.value(), { target }, error));
  DaemonUpgradeSnapshot snapshot = state.snapshot();
  state.mark_stale();

  PendingTransactionActionRows rows =
      pending_transaction_action_rows_for_selection(update, &target, snapshot.generation);

  REQUIRE(rows.state == PackageInstallState::UPGRADEABLE);
  REQUIRE(rows.install_is_upgrade);
  REQUIRE_FALSE(rows.has_install_row);

  std::vector<PendingAction> actions;
  REQUIRE_FALSE(pending_transaction_mark_upgrade_action_for_row(actions, update, &target, snapshot.generation));
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
  TransactionServiceUpgradeTarget target = make_test_upgrade_target("demo-2.0-1.x86_64", "demo", "2.0", "1", "x86_64");

  dnf_backend_testonly_replace_installed_snapshot_rows({ installed });

  std::optional<DaemonUpgradeRefreshId> refresh_id = state.begin_refresh();
  REQUIRE(refresh_id.has_value());

  std::string error;
  REQUIRE(state.publish_success(refresh_id.value(), { target }, error));
  DaemonUpgradeSnapshot snapshot = state.snapshot();

  std::vector<PendingAction> actions;
  REQUIRE(pending_transaction_mark_upgrade_action_for_row(actions, update_metadata, &target, snapshot.generation));

  REQUIRE(actions.size() == 1);
  REQUIRE(actions[0].type == PendingAction::UPGRADE);
  REQUIRE(actions[0].nevra == target.nevra);
  REQUIRE(actions[0].transaction_spec == target.upgrade_spec());
}

// -----------------------------------------------------------------------------
// Verify that bulk marking only queues visible upgrade candidates.
// -----------------------------------------------------------------------------
TEST_CASE("Pending transaction bulk upgrade marking ignores non upgrade rows")
{
  reset_backend_globals();

  PackageRow installed = make_test_package_row("demo-1.0-1.x86_64", "demo", "1.0", "1", "x86_64");
  PackageRow update = make_test_package_row("demo-2.0-1.x86_64", "demo", "2.0", "1", "x86_64");
  PackageRow available = make_test_package_row("other-1.0-1.x86_64", "other", "1.0", "1", "x86_64");

  dnf_backend_testonly_replace_installed_snapshot_rows({ installed });

  std::vector<PendingAction> actions;

  REQUIRE(pending_transaction_mark_upgrade_action_for_row(actions, update, nullptr, 0));
  REQUIRE_FALSE(pending_transaction_mark_upgrade_action_for_row(actions, available, nullptr, 0));

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

  dnf_backend_testonly_replace_installed_snapshot_rows({ installed });

  std::vector<PendingAction> actions = {
    { PendingAction::REMOVE, installed.nevra, installed.nevra },
  };

  REQUIRE(pending_transaction_mark_upgrade_action_for_row(actions, update, nullptr, 0));

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

  dnf_backend_testonly_replace_installed_snapshot_rows({ installed });

  std::vector<PendingAction> actions = {
    { PendingAction::UPGRADE, old_update.nevra, "demo.x86_64" },
  };

  REQUIRE(pending_transaction_mark_upgrade_action_for_row(actions, new_update, nullptr, 0));

  REQUIRE(actions.size() == 1);
  REQUIRE(actions[0].type == PendingAction::UPGRADE);
  REQUIRE(actions[0].nevra == new_update.nevra);
  REQUIRE(actions[0].transaction_spec == "demo.x86_64");
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

  PendingTransactionActionRows rows = pending_transaction_action_rows_for_selection(installed, nullptr, 0);

  REQUIRE(rows.state == PackageInstallState::LOCAL_ONLY);
  REQUIRE_FALSE(rows.has_install_row);
  REQUIRE(rows.has_installed_row);
  REQUIRE(rows.installed_row.nevra == installed.nevra);
  REQUIRE_FALSE(rows.can_try_reinstall);
}

// -----------------------------------------------------------------------------
// EOF
// -----------------------------------------------------------------------------
