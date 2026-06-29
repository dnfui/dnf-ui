// -----------------------------------------------------------------------------
// Pending transaction action row resolver tests
// Covers the package IDs used by pending install, upgrade, remove, and reinstall actions.
// -----------------------------------------------------------------------------
#include <catch2/catch_test_macros.hpp>

#include "test_utils.hpp"
#include "ui/transaction/pending_transaction_action_rows.hpp"

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
// Verify that an available row with no installed match can only be installed.
// -----------------------------------------------------------------------------
TEST_CASE("Pending transaction action rows resolve plain available package")
{
  reset_backend_globals();

  PackageRow available = make_test_package_row("demo-1.0-1.x86_64", "demo", "1.0", "1", "x86_64");

  PendingTransactionActionRows rows = pending_transaction_action_rows_for_selection(available);

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

  PendingTransactionActionRows rows = pending_transaction_action_rows_for_selection(installed);

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
// Verify that an update candidate upgrades itself but removes the installed row.
// -----------------------------------------------------------------------------
TEST_CASE("Pending transaction action rows resolve upgrade from available update row")
{
  reset_backend_globals();

  PackageRow installed = make_test_package_row("demo-1.0-1.x86_64", "demo", "1.0", "1", "x86_64");
  PackageRow update = make_test_package_row("demo-2.0-1.x86_64", "demo", "2.0", "1", "x86_64");

  dnf_backend_testonly_replace_installed_snapshot_rows({ installed });

  PendingTransactionActionRows rows = pending_transaction_action_rows_for_selection(update);

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
// Verify that local-only installed packages cannot be reinstalled from repositories.
// -----------------------------------------------------------------------------
TEST_CASE("Pending transaction action rows reject reinstall for local only installed package")
{
  reset_backend_globals();

  PackageRow installed = make_test_package_row("demo-1.0-1.x86_64", "demo", "1.0", "1", "x86_64");
  installed.repo_candidate_relation = PackageRepoCandidateRelation::NONE;

  dnf_backend_testonly_replace_installed_snapshot_rows({ installed });

  PendingTransactionActionRows rows = pending_transaction_action_rows_for_selection(installed);

  REQUIRE(rows.state == PackageInstallState::LOCAL_ONLY);
  REQUIRE_FALSE(rows.has_install_row);
  REQUIRE(rows.has_installed_row);
  REQUIRE(rows.installed_row.nevra == installed.nevra);
  REQUIRE_FALSE(rows.can_try_reinstall);
}

// -----------------------------------------------------------------------------
// EOF
// -----------------------------------------------------------------------------
